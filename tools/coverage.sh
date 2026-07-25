#!/usr/bin/env bash
# Structural-coverage measurement for the test suite, two instrumentation modes:
#
#   tools/coverage.sh gcov    — GCC --coverage build; lcov/genhtml HTML report
#                               with LINE and BRANCH coverage
#                               → coverage/gcov/index.html
#   tools/coverage.sh mcdc    — Clang 18 source-based coverage with MC/DC
#                               (-fcoverage-mcdc); llvm-cov HTML + console
#                               summary incl. the MC/DC column
#                               → coverage/mcdc/index.html
#
# MC/DC tooling note: Squish Coco (Qt Group) is installed at /opt/SquishCoco
# and would measure MC/DC (and drive CocoAI test-case suggestions), but its
# license is currently expired (`cocolic --check`). Clang's MC/DC
# instrumentation is the free, supported path used here; switch to Coco by
# renewing the license and building with coveragescanner wrappers.
#
# Scope: coverage is reported for the domain + services sources (src/domain,
# src/services). The UI layer has no automated GUI tests yet — that gap is
# tracked in the traceability report, not hidden by excluding it silently.
set -euo pipefail

MODE="${1:-gcov}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.10.2/gcc_64}"
JOBS="$(nproc)"

case "$MODE" in
gcov)
    BUILD="$ROOT/build-cov-gcc"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="--coverage" -DCMAKE_EXE_LINKER_FLAGS="--coverage"
    cmake --build "$BUILD" -j"$JOBS"
    find "$BUILD" -name '*.gcda' -delete
    (cd "$BUILD" && ctest --output-on-failure)
    OUT="$ROOT/coverage/gcov"
    mkdir -p "$OUT"
    lcov --capture --directory "$BUILD" --output-file "$OUT/coverage.info" \
        --rc branch_coverage=1 --ignore-errors mismatch,negative,gcov,unused \
        --include "$ROOT/src/domain/*" --include "$ROOT/src/services/*"
    genhtml "$OUT/coverage.info" --output-directory "$OUT" \
        --branch-coverage --title "TradingApp line/branch coverage"
    lcov --summary "$OUT/coverage.info" --rc branch_coverage=1
    echo "HTML: $OUT/index.html"
    ;;

mcdc)
    BUILD="$ROOT/build-cov-mcdc"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
        -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc" \
        -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate"
    cmake --build "$BUILD" -j"$JOBS"
    OUT="$ROOT/coverage/mcdc"
    mkdir -p "$OUT"
    rm -f "$OUT"/*.profraw "$OUT"/merged.profdata
    for exe in "$BUILD"/tests/tst_*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        LLVM_PROFILE_FILE="$OUT/$(basename "$exe").profraw" "$exe" >/dev/null
    done
    llvm-profdata-18 merge -sparse "$OUT"/*.profraw -o "$OUT/merged.profdata"
    # llvm-cov takes the first binary positionally and the rest via -object.
    BINS=()
    for exe in "$BUILD"/tests/tst_*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        if [ ${#BINS[@]} -eq 0 ]; then
            BINS+=("$exe")
        else
            BINS+=(-object "$exe")
        fi
    done
    SOURCES=("$ROOT"/src/domain/*.cpp "$ROOT"/src/domain/*.h \
             "$ROOT"/src/services/*.cpp "$ROOT"/src/services/*.h)
    llvm-cov-18 report "${BINS[@]}" -instr-profile "$OUT/merged.profdata" \
        --show-mcdc-summary "${SOURCES[@]}" | tee "$OUT/summary.txt"
    llvm-cov-18 show "${BINS[@]}" -instr-profile "$OUT/merged.profdata" \
        --show-mcdc --show-branches=count --format=html \
        --output-dir="$OUT" "${SOURCES[@]}"
    echo "HTML: $OUT/index.html   (MC/DC column in summary.txt and per-file views)"
    ;;

*)
    echo "usage: $0 [gcov|mcdc]" >&2
    exit 2
    ;;
esac
