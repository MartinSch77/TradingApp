#!/usr/bin/env bash
# Run the Axivion analysis for TradingApp (MISRA C++ 2023, Qt-Autosar rules,
# architecture checks) and upload the result to the dashboard.
#
# The Axivion Suite is LICENSE-BOUND and cannot be installed by ./setup.sh.
# When it is not present this script exits 3 = "stage skipped", which
# build_all.sh reports as `skipped` rather than `FAILED`, so a machine without
# an Axivion license still runs the whole rest of the pipeline green.
#
# Only one axivion_ci per project: concurrent runs (e.g. build_all.sh while a
# manual run is active) share build_axivion/ and delete each other's IR right
# before the dashboard upload. clean_all.sh during a run does the same.
#
# Usage: axivion/start_analysis.sh [axivion_ci args ...]
set -uo pipefail

EXIT_SKIPPED=3
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# --- locate the Suite ------------------------------------------------------
# No hardcoded paths: honour BAUHAUS_HOME / AXIVIONBASE, then the usual install
# locations, then PATH.
find_suite() {
    local c
    for c in \
        ${BAUHAUS_HOME:+"$BAUHAUS_HOME"} \
        ${AXIVIONBASE:+"$AXIVIONBASE/bauhaus-suite"} \
        "$HOME/bauhaus-suite" \
        /opt/bauhaus-suite \
        /usr/local/bauhaus-suite; do
        [ -x "$c/bin/axivion_ci" ] && {
            echo "$c"
            return 0
        }
    done
    # axivion_ci on PATH -> its ../.. is the suite root
    if command -v axivion_ci >/dev/null 2>&1; then
        c="$(dirname "$(dirname "$(command -v axivion_ci)")")"
        [ -x "$c/bin/axivion_ci" ] && {
            echo "$c"
            return 0
        }
    fi
    return 1
}

SUITE="$(find_suite)" || {
    echo "SKIPPED: the Axivion Suite is not installed (license-bound; ./setup.sh cannot install it)."
    echo "         Install it and set BAUHAUS_HOME, or put its bin/ on PATH."
    exit $EXIT_SKIPPED
}
echo "Axivion Suite: $SUITE"
export BAUHAUS_HOME="$SUITE"
export PATH="$SUITE/bin:$PATH"

# The Suite's own environment file, when the installation ships one.
# shellcheck source=/dev/null
[ -f "$SUITE/bauhaus-kshrc" ] && . "$SUITE/bauhaus-kshrc"

# --- single-run lock -------------------------------------------------------
exec 9>"${TMPDIR:-/tmp}/.axivion-TradingApp.lock"
if ! flock -n 9; then
    echo "another Axivion run for TradingApp is already active — aborting" >&2
    exit 1
fi

# --- dashboard credentials -------------------------------------------------
# You may put dashboard credentials inside such a guarded block:
if [ -z "${AXIVION_USERNAME:-}" ] && [ -z "${AXIVION_PASSWORD:-}" ] && [ -z "${AXIVION_PASSFILE:-}" ]; then
    export AXIVION_USERNAME=admin
    export AXIVION_PASSWORD=password
fi

export BAUHAUS_CONFIG="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

# Toolchain profile for the analysis was generated with:
#   "$SUITE/bin/gccsetup" --cc 'gcc ' --cxx 'g++ ' --config axivion/compiler_config.json

# --- Qt --------------------------------------------------------------------
# The Axivion cmake configure doesn't pass -DCMAKE_PREFIX_PATH, so find_package
# would fall back to a system Qt6 (which lacks Qt6Charts) and fail. Point it at
# the same kit the normal build uses; prepend so any existing value still wins.
QT_PREFIX="${QT_PREFIX:-$(ls -d "$HOME"/Qt/*/gcc_64 2>/dev/null | sort -V | tail -1)}"
if [ -z "${QT_PREFIX:-}" ] || [ ! -d "$QT_PREFIX" ]; then
    echo "SKIPPED: no Qt 6 kit found for the analysis build (looked for ~/Qt/*/gcc_64)." >&2
    echo "         Set QT_PREFIX, or run ./setup.sh install." >&2
    exit $EXIT_SKIPPED
fi
echo "Qt kit: $QT_PREFIX"
export CMAKE_PREFIX_PATH="$QT_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
# Consumed by the Frameworks-QtSupport rule in rule_config.json, which must not
# hardcode a path that only exists on one machine.
export AXIVION_QTDIR="$QT_PREFIX"

# --jobs (no N) = parallel analysis jobs, auto-sized: cores capped so each job
# keeps >=2 GB RAM (AXIVION_MIN_MEM_PER_CORE). Placed after "$@" so an explicit
# caller-supplied -j N still wins; the Ninja build phase is already parallel
# (native parallelization since Suite 7.11.5).
axivion_ci "$@" --jobs
