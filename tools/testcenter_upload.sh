#!/usr/bin/env bash
# Send EVERY test result this project produces to Squish Test Center — the
# unit/integration suite and the Squish GUI suite alike.
#
#   tools/testcenter_upload.sh                       # upload test-results/**.xml
#   tools/testcenter_upload.sh --dry-run             # list exactly what would be sent
#   tools/testcenter_upload.sh --testcenter-dir DIR  # a specific installation
#   tools/testcenter_upload.sh --url URL --project P --batch B
#
# Every path and every setting is a PARAMETER with an environment fallback, because
# these tools live wherever their owner installed them:
#   --testcenter-dir | TESTCENTER_DIR       installation directory (auto-discovered)
#   --url            | TESTCENTER_URL       default http://localhost:8800
#   --project        | TESTCENTER_PROJECT   default TradingApp
#   --batch          | TESTCENTER_BATCH     default: the short git sha
#   --token          | TESTCENTER_TOKEN     an access token from Test Center
#   --user/--password| TESTCENTER_USER / TESTCENTER_PASSWORD
#
# UPLOADS GO THROUGH `testcentercmd`, which ships with Test Center (bin/testcentercmd)
# and with Squish. An earlier version of this script POSTed JUnit XML to an invented
# REST path — the product has a supported command-line client, and using it makes the
# protocol the vendor's business rather than this repository's guess.
#
# Licence-bound like Squish itself, so it follows the same rule: not installed, not
# reachable or not configured means the stage prints why and exits 3 ("skipped"). It
# is never a build gate — the quality PDF lists the missing licence instead.
# How to obtain and install it: docs/qt-tools.md.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS="$ROOT/test-results"

DRY_RUN=0
TC_DIR="${TESTCENTER_DIR:-}"
URL="${TESTCENTER_URL:-http://localhost:8800}"
PROJECT="${TESTCENTER_PROJECT:-TradingApp}"
BATCH="${TESTCENTER_BATCH:-}"
TOKEN="${TESTCENTER_TOKEN:-}"
USER_EMAIL="${TESTCENTER_USER:-}"
PASSWORD="${TESTCENTER_PASSWORD:-}"

while [ $# -gt 0 ]; do
    case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --testcenter-dir) TC_DIR="${2:-}"; shift ;;
    --url) URL="${2:-}"; shift ;;
    --project) PROJECT="${2:-}"; shift ;;
    --batch) BATCH="${2:-}"; shift ;;
    --token) TOKEN="${2:-}"; shift ;;
    --user) USER_EMAIL="${2:-}"; shift ;;
    --password) PASSWORD="${2:-}"; shift ;;
    -h | --help)
        sed -n '2,27p' "$0"
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        exit 2
        ;;
    esac
    shift
done

[ -n "$BATCH" ] || BATCH="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null ||
    date -u +%Y%m%d-%H%M)"

# Where testcentercmd is. In order: an explicit directory, PATH, the Test Center
# installer's own layout (~/testcenter-<version>-linux-x64), then a Squish install,
# which bundles the same client. Newest version wins.
find_testcentercmd() {
    if [ -n "$TC_DIR" ]; then
        local candidate
        for candidate in "$TC_DIR/bin/testcentercmd" "$TC_DIR/testcentercmd"; do
            [ -x "$candidate" ] && echo "$candidate" && return
        done
        return
    fi
    if command -v testcentercmd >/dev/null 2>&1; then
        command -v testcentercmd
        return
    fi
    local dir
    for dir in $(ls -d "$HOME"/testcenter-* /opt/testcenter* "$HOME"/squish-for-qt-* \
        /opt/squish* 2>/dev/null | sort -Vr); do
        [ -x "$dir/bin/testcentercmd" ] && echo "$dir/bin/testcentercmd" && return
    done
}

CMD="$(find_testcentercmd)"

mapfile -t XML < <(find "$RESULTS" -name '*.xml' -type f 2>/dev/null | sort)

if [ "${#XML[@]}" -eq 0 ]; then
    echo "no test results in $RESULTS — run tools/run_tests.sh first"
    exit 1
fi

echo "== Squish Test Center upload =="
echo "results:  ${#XML[@]} XML file(s)"
for f in "${XML[@]}"; do
    printf '  %s\n' "${f#"$ROOT"/}"
done
echo "client:   ${CMD:-<not found>}"
echo "server:   $URL"
echo "project:  $PROJECT"
echo "batch:    $BATCH"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "(dry run — nothing sent)"
    exit 0
fi

if [ -z "$CMD" ]; then
    echo "SKIPPED: testcentercmd not found."
    echo "         Squish Test Center is licence-bound (qt.io); its installer unpacks to"
    echo "         ~/testcenter-<version>-linux-x64. Pass --testcenter-dir or set"
    echo "         TESTCENTER_DIR. See docs/qt-tools.md. Never a build gate."
    exit 3
fi

# Is a server actually there? Without this check testcentercmd waits for interactive
# credentials, which in a pipeline is a HUNG stage rather than a reported one.
if ! curl --max-time 10 -fsS -o /dev/null "$URL" 2>/dev/null; then
    echo "SKIPPED: no Test Center answering at $URL"
    echo "         Start it with:  <install-dir>/bin/testcenter start"
    echo "         then open $URL once to create the first user. Never a build gate."
    exit 3
fi

AUTH=()
if [ -n "$TOKEN" ]; then
    AUTH+=("--token=$TOKEN")
elif [ -n "$USER_EMAIL" ] && [ -n "$PASSWORD" ]; then
    AUTH+=("--user=$USER_EMAIL" "--password=$PASSWORD")
else
    echo "SKIPPED: no credentials — set TESTCENTER_TOKEN (recommended) or"
    echo "         TESTCENTER_USER + TESTCENTER_PASSWORD. The token is created in the"
    echo "         Test Center UI under the user menu. Never a build gate."
    exit 3
fi

# --interactive=no is what keeps a pipeline from stopping at a credential prompt.
# One call with every file: Test Center groups them into the named batch itself.
"$CMD" --url="$URL" "${AUTH[@]}" --interactive=no \
    upload "$PROJECT" --batch="$BATCH" "${XML[@]}"
RC=$?

if [ "$RC" -ne 0 ]; then
    echo "Test Center upload failed (rc=$RC) — the results are still in $RESULTS" >&2
    exit "$RC"
fi
echo "batch $BATCH is at $URL (project $PROJECT)"
