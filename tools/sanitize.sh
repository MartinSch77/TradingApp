#!/usr/bin/env bash
# Dynamic runtime-error evidence over the test suite, three independent
# checkers (LLVM/GCC sanitizers + valgrind):
#
#   asan-ubsan  GCC build with AddressSanitizer (incl. LeakSanitizer) +
#               UndefinedBehaviorSanitizer; halt_on_error makes any finding
#               fail the run loudly.
#   tsan        clang-18 build with ThreadSanitizer (data races, lock-order
#               inversions). Separate build tree: TSan cannot be combined
#               with ASan.
#   valgrind    memcheck over the plain build's tests with full leak search:
#               --leak-check=full --show-leak-kinds=all --track-origins=yes
#               --error-exitcode=1
#   all         run the three in sequence (build_all.sh sanitize stage)
#
# Every mode writes its raw output to analysis-results/sanitize-<mode>.raw.txt
# and a normalized findings file analysis-results/sanitize-<mode>.txt
# (file|line|severity|id|message, via tools/parse_sanitizer_log.py) that
# axivion/external_import.py brings onto the dashboard. A clean run leaves an
# empty findings file — the dashboard then shows nothing for that provider.
#
# A clean run demonstrates the absence of these error classes ON THE EXECUTED
# PATHS (the test suite). This is EVIDENCE, not PROOF — see docs/verification.md.
#
# Usage: tools/sanitize.sh [asan-ubsan|tsan|valgrind|all]
set -uo pipefail

MODE="${1:-all}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.10.2/gcc_64}"
JOBS="$(nproc)"
OUT="$ROOT/analysis-results"
mkdir -p "$OUT"

# normalize <mode>: raw log -> pipe-format findings file for the dashboard
normalize() {
    python3 "$ROOT/tools/parse_sanitizer_log.py" "$1" \
        "$OUT/sanitize-$1.raw.txt" "$OUT/sanitize-$1.txt" "$ROOT"
}

run_asan_ubsan() {
    local BUILD="$ROOT/build-san"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" &&
        cmake --build "$BUILD" -j"$JOBS" || return 1
    local rc=0
    (cd "$BUILD" && ASAN_OPTIONS=halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        ctest --output-on-failure --timeout 600) 2>&1 | tee "$OUT/sanitize-asan-ubsan.raw.txt" || rc=1
    normalize asan-ubsan
    [ $rc -eq 0 ] && echo "ASan+UBSan: all tests clean"
    return $rc
}

run_tsan() {
    local BUILD="$ROOT/build-san-tsan"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER=clang++-18 \
        -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" &&
        cmake --build "$BUILD" -j"$JOBS" || return 1
    local rc=0
    # Qt libraries are not TSan-instrumented — reports originating inside them
    # are unanalyzable false positives (see tools/tsan.supp), and worse: TSan
    # mis-models Qt-internal condition variables (QTestLib's watchdog thread),
    # reports "unlock of an unlocked mutex" in pthread_cond_wait and then
    # DEADLOCKS the test at exit (main + watchdog both stuck on futexes).
    # ignore_noninstrumented_modules=1 is the canonical fix for mixing TSan
    # with a non-TSan Qt; project code stays fully checked. The ctest timeout
    # is the belt-and-braces guard against any future hang; the explicit
    # symbolizer path gives file:line in reports (only -18 binaries exist).
    (cd "$BUILD" && TSAN_OPTIONS="exitcode=1:second_deadlock_stack=1:ignore_noninstrumented_modules=1:suppressions=$ROOT/tools/tsan.supp:external_symbolizer_path=/usr/bin/llvm-symbolizer-18" \
        ctest --output-on-failure --timeout 600) 2>&1 | tee "$OUT/sanitize-tsan.raw.txt" || rc=1
    normalize tsan
    [ $rc -eq 0 ] && echo "TSan: all tests clean"
    return $rc
}

run_valgrind() {
    # Second, independent dynamic checker over the already-built plain tests.
    local BUILD="$ROOT/build"
    local rc=0 exe
    : > "$OUT/sanitize-valgrind.raw.txt"
    for exe in "$BUILD"/tests/tst_*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        echo "=== valgrind $(basename "$exe") ===" | tee -a "$OUT/sanitize-valgrind.raw.txt"
        valgrind \
            --leak-check=full \
            --show-leak-kinds=all \
            --track-origins=yes \
            --error-exitcode=1 \
            --suppressions="$ROOT/tools/valgrind.supp" \
            "$exe" >/dev/null 2>>"$OUT/sanitize-valgrind.raw.txt" || rc=1
    done
    normalize valgrind
    [ $rc -eq 0 ] && echo "valgrind memcheck: all tests clean"
    return $rc
}

case "$MODE" in
asan-ubsan) run_asan_ubsan ;;
tsan) run_tsan ;;
valgrind) run_valgrind ;;
all)
    FAIL=0
    run_asan_ubsan || FAIL=1
    run_tsan || FAIL=1
    run_valgrind || FAIL=1
    exit $FAIL
    ;;
*)
    echo "usage: $0 [asan-ubsan|tsan|valgrind|all]" >&2
    exit 2
    ;;
esac
