#!/usr/bin/env bash
# Structural-coverage measurement for the test suite. Modes:
#
#   tools/coverage.sh [auto]  — Squish Coco when installed AND licensed,
#                               otherwise gcov + mcdc (the free toolchain)
#   tools/coverage.sh gcov    — GCC --coverage build; lcov/genhtml HTML report
#                               with LINE and BRANCH coverage
#                               → coverage/gcov/index.html
#   tools/coverage.sh mcdc    — Clang 18 source-based coverage with MC/DC
#                               (-fcoverage-mcdc); llvm-cov HTML + console
#                               summary incl. the MC/DC column
#                               → coverage/mcdc/index.html
#   tools/coverage.sh coco    — Squish Coco (Qt Group, $COCO_DIR): csg++
#                               instrumented build incl. MC/DC
#                               → coverage/coco/index.html
#
# Notes on the free MC/DC path (mode mcdc):
#  * clang-18 cannot instrument MC/DC for decisions with more than 6
#    conditions ("unsupported MC/DC boolean expression") — the sources keep
#    every decision at ≤ 6 conditions (see the hasAny() keyword-group helpers
#    in EventInsight.cpp and signalAgainstPosition in MainWindow.cpp).
#  * "warning: N functions have mismatched data" is emitted by llvm-cov when
#    the test binaries are cross-referenced against the one merged profile:
#    a handful of identically-named symbols that every Qt Test executable
#    carries (its own main() etc.) have per-binary coverage records. Only
#    those functions are affected, and they lie outside the measured
#    domain+services scope below.
#
# Scope: coverage is reported for the domain + services sources (src/domain,
# src/services). The UI layer has no automated GUI tests yet — that gap is
# tracked in the traceability report, not hidden by excluding it silently.
set -euo pipefail

MODE="${1:-auto}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.10.2/gcc_64}"
JOBS="$(nproc)"
COCO_DIR="${COCO_DIR:-/opt/SquishCoco}"

# Coco is usable when the compiler wrapper exists and the license is valid.
coco_usable() {
    [ -x "$COCO_DIR/bin/csg++" ] && "$COCO_DIR/bin/cocolic" --check >/dev/null 2>&1
}

run_gcov() {
    local BUILD="$ROOT/build-cov-gcc"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="--coverage" -DCMAKE_EXE_LINKER_FLAGS="--coverage"
    cmake --build "$BUILD" -j"$JOBS"
    find "$BUILD" -name '*.gcda' -delete
    (cd "$BUILD" && ctest --output-on-failure)
    local OUT="$ROOT/coverage/gcov"
    mkdir -p "$OUT"
    lcov --capture --directory "$BUILD" --output-file "$OUT/coverage.info" \
        --rc branch_coverage=1 --ignore-errors mismatch,negative,gcov,unused \
        --include "$ROOT/src/domain/*" --include "$ROOT/src/services/*"
    genhtml "$OUT/coverage.info" --output-directory "$OUT" \
        --branch-coverage --title "TradingApp line/branch coverage"
    lcov --summary "$OUT/coverage.info" --rc branch_coverage=1
    echo "HTML: $OUT/index.html"
}

run_mcdc() {
    local BUILD="$ROOT/build-cov-mcdc"
    # C++-only project: set only the CXX compiler (a -DCMAKE_C_COMPILER would
    # draw a "Manually-specified variables were not used" warning).
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER=clang++-18 \
        -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc" \
        -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate"
    cmake --build "$BUILD" -j"$JOBS"
    local OUT="$ROOT/coverage/mcdc"
    mkdir -p "$OUT"
    rm -f "$OUT"/*.profraw "$OUT"/merged.profdata
    local exe
    for exe in "$BUILD"/tests/tst_*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        LLVM_PROFILE_FILE="$OUT/$(basename "$exe").profraw" "$exe" >/dev/null
    done
    llvm-profdata-18 merge -sparse "$OUT"/*.profraw -o "$OUT/merged.profdata"
    # llvm-cov takes the first binary positionally and the rest via -object.
    local BINS=()
    for exe in "$BUILD"/tests/tst_*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        if [ ${#BINS[@]} -eq 0 ]; then
            BINS+=("$exe")
        else
            BINS+=(-object "$exe")
        fi
    done
    local SOURCES=("$ROOT"/src/domain/*.cpp "$ROOT"/src/domain/*.h
        "$ROOT"/src/services/*.cpp "$ROOT"/src/services/*.h)
    llvm-cov-18 report "${BINS[@]}" -instr-profile "$OUT/merged.profdata" \
        --show-mcdc-summary "${SOURCES[@]}" | tee "$OUT/summary.txt"
    llvm-cov-18 show "${BINS[@]}" -instr-profile "$OUT/merged.profdata" \
        --show-mcdc --show-branches=count --format=html \
        --output-dir="$OUT" "${SOURCES[@]}"
    echo "HTML: $OUT/index.html   (MC/DC column in summary.txt and per-file views)"
}

run_coco() {
    # Squish Coco measures statement/decision/condition and true MC/DC and
    # feeds CocoAI test-case suggestions. NOTE: written per the Coco manual
    # but not yet runnable here (license expired at the time of writing) —
    # validate the cmcsexeimport/cmreport switches on the first licensed run.
    if ! coco_usable; then
        echo "Squish Coco not usable: $COCO_DIR/bin/csg++ missing or license invalid" >&2
        echo "(check: $COCO_DIR/bin/cocolic --check)" >&2
        exit 2
    fi
    local BUILD="$ROOT/build-cov-coco"
    # csg++ wraps g++; instrumentation only happens with --cs-on. Tests and UI
    # are excluded from instrumentation to match the gcov/mcdc report scope.
    local CSFLAGS="--cs-on --cs-mcdc --cs-mcc"
    CSFLAGS+=" --cs-exclude-path=$ROOT/tests --cs-exclude-path=$ROOT/src/ui"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER="$COCO_DIR/bin/csg++" \
        -DCMAKE_CXX_FLAGS="$CSFLAGS"
    cmake --build "$BUILD" -j"$JOBS"
    local OUT="$ROOT/coverage/coco"
    mkdir -p "$OUT"
    rm -f "$OUT"/merged.csmes
    # Each instrumented test writes <exe>.csexe next to the binary when run;
    # import every execution report into its <exe>.csmes, then merge.
    local exe
    for exe in "$BUILD"/tests/tst_*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        case "$exe" in *.csmes | *.csexe) continue ;; esac
        rm -f "$exe.csexe"
        "$exe" >/dev/null
        "$COCO_DIR/bin/cmcsexeimport" -m "$exe.csmes" -e "$exe.csexe" \
            -t "$(basename "$exe")"
    done
    "$COCO_DIR/bin/cmmerge" -o "$OUT/merged.csmes" "$BUILD"/tests/tst_*.csmes
    "$COCO_DIR/bin/cmreport" -m "$OUT/merged.csmes" --html="$OUT"
    echo "HTML: $OUT/index.html   (open $OUT/merged.csmes in coveragebrowser for MC/DC drill-down)"
}

case "$MODE" in
gcov) run_gcov ;;
mcdc) run_mcdc ;;
coco) run_coco ;;
auto)
    if coco_usable; then
        echo "auto: Squish Coco found at $COCO_DIR with a valid license — measuring with Coco"
        run_coco
    else
        echo "auto: Squish Coco unavailable ($COCO_DIR missing or license invalid) — measuring with gcov + clang-18 MC/DC"
        run_gcov
        run_mcdc
    fi
    ;;
*)
    echo "usage: $0 [auto|gcov|mcdc|coco]" >&2
    exit 2
    ;;
esac
