#!/usr/bin/env bash
# Send EVERY test result this project produces to Qt Test Center (Squish Test
# Center) — the unit/integration suite and the Squish GUI suite alike.
#
#   tools/testcenter_upload.sh              # upload test-results/**.xml
#   tools/testcenter_upload.sh --dry-run    # list exactly what would be uploaded
#
# Configuration, all from the environment so no secret lands in the repository:
#   TESTCENTER_URL      e.g. http://localhost:8800          (required)
#   TESTCENTER_PROJECT  the project name in Test Center      (default TradingApp)
#   TESTCENTER_TOKEN    an API token from Test Center        (required)
#   TESTCENTER_BATCH    batch/label for this upload          (default: the git sha)
#
# Licence-bound like Squish itself, so it follows the same rule: not configured or
# not reachable means the stage prints why and exits 3 ("skipped"). It is never a
# build gate — the quality PDF reports the missing licence instead.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS="$ROOT/test-results"
DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

URL="${TESTCENTER_URL:-}"
PROJECT="${TESTCENTER_PROJECT:-TradingApp}"
TOKEN="${TESTCENTER_TOKEN:-}"
BATCH="${TESTCENTER_BATCH:-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || date -u +%Y%m%d-%H%M)}"

# Every JUnit XML the pipeline writes: tools/run_tests.sh puts the Qt Test suites
# here, tools/squish_run.sh adds the GUI suite under squish/.
mapfile -t XML < <(find "$RESULTS" -name '*.xml' -type f 2>/dev/null | sort)

if [ "${#XML[@]}" -eq 0 ]; then
    echo "no test results in $RESULTS — run tools/run_tests.sh first"
    exit 1
fi

echo "== Qt Test Center upload =="
echo "results:  ${#XML[@]} XML file(s)"
for f in "${XML[@]}"; do
    printf '  %s\n' "${f#"$ROOT"/}"
done
echo "project:  $PROJECT"
echo "batch:    $BATCH"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "(dry run — nothing sent)"
    exit 0
fi

if [ -z "$URL" ] || [ -z "$TOKEN" ]; then
    echo "SKIPPED: Test Center not configured — set TESTCENTER_URL and TESTCENTER_TOKEN"
    echo "         (licence-bound; see todo.txt). Never a build gate."
    exit 3
fi
if ! curl --proto '=https,http' --max-time 10 -fsS "$URL/api/v1/projects" \
        -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1; then
    echo "SKIPPED: Test Center at $URL did not answer (wrong URL, server down, or bad token)"
    echo "         Never a build gate."
    exit 3
fi

RC=0
for f in "${XML[@]}"; do
    # Test Center ingests JUnit XML through its upload endpoint; one call per file so
    # a single bad report cannot hide the rest.
    if curl --proto '=https,http' --max-time 60 -fsS -X POST \
        "$URL/api/v1/projects/$PROJECT/reports?batch=$BATCH" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/xml" \
        --data-binary "@$f" >/dev/null; then
        echo "uploaded: ${f#"$ROOT"/}"
    else
        echo "FAILED to upload ${f#"$ROOT"/}" >&2
        RC=1
    fi
done
echo "batch $BATCH is at $URL (project $PROJECT)"
exit "$RC"
