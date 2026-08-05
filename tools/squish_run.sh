#!/usr/bin/env bash
# Run the Squish GUI suite — and never touch a real account doing it.
#
#   tools/squish_run.sh [build]        # runs squish/suite_gui against build/TradingApp
#
# Licence-bound, so it follows this project's rule for such tools: when Squish is
# not installed or not licensed, the stage prints why and exits 3 ("skipped"). It is
# never a build gate; the missing licence is reported in the quality PDF instead.
#
# THE SAFETY PROPERTY, because it is the reason this script exists rather than a
# bare `squishrunner` call:
#
#   Every run is forced into SIMULATION. TRADINGAPP_FORCE_SIMULATION makes
#   Config::hasCredentials() answer false (src/services/Config.cpp), so the app has
#   no credentials, cannot be LIVE, and has no order path to the broker — whatever
#   apiKeyEtoro.json on this machine says. That is checked by a unit test
#   (TS-CFG-007) and asserted again from the outside by the suite's first test case,
#   which reads the mode badge. On top of it the run gets its own XDG_CONFIG_HOME,
#   so the developer's keys and bot books are not even visible.
#
# Results are written as JUnit XML next to the unit suite's, so tools like Qt Test
# Center ingest both from one place (tools/testcenter_upload.sh).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build}"
AUT="$ROOT/$BUILD_DIR/TradingApp"
SUITE="$ROOT/squish/suite_gui"
RESULTS="$ROOT/test-results/squish"
SCRATCH="${TMPDIR:-/tmp}/tradingapp-squish"

SQUISH_DIR="${SQUISH_DIR:-${SQUISH_PREFIX:-/opt/squish}}"
runner="$(command -v squishrunner || echo "$SQUISH_DIR/bin/squishrunner")"
server="$(command -v squishserver || echo "$SQUISH_DIR/bin/squishserver")"

if [ ! -x "$runner" ] || [ ! -x "$server" ]; then
    echo "SKIPPED: Squish not found (looked for squishrunner/squishserver on PATH and in $SQUISH_DIR/bin)"
    echo "         Licence-bound; ./setup.sh cannot install it. See todo.txt for what to do."
    exit 3
fi
if [ ! -x "$AUT" ]; then
    echo "no AUT at $AUT — build it first (./build_all.sh build)" >&2
    exit 1
fi

# A licence check that does not need a display: squishrunner --version fails when the
# licence is missing or expired.
if ! "$runner" --version >/dev/null 2>&1; then
    echo "SKIPPED: Squish is installed but not usable — licence missing or expired"
    echo "         (check: $runner --version). Never a build gate."
    exit 3
fi

mkdir -p "$RESULTS" "$SCRATCH/config"
# A run must not inherit the developer's account, books or model settings.
rm -rf "$SCRATCH/config"
mkdir -p "$SCRATCH/config"

export TRADINGAPP_FORCE_SIMULATION=1
export XDG_CONFIG_HOME="$SCRATCH/config"
export ETORO_MODE=demo
export ETORO_API_KEY=""
export ETORO_USER_KEY=""
export TRADINGAPP_BOT_AI=off
export TRADINGAPP_BOT_NET=off
# Offscreen unless the caller wants to watch: a GUI suite in CI has no display, and
# Squish drives an offscreen Qt app perfectly well.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

echo "== Squish ($("$runner" --version 2>&1 | head -1)) =="
echo "AUT:      $AUT"
echo "suite:    $SUITE"
echo "mode:     FORCED SIMULATION (TRADINGAPP_FORCE_SIMULATION=1, isolated XDG_CONFIG_HOME)"

"$server" --stop >/dev/null 2>&1 || true
"$server" --config addAUT TradingApp "$ROOT/$BUILD_DIR" >/dev/null 2>&1 || true
"$server" --daemon >/dev/null 2>&1 || {
    echo "SKIPPED: squishserver would not start — see its own output above"
    exit 3
}

# Squish's AI-assisted object lookup (Squish 8.x) can find a widget whose properties
# moved, instead of failing the step. Opt-in with SQUISH_AI=1 rather than on by
# default, for two reasons: the object map here addresses widgets by objectName, which
# a refactor does not silently break (that is what tools/check_object_names.py
# guards), so the AI has little to repair; and a lookup that "heals" a genuinely wrong
# name would hide exactly the breakage this suite exists to catch. The switch name
# below is from the documentation and is NOT verified against a licensed run — if this
# version rejects it, the run continues without it rather than failing.
AI_ARGS=()
if [ "${SQUISH_AI:-0}" = "1" ]; then
    if "$runner" --help 2>&1 | grep -qi "objectnotfounddebugging\|ai"; then
        AI_ARGS+=(--objectNotFoundDebugging=ai)
        echo "AI object lookup: enabled (SQUISH_AI=1)"
    else
        echo "AI object lookup: requested but this squishrunner does not offer it — continuing without"
    fi
fi

RC=0
"$runner" --testsuite "$SUITE" \
    "${AI_ARGS[@]}" \
    --reportgen "junit,$RESULTS/squish-suite_gui.xml" \
    --reportgen "stdout" || RC=$?
"$server" --stop >/dev/null 2>&1 || true

echo "JUnit XML: $RESULTS/squish-suite_gui.xml"
if [ "$RC" -ne 0 ]; then
    echo "Squish reported failures (rc=$RC)" >&2
fi
exit "$RC"
