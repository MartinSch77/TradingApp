#!/usr/bin/env bash
# Dynamic runtime-error evidence: build the test suite with AddressSanitizer +
# UndefinedBehaviorSanitizer and run it. A clean run demonstrates the absence
# of out-of-bounds accesses, use-after-free and undefined behaviour ON THE
# EXECUTED PATHS (the test suite). This is EVIDENCE, not PROOF — see
# docs/verification.md for the honest discussion of sound analysis options
# (Astrée / Polyspace / TrustInSoft) for C++/Qt code.
#
# Usage: tools/sanitize.sh [asan-ubsan|valgrind]
set -euo pipefail

MODE="${1:-asan-ubsan}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.10.2/gcc_64}"

case "$MODE" in
asan-ubsan)
    BUILD="$ROOT/build-san"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
    cmake --build "$BUILD" -j"$(nproc)"
    # halt_on_error: any finding fails the run loudly instead of just logging.
    (cd "$BUILD" && ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        ctest --output-on-failure)
    echo "ASan+UBSan: all tests clean"
    ;;
valgrind)
    # Second, independent dynamic checker over the already-built plain tests.
    BUILD="$ROOT/build"
    FAIL=0
    for exe in "$BUILD"/tests/tst_*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        echo "=== valgrind $(basename "$exe") ==="
        valgrind --error-exitcode=1 --leak-check=no --quiet "$exe" >/dev/null || FAIL=1
    done
    [ $FAIL -eq 0 ] && echo "valgrind memcheck: all tests clean"
    exit $FAIL
    ;;
*)
    echo "usage: $0 [asan-ubsan|valgrind]" >&2
    exit 2
    ;;
esac
