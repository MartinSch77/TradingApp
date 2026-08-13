#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Run the Squish GUI suite — and never touch a real account doing it.
#
#   tools/squish_run.sh [build]                    # squish/suite_gui vs build/TradingApp
#   tools/squish_run.sh --list-cases               # the names --case accepts
#   tools/squish_run.sh --case tst_trade_panel_guards          # ONE case
#   tools/squish_run.sh --case tst_startup_is_simulation --case tst_trade_panel_guards
#   tools/squish_run.sh --skip-case tst_heavyweights_early_read  # all but one
#   tools/squish_run.sh build --squish-dir ~/squish-for-qt-9.2.2
#   tools/squish_run.sh --ai                       # opt into AI-assisted lookup
#
# The installation is found automatically when it is not given: SQUISH_DIR /
# SQUISH_PREFIX, then PATH, then ~/squish-for-qt-<version> (where the official
# installer puts it) and /opt/squish*, newest first. How to obtain and install
# Squish: docs/qt-tools.md.
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

# Both the build directory and the Squish installation are PARAMETERS: these tools
# live wherever their owner installed them, and a hard-coded path is a script that
# works on one machine.
#   tools/squish_run.sh [build-dir] [--squish-dir DIR] [--ai]
#                       [--case NAME]* [--skip-case NAME]* [--list-cases]
#   SQUISH_DIR / SQUISH_PREFIX do the same job from the environment.
BUILD_DIR="build"
SQUISH_ARG=""
SUITE_REL="$ROOT/squish/suite_gui"
LIST_CASES=0
# Selected test cases, by directory name. Repeatable, and --skip-case is its mirror. Without
# these a single failing case could only be re-run by driving squishrunner and squishserver
# by hand — which also loses the forced-SIMULATION environment this script sets, and that is
# the one thing about a GUI run nobody should have to remember.
CASE_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
    --squish-dir) SQUISH_ARG="${2:-}"; shift ;;
    --case) CASE_ARGS+=(--testcase "$SUITE_REL/${2:-}"); shift ;;
    --skip-case) CASE_ARGS+=(--skip-testcase "$SUITE_REL/${2:-}"); shift ;;
    --list-cases) LIST_CASES=1 ;;
    --ai) SQUISH_AI=1 ;;
    -h | --help)
        sed -n '2,24p' "$0"
        exit 0
        ;;
    -*)
        echo "unknown argument: $1" >&2
        exit 2
        ;;
    *) BUILD_DIR="$1" ;;
    esac
    shift
done
AUT="$ROOT/$BUILD_DIR/TradingApp"
SUITE="$SUITE_REL"

if [ "$LIST_CASES" = "1" ]; then
    # The names --case takes, so nobody has to guess them from the directory listing.
    for dir in "$SUITE"/tst_*; do
        [ -d "$dir" ] && basename "$dir"
    done
    exit 0
fi
RESULTS="$ROOT/test-results/squish"
SCRATCH="${TMPDIR:-/tmp}/tradingapp-squish"

# Where Squish is. In order: an explicit SQUISH_DIR/SQUISH_PREFIX, whatever is on
# PATH, then the places the official installer actually puts it — its default is
# ~/squish-for-qt-<version> in the user's home, NOT /opt. Newest version wins, so a
# machine with two installs uses the one that was installed last.
find_squish_dir() {
    if [ -n "$SQUISH_ARG" ]; then
        echo "$SQUISH_ARG"
        return
    fi
    if [ -n "${SQUISH_DIR:-${SQUISH_PREFIX:-}}" ]; then
        echo "${SQUISH_DIR:-$SQUISH_PREFIX}"
        return
    fi
    local candidate
    for candidate in $(ls -d "$HOME"/squish-for-qt-* /opt/squish* 2>/dev/null | sort -Vr); do
        if [ -x "$candidate/bin/squishrunner" ]; then
            echo "$candidate"
            return
        fi
    done
    echo "/opt/squish"
}

SQUISH_DIR="$(find_squish_dir)"
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

# Extra environment for the AUT: appended DIRECTLY to the suite's own envvars file
# (the one suite.conf's ENVVARS=envvars names), restored on exit via the trap below.
#
# CORRECTED 2026-08-13, after the ORIGINAL comment here (kept in git history) spent a
# session across four mechanisms concluding root cause was env-var delivery and left
# it unconfirmed. It was not delivery. Measured directly this session, with
# squishserver/squishrunner actually run and the AUT's live /proc/<pid>/environ read
# while a suite was executing:
#   * A brand-new marker var appended to squish/suite_gui/envvars WHILE squishserver
#     was already daemonized DID reach the AUT's real environment — confirmed via
#     /proc/<pid>/environ, not inferred from a side effect.
#   * COVERAGESCANNER_ARGS=--cs-exec=<path> (the exact value/shape coverage.sh uses,
#     embedded "=" and all) delivered the SAME way, on the ACTUAL Coco-instrumented
#     build-cov-coco-gui/TradingApp binary — confirmed present in its environment.
#   * The suite still passed 62/62 and still wrote NO .csexe. Delivery is not, and
#     was never, the blocker — the earlier investigation's own "TRADINGAPP_FORCE_
#     SIMULATION arrives" signal was a true confirmation of THIS mechanism working;
#     it was only invalid as a proxy for validating the OTHER three mechanisms tried.
#   * Root cause instead: Coco's CoverageScanner runtime writes its execution report
#     at exit() by default (Coco manual, "Control of execution report generation") —
#     but Squish does not let a GUI AUT exit() on its own; it terminates it. A GUI app
#     under Squish is exactly the "daemon which never terminates" case that manual
#     page names as needing --cs-dump-on-signal=<sig>. TESTED: instrumented with
#     --cs-dump-on-signal=SIGTERM and even sent a MANUAL `kill -TERM` to the live AUT
#     mid-suite — no .csexe, and the process did not even terminate, so something in
#     Squish's hooked/instrumented runtime is intercepting or swallowing SIGTERM
#     before Coco's own handler (or the default disposition) can act on it. SIGUSR1
#     (the Coco manual's own example) was not yet tried — the likelier candidate,
#     since nothing in Qt or Squish has an a priori reason to special-case it the way
#     SIGTERM plausibly is (a "clean shutdown" signal many frameworks intercept).
# Next step for whoever picks this up: rebuild build-cov-coco-gui with
# --cs-dump-on-signal=SIGUSR1 instead, repeat the manual `kill -SIGUSR1 <aut-pid>`
# test above, and if that ALSO does not dump, the question moves to Squish/froglogic
# support (does squishserver's teardown SIGKILL rather than SIGTERM the AUT, with no
# grace period at all) rather than anything further guessable from this side.
EXTRA_ENVVARS=""
if [ -n "${COVERAGESCANNER_ARGS:-}" ]; then
    EXTRA_ENVVARS="$SUITE/envvars"
    cp "$EXTRA_ENVVARS" "$EXTRA_ENVVARS.orig"
    echo "COVERAGESCANNER_ARGS=$COVERAGESCANNER_ARGS" >> "$EXTRA_ENVVARS"
    echo "coverage:  AUT will write $COVERAGESCANNER_ARGS"
fi
cleanup_envvars() {
    if [ -n "$EXTRA_ENVVARS" ] && [ -f "$EXTRA_ENVVARS.orig" ]; then
        mv "$EXTRA_ENVVARS.orig" "$EXTRA_ENVVARS"
    fi
}
trap cleanup_envvars EXIT

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
    "${CASE_ARGS[@]}" \
    --reportgen "junit,$RESULTS/squish-suite_gui.xml" \
    --reportgen "stdout" || RC=$?
"$server" --stop >/dev/null 2>&1 || true
cleanup_envvars

echo "JUnit XML: $RESULTS/squish-suite_gui.xml"
if [ "$RC" -ne 0 ]; then
    echo "Squish reported failures (rc=$RC)" >&2
fi
exit "$RC"
