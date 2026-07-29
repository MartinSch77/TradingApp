#!/usr/bin/env bash
# Build every artefact this project produces, in dependency order:
#
#   build      build/                    app executable + test binaries (Debug,
#                                        with compile_commands.json for the analyzers)
#   app        build/TradingApp          the app executable ONLY (convenience
#                                        stage, not part of the default run)
#   release    build-release/TradingApp  optimized RelWithDebInfo build for
#                                        daily use / profiling (extra stage;
#                                        the compute paths run 5–20× faster
#                                        than the Debug build)
#   test       test-results/             JUnit XML per Qt Test function (tools/run_tests.sh)
#   trace      docs/traceability.html    REQ <-> DES <-> TS <-> result matrix
#   docs       docs/html/                Doxygen + PlantUML (ships the trace matrix)
#   coverage   coverage/…                Squish Coco (incl. MC/DC) when installed+licensed,
#                                        else gcov line/branch + clang-18 MC/DC reports
#   analysis   analysis-results/         cppcheck + clang-tidy (+ clazy) logs + merged CSV
#   sanitize   analysis-results/         ASan+UBSan (GCC), TSan (clang) and valgrind
#                                        memcheck runs; normalized findings logs feed
#                                        the Axivion dashboard import
#   axivion    dashboard                 MISRA C++ 2023 + architecture analysis via
#                                        axivion_ci; imports the analysis/sanitize logs
#                                        (runs last so it picks up the fresh logs)
#
# A failing stage does not stop the later ones; the summary at the end lists
# every stage's result and the exit code is non-zero if anything failed.
#
# Usage: ./build_all.sh [stage ...]          default: all stages in the order above
#        ./build_all.sh --skip axivion       everything except a stage (repeatable);
#                                            the Axivion run is by far the slowest
#        QT_PREFIX=<qt-kit> ./build_all.sh   (default: ~/Qt/6.11.1/gcc_64)
#
# Counterpart: ./clean_all.sh removes everything these stages generate.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
export QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.11.1/gcc_64}"
JOBS="$(nproc)"

ALL_STAGES=(build test trace docs coverage analysis sanitize axivion)
EXTRA_STAGES=(app release) # selectable by name, not part of the default run

# A CMake build tree records the absolute source/binary paths it was generated
# with and refuses to be reused if either changed. This repository invites that
# clash: a checkout on a Windows drive is /mnt/c/…/TradingApp from here and
# C:\…\TradingApp from Windows, and both platforms default to build/. Detect the
# mismatch and start clean instead of dying with CMake's error.
reset_stale_cache() {
    local build="$1" cache="$1/CMakeCache.txt" home
    [ -f "$cache" ] || return 0
    home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" | head -1)"
    if [ -n "$home" ] && [ "$home" != "$ROOT" ]; then
        echo "discarding the build tree in $build - it was generated for source dir '$home'"
        echo "(a tree configured from Windows and one configured from Linux cannot be shared)"
        rm -rf "$build"
    fi
}

stage_build() {
    reset_stale_cache "$ROOT/build"
    cmake -S "$ROOT" -B "$ROOT/build" \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DTRADINGAPP_WARNINGS_AS_ERRORS=ON &&
        cmake --build "$ROOT/build" -j"$JOBS"
}

stage_app() {
    reset_stale_cache "$ROOT/build"
    cmake -S "$ROOT" -B "$ROOT/build" \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DTRADINGAPP_WARNINGS_AS_ERRORS=ON &&
        cmake --build "$ROOT/build" --target TradingApp -j"$JOBS"
}

stage_release() {
    # Optimized build for daily use and profiling. Frame pointers stay in so
    # perf/gperftools produce usable stacks (see tools/profile.sh).
    reset_stale_cache "$ROOT/build-release"
    cmake -S "$ROOT" -B "$ROOT/build-release" \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer" &&
        cmake --build "$ROOT/build-release" -j"$JOBS" &&
        echo "release binary: build-release/TradingApp"
}

stage_test() { "$ROOT/tools/run_tests.sh" build; }

stage_trace() { python3 "$ROOT/tools/trace_report.py"; }

stage_docs() { "$ROOT/tools/make_docs.sh"; }

stage_coverage() { "$ROOT/tools/coverage.sh" auto; }

stage_analysis() { "$ROOT/tools/static_analysis.sh" build; }

stage_sanitize() { "$ROOT/tools/sanitize.sh" all; }

stage_axivion() { "$ROOT/axivion/start_analysis.sh"; }

usage() {
    echo "usage: $0 [stage ...] [--skip stage ...]   stages: ${ALL_STAGES[*]}   (default: all)"
    echo "       extra stages (only when named): ${EXTRA_STAGES[*]}"
    echo "       $0 app             builds only the TradingApp executable"
    echo "       $0 --skip axivion  everything except the (slow) Axivion analysis"
}

STAGES=()
SKIP=()
while [ $# -gt 0 ]; do
    case "$1" in
    --skip)
        shift
        [ $# -gt 0 ] || { usage; exit 2; }
        SKIP+=("$1")
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        STAGES+=("$1")
        ;;
    esac
    shift
done
if [ ${#STAGES[@]} -eq 0 ]; then
    STAGES=("${ALL_STAGES[@]}")
fi
for s in "${STAGES[@]}" ${SKIP[@]+"${SKIP[@]}"}; do
    case " ${ALL_STAGES[*]} ${EXTRA_STAGES[*]} " in
    *" $s "*) ;;
    *)
        echo "unknown stage: $s" >&2
        usage
        exit 2
        ;;
    esac
done
if [ ${#SKIP[@]} -gt 0 ]; then
    FILTERED=()
    for s in "${STAGES[@]}"; do
        case " ${SKIP[*]} " in
        *" $s "*) ;;
        *) FILTERED+=("$s") ;;
        esac
    done
    STAGES=(${FILTERED[@]+"${FILTERED[@]}"})
fi

# Stage outcomes are tri-state. Exit code 3 means "skipped": the stage needs a
# tool that is license-bound (Axivion Suite, Squish Coco) or otherwise absent,
# and could not run. That is reported as `skipped`, NOT as a failure, so the
# pipeline stays green on a machine without those licenses. Any other non-zero
# code is a real failure.
EXIT_SKIPPED=3
declare -A RESULT
FAIL=0
SKIPPED=0
for s in "${STAGES[@]}"; do
    echo
    echo "==================== $s ===================="
    "stage_$s"
    rc=$?
    case $rc in
    0) RESULT[$s]=ok ;;
    $EXIT_SKIPPED)
        RESULT[$s]=skipped
        SKIPPED=$((SKIPPED + 1))
        ;;
    *)
        RESULT[$s]=FAILED
        FAIL=1
        ;;
    esac
done

echo
echo "==================== summary ===================="
for s in "${STAGES[@]}"; do
    printf '  %-10s %s\n' "$s" "${RESULT[$s]}"
done
[ $SKIPPED -gt 0 ] && echo "  ($SKIPPED stage(s) skipped — a required tool is unavailable; see the log above)"
exit $FAIL
