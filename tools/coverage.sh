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
#   tools/coverage.sh coco-ai — CocoAI's test-case SUGGESTIONS over the coverage
#                               database the coco mode produced (newer Coco
#                               releases only; skips when the installation has no
#                               AI tool)
#   tools/coverage.sh coco-components
#                             — the same instrumented libraries, but exercised by
#                               the COMPONENT/INTEGRATION tests only, reported per
#                               test case: which functions of each integrated
#                               component those tests actually reach ("call
#                               coverage" in the everyday sense — Coco's own name
#                               for it is function coverage, and the per-test
#                               attribution comes from cmcsexeimport -t)
#                               → coverage/coco-components/index.html
#
# Notes on the free MC/DC path (mode mcdc):
#  * clang-18 cannot instrument MC/DC for decisions with more than 6
#    conditions ("unsupported MC/DC boolean expression") — the sources keep
#    every decision at ≤ 6 conditions (see the hasAny() keyword-group helpers
#    in EventInsight.cpp and signalAgainstPosition in MainWindow.cpp).
#  * The clang VERSION is resolved, not hardcoded (llvm_suffix in
#    tools/common.sh): -fcoverage-mcdc needs >= 18, and a Debian or Raspberry Pi
#    OS release ships exactly one clang. Without a new enough one this mode
#    reports `skipped` (exit 3) instead of failing — gcov still measures line and
#    branch coverage.
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
. "$ROOT/tools/common.sh"
QT_PREFIX="${QT_PREFIX:-$(qt_prefix)}"
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
    # One matched LLVM installation for the compiler, the profile merge and the
    # report — see llvm_suffix() on why they must not be mixed. Exit 3 =
    # "skipped" (build_all.sh), because no clang >= 18 is a property of the
    # machine, not a defect in the code.
    local llvm
    if ! llvm="$(llvm_suffix 18)"; then
        echo "SKIPPED: no clang >= 18 on this host — MC/DC needs -fcoverage-mcdc (clang 18+)." >&2
        echo "         Install one (Debian/Ubuntu: apt-get install clang-18 llvm-18," >&2
        echo "         or a newer clang-NN + llvm-NN) — gcov line/branch coverage is unaffected." >&2
        return 3
    fi
    echo "MC/DC toolset: clang++$llvm ($("clang++$llvm" --version | head -1))"
    # C++-only project: set only the CXX compiler (a -DCMAKE_C_COMPILER would
    # draw a "Manually-specified variables were not used" warning).
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER="clang++$llvm" \
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
    "llvm-profdata$llvm" merge -sparse "$OUT"/*.profraw -o "$OUT/merged.profdata"
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
    "llvm-cov$llvm" report "${BINS[@]}" -instr-profile "$OUT/merged.profdata" \
        --show-mcdc-summary "${SOURCES[@]}" | tee "$OUT/summary.txt"
    "llvm-cov$llvm" show "${BINS[@]}" -instr-profile "$OUT/merged.profdata" \
        --show-mcdc --show-branches=count --format=html \
        --output-dir="$OUT" "${SOURCES[@]}"
    echo "HTML: $OUT/index.html   (MC/DC column in summary.txt and per-file views)"
}

# The tests that drive a COMPONENT through its real seams rather than a pure
# function: the broker client and the feeds against the in-process mock HTTP
# server, the advisors against a mocked endpoint, the simulated broker, the
# learning loop against the real trainer, and configuration against real files.
# Marked type "I" in docs/test_spec.md — this list is that column, in one place.
component_tests() {
    printf '%s\n' tst_etoroclient tst_marketfeeds tst_jsonhttp tst_simulationengine \
        tst_aiadvisor tst_ollamaadvisor tst_botnet tst_config tst_economiccalendar \
        tst_positionsmodel tst_tradescript
}

# What must NOT be instrumented, in both Coco modes. The test code is excluded so a
# function only counts when a COMPONENT executed it — and the Qt, libstdc++ and
# system headers because each translation unit instantiates those templates
# differently: cmmerge then reports "source file qmetatype.h is differently
# instrumented in the database" for every test binary and cmreport crashes on the
# result. The Windows counterpart learned that first (see tools/coverage.ps1); the
# same applies here.
coco_excludes() {
    local out=" --cs-exclude-path=$ROOT/tests"
    [ -n "${QT_PREFIX:-}" ] && out+=" --cs-exclude-path=$QT_PREFIX"
    out+=" --cs-exclude-file-abs-wildcard=*/usr/include/*"
    out+=" --cs-exclude-file-abs-wildcard=*/c++/*"
    printf '%s' "$out"
}

# Shared by both Coco modes: configure and build an instrumented tree.
#   $1 = build dir, $2 = extra --cs flags
# The configure step on its own, so the retry below can repeat it exactly.
coco_configure() {
    local build="$1"
    local csflags="$2"
    local ar_arg=()
    local coco_ar
    for coco_ar in "$COCO_DIR/bin/csar" "$COCO_DIR/bin/cslib"; do
        if [ -x "$coco_ar" ]; then
            ar_arg=("-DCMAKE_AR=$coco_ar")
            break
        fi
    done
    cmake -S "$ROOT" -B "$build" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_CXX_COMPILER="$COCO_DIR/bin/csg++" \
        "${ar_arg[@]}" \
        -DCMAKE_CXX_FLAGS="$csflags"
}

coco_build() {
    local build="$1"
    local csflags="$2"
    # Coco's front end parses up to C++20, so THIS build tree alone drops from 23 —
    # exactly as the Windows counterpart does (CMakeLists only defaults the standard
    # when it is not already defined). Without it the instrumenting compiler fails on
    # C++23 constructs before any measurement can happen.
    # Instrumented objects reference a per-TU coverage table that Coco's own
    # librarian emits; this project links domain/services/ui as STATIC libraries, so
    # archiving them with plain ar drops those tables and every test binary fails to
    # link. The Windows path sets CMAKE_AR=cslib for exactly this reason — use Coco's
    # wrapper here too when the installation ships one.
    coco_configure "$build" "$csflags"
    # csg++ MERGES instrumentation into an existing .csmes and refuses when the source
    # behind it changed ("Source file ... is different at line N", then the link fails
    # with error 255). That is a stale database, not a code problem — so a failed build
    # is retried ONCE with the databases removed, which is the documented remedy and
    # costs a re-instrumentation rather than a confusing red stage.
    if ! cmake --build "$build" -j"$JOBS"; then
        # The instrumentation state lives in BOTH the .csmes databases and the object
        # files that reference their symbols, so removing only the databases leaves a
        # tree that fails to LINK ("undefined reference to __cs_tb_…"). The remedy is
        # the whole tree: expensive, but it only happens after a failure, and a
        # confusing red stage costs more than a re-instrumentation.
        echo "coco: build failed — wiping the instrumented tree and retrying once"
        rm -rf "$build"
        coco_configure "$build" "$csflags"
        cmake --build "$build" -j"$JOBS"
    fi
}

# Run one instrumented test and import its execution report UNDER ITS OWN NAME: the
# -t name is what lets cmreport attribute coverage per test case, which is the whole
# point of the component report.
#
# VALIDATED on the first licensed run (2026-08-05, Coco Full Commercial): an
# instrumented binary writes <name>.csexe into its WORKING DIRECTORY, not next to the
# executable — the version of this function that assumed otherwise failed with
# "Cannot open CSExe file tst_aiadvisor.csexe for reading". Running each test from its
# own directory puts the report where the .csmes is and keeps the repository root
# clean.
coco_run_one() {
    local exe="$1"
    local dir
    dir="$(dirname "$exe")"
    local name
    name="$(basename "$exe")"
    rm -f "$dir/$name.csexe"
    (cd "$dir" && QT_QPA_PLATFORM=offscreen "./$name" >/dev/null 2>&1) || true
    if [ ! -f "$dir/$name.csexe" ]; then
        # Two of the twenty-four (tst_indicators, tst_models) do this consistently, and
        # the cause is NOT yet identified: the Coco runtime IS linked into them (82
        # symbols incl. __coveragescanner_filename), their .csmes is the usual ~2 MB,
        # they pass, and neither a COVERAGESCANNER_ARGS --cs-exec nor a command-line
        # one produces a report. Reported rather than hidden — and the functions they
        # cover are exercised by other suites in the same merge, so the numbers below
        # are a floor, not a fiction.
        echo "note: $name produced no execution report — skipped in the merge"
        return 0
    fi
    "$COCO_DIR/bin/cmcsexeimport" -m "$dir/$name.csmes" -e "$dir/$name.csexe" -t "$name"
}

# cmreport writes ONE output file per invocation: asking for --html and --csv-excel
# together fails with "Multiple output files defined" and produces neither. Measured on
# 7.2.0 against a licensed run — so each report gets its own call.
coco_report() {
    local csmes="$1" title="$2"
    shift 2
    local spec
    for spec in "$@"; do
        "$COCO_DIR/bin/cmreport" -m "$csmes" --title="$title" "$spec" ||
            echo "note: cmreport rejected $spec on this version — the other reports still exist"
    done
}

# Every coverage level the merged database can answer, on the standard output.
#
# `--stat` is how cmreport prints a number to the console; `--text=` writes a 0-byte
# file no matter which --section values it is given (measured on 7.2.0), so it is not
# used. The four levels are the reason Coco is in this pipeline at all: gcov reports
# lines, and a line-covered decision can still have untested condition combinations.
coco_stat_levels() {
    local csmes="$1"
    local json="${2:-}"
    local label value name first=1
    [ -n "$json" ] && printf '{\n' > "$json"
    for label in statement:--coverage-statement-block decision:--coverage-decision \
                 condition:--coverage-condition mcdc:--coverage-mcdc; do
        name="${label%%:*}"
        value="$("$COCO_DIR/bin/cmreport" -m "$csmes" "${label#*:}" --stat 2>/dev/null |
                 tr -d ' \n')"
        printf '  %-10s %s\n' "$name" "${value:-unavailable}"
        # …and the same numbers as JSON, because the quality PDF must be able to REPORT
        # Coco rather than merely note that a licence exists.
        if [ -n "$json" ] && [ -n "$value" ]; then
            local pct covered total
            pct="${value%%\%*}"
            covered="$(printf '%s' "$value" | sed -n 's/.*(\([0-9]*\)\/\([0-9]*\)).*/\1/p')"
            total="$(printf '%s' "$value" | sed -n 's/.*(\([0-9]*\)\/\([0-9]*\)).*/\2/p')"
            [ "$first" -eq 1 ] || printf ',\n' >> "$json"
            printf '  "%s": {"percent": %s, "covered": %s, "total": %s}' \
                "$name" "${pct:-0}" "${covered:-0}" "${total:-0}" >> "$json"
            first=0
        fi
    done
    if [ -n "$json" ]; then
        printf '\n}\n' >> "$json"
        echo "  summary:   $json"
    fi
}

# CocoAI turns an existing coverage database into suggestions for the tests that
# would close its biggest gaps. It ships only with newer Coco releases: this
# installation's bin/ holds cmcsexeimport, cmedit, cmmerge, cmreport and cmvs and no
# AI tool at all, so the mode looks for the candidates by name and says exactly what
# it looked for when it finds none. Nothing here is invented — when a release that
# has it is installed, the call below is the one its manual documents, and the first
# licensed run is the validation run (as for the rest of the Coco path).
run_coco_ai() {
    if ! coco_usable; then
        echo "SKIPPED: Squish Coco not usable — $COCO_DIR/bin/csg++ missing or license invalid"
        exit 3
    fi
    local db="$ROOT/coverage/coco/merged.csmes"
    if [ ! -f "$db" ]; then
        echo "no coverage database at $db — run tools/coverage.sh coco first" >&2
        exit 1
    fi
    local ai=""
    local candidate
    for candidate in cocoai coco-ai cmai cmsuggest; do
        if [ -x "$COCO_DIR/bin/$candidate" ]; then
            ai="$COCO_DIR/bin/$candidate"
            break
        fi
    done
    if [ -z "$ai" ]; then
        echo "SKIPPED: this Coco installation has no AI tool"
        echo "         (looked for cocoai, coco-ai, cmai, cmsuggest in $COCO_DIR/bin)"
        echo "         CocoAI ships with newer releases — see cocoSetupInstructions.txt."
        exit 3
    fi
    local out="$ROOT/coverage/coco/ai-suggestions.txt"
    echo "== CocoAI test-case suggestions =="
    "$ai" --csmes "$db" --output "$out" || {
        echo "SKIPPED: $ai rejected these arguments — the switch names differ on this"
        echo "         version; paste its --help output and they will be matched."
        exit 3
    }
    echo "suggestions: $out"
}

run_coco_components() {
    # Which functions of the integrated components (domain + services, built as the
    # static libraries the app links) the COMPONENT tests reach, per test.
    #
    # NOTE, as for run_coco: written from the Coco manual and never run against a
    # live licence here. The switches to confirm on the first licensed run are the
    # cmreport ones below; everything else is shared with the plain coco mode.
    if ! coco_usable; then
        echo "SKIPPED: Squish Coco not usable — $COCO_DIR/bin/csg++ missing or license invalid"
        echo "         (check: $COCO_DIR/bin/cocolic --check). License-bound; ./setup.sh cannot install it."
        exit 3
    fi
    local BUILD="$ROOT/build-cov-coco-components"
    # The COMPONENTS are instrumented; the test code itself is not, so a function
    # only counts as reached when the component executed it.
    local CSFLAGS="--cs-on --cs-mcdc --cs-mcc$(coco_excludes)"
    coco_build "$BUILD" "$CSFLAGS"
    local OUT="$ROOT/coverage/coco-components"
    mkdir -p "$OUT"
    rm -f "$OUT"/merged.csmes
    local name exe ran=0
    for name in $(component_tests); do
        exe="$BUILD/tests/$name"
        [ -x "$exe" ] || { echo "no such component test: $name (skipped)"; continue; }
        coco_run_one "$exe"
        ran=$((ran + 1))
    done
    if [ "$ran" -eq 0 ]; then
        echo "no component tests were built — nothing to measure" >&2
        exit 1
    fi
    "$COCO_DIR/bin/cmmerge" -o "$OUT/merged.csmes" "$BUILD"/tests/tst_*.csmes
    # Function-level HTML plus a CSV that can be diffed between runs. Switches
    # validated on the first licensed run: --html takes a FILE, --csv-excel is the CSV
    # switch (there is no plain --csv), and --junit lets the per-test attribution reach
    # Qt Test Center alongside the unit suite's own JUnit XML.
    coco_report "$OUT/merged.csmes" "TradingApp — component call coverage, per test case" \
        "--html=$OUT/index.html" \
        "--csv-excel=$OUT/functions.csv" \
        "--junit=$ROOT/test-results/coco-components.xml"
    echo "component call coverage over $ran component tests"
    coco_stat_levels "$OUT/merged.csmes" "$OUT/summary.json"
    echo "HTML: $OUT/index.html   (per-test attribution: open $OUT/merged.csmes in coveragebrowser and group by test case)"
}

run_coco() {
    # Squish Coco measures statement/decision/condition and true MC/DC and feeds
    # CocoAI test-case suggestions. Every switch below has been run against a licensed
    # Coco (7.2.0, Full Commercial) — see coco_run_one and coco_stat_levels for the two
    # things the manual does not say out loud.
    if ! coco_usable; then
        # Exit 3 = "stage skipped" (see build_all.sh). Coco is license-bound and
        # ./setup.sh cannot install it, so its absence must not fail the run.
        echo "SKIPPED: Squish Coco not usable — $COCO_DIR/bin/csg++ missing or license invalid"
        echo "         (check: $COCO_DIR/bin/cocolic --check). License-bound; ./setup.sh cannot install it."
        exit 3
    fi
    local BUILD="$ROOT/build-cov-coco"
    # csg++ wraps g++; instrumentation only happens with --cs-on. Tests and UI
    # are excluded from instrumentation to match the gcov/mcdc report scope.
    local CSFLAGS="--cs-on --cs-mcdc --cs-mcc$(coco_excludes)"
    CSFLAGS+=" --cs-exclude-path=$ROOT/src/ui"
    coco_build "$BUILD" "$CSFLAGS"
    local OUT="$ROOT/coverage/coco"
    mkdir -p "$OUT"
    rm -f "$OUT"/merged.csmes
    # Each instrumented test writes <exe>.csexe next to the binary when run;
    # import every execution report into its <exe>.csmes, then merge.
    local exe
    for exe in "$BUILD"/tests/tst_*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        case "$exe" in *.csmes | *.csexe) continue ;; esac
        coco_run_one "$exe"
    done
    "$COCO_DIR/bin/cmmerge" -o "$OUT/merged.csmes" "$BUILD"/tests/tst_*.csmes
    # --html takes a FILE name, not a directory (validated against cmreport --help on
    # the first licensed run — passing a directory silently produced nothing).
    coco_report "$OUT/merged.csmes" "TradingApp — statement/decision/condition + MC/DC" \
        "--html=$OUT/index.html" \
        "--csv-excel=$OUT/functions.csv"
    coco_stat_levels "$OUT/merged.csmes" "$OUT/summary.json"
    echo "HTML: $OUT/index.html   (open $OUT/merged.csmes in coveragebrowser for MC/DC drill-down)"
}

case "$MODE" in
gcov) run_gcov ;;
mcdc) run_mcdc ;;
coco) run_coco ;;
coco-components) run_coco_components ;;
coco-ai) run_coco_ai ;;
auto)
    # EVERY back end that is available runs — Coco is not a replacement for the free
    # toolchain, it is a second opinion. They measure different things and are read by
    # different consumers: gcov produces the line/branch numbers the coverage BADGE and
    # the quality PDF are built from, clang gives an independent MC/DC figure (it
    # instruments the IR where Coco instruments the source), and Coco adds
    # statement/decision/condition/MC/DC from a qualified tool. The earlier version of
    # this block ran Coco INSTEAD of the other two the moment a licence appeared, which
    # silently emptied coverage/gcov and coverage/mcdc and left the PDF reporting "no
    # coverage artefacts were produced" — a better tool must not remove evidence.
    # A back end that is absent says so and is skipped; the stage fails only when none
    # of them measured anything.
    measured=()
    rc=0
    run_gcov && measured+=(gcov) || echo "auto: gcov produced nothing — see the message above"
    run_mcdc || rc=$?
    case $rc in
    0) measured+=(mcdc) ;;
    3) echo "auto: clang MC/DC skipped (see above)" ;;
    *) echo "auto: clang MC/DC FAILED (rc=$rc)" ;;
    esac
    if coco_usable; then
        echo "auto: Squish Coco found at $COCO_DIR with a valid licence"
        run_coco && measured+=(coco)
    else
        echo "auto: Squish Coco unavailable ($COCO_DIR missing or licence invalid) — skipped"
    fi
    echo ""
    if [ "${#measured[@]}" -eq 0 ]; then
        echo "no coverage back end produced a report" >&2
        exit 1
    fi
    echo "coverage measured by: ${measured[*]}"
    ;;
*)
    echo "usage: $0 [auto|gcov|mcdc|coco]" >&2
    exit 2
    ;;
esac
