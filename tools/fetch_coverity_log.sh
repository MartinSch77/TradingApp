#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Bring the Coverity build log DOWN from the CI run into analysis-results/, where the
# rest of the analysis evidence lives.
#
#   tools/fetch_coverity_log.sh                 # the newest Coverity Scan run
#   tools/fetch_coverity_log.sh <run-id>        # a specific run
#   tools/fetch_coverity_log.sh --list          # what runs are available
#
# WHY THIS EXISTS. Coverity Scan analyses server-side, so the defects appear on the
# Scan web UI rather than in this repository. The one artefact the run itself produces
# is `cov-int/build-log.txt` — what cov-build actually captured — and that is the file
# that answers the question a failed or empty submission raises: did it see the
# compiler at all, and how many translation units did it emit? It only exists on the
# runner, so the workflow uploads it (`coverity-build-log`, kept for 90 days) and this
# script fetches it.
#
# It lands in analysis-results/coverity-build-log.txt, which puts it in the
# qualification bundle with every other analyzer's output rather than behind a
# download button in a web UI nobody opens after the fact.
#
# Needs the GitHub CLI, authenticated (`gh auth login`). Without it — or with no run
# to fetch from — this prints why and exits 3 ("skipped"), like every other optional
# stage here: a missing cloud artefact must not fail a local build.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/analysis-results"
WORKFLOW="coverity.yml"
ARTIFACT="coverity-build-log"

if ! command -v gh >/dev/null 2>&1; then
    echo "SKIPPED: the GitHub CLI (gh) is not installed — see docs/tools.md"
    exit 3
fi
if ! gh auth status >/dev/null 2>&1; then
    echo "SKIPPED: gh is not authenticated (run: gh auth login)"
    exit 3
fi

if [ "${1:-}" = "--list" ]; then
    gh run list --workflow="$WORKFLOW" -L 10
    exit 0
fi

RUN_ID="${1:-}"
if [ -z "$RUN_ID" ]; then
    # The newest run that actually produced the artifact; a run can be green and still
    # have none (the no-op path when the scan secrets are absent).
    RUN_ID="$(gh run list --workflow="$WORKFLOW" -L 10 --json databaseId \
        --jq '.[].databaseId' 2>/dev/null | head -1)"
fi
if [ -z "$RUN_ID" ]; then
    echo "SKIPPED: no Coverity Scan runs found (the workflow is weekly + manual)"
    exit 3
fi

mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== Coverity build log =="
echo "run:      $RUN_ID"
if ! gh run download "$RUN_ID" --name "$ARTIFACT" --dir "$TMP" 2>/dev/null; then
    echo "SKIPPED: run $RUN_ID has no '$ARTIFACT' artifact"
    echo "         (it expires after 90 days, and a run without the scan secrets"
    echo "         produces none). tools/fetch_coverity_log.sh --list shows the runs."
    exit 3
fi

SRC="$TMP/build-log.txt"
[ -f "$SRC" ] || SRC="$(find "$TMP" -name 'build-log.txt' -type f | head -1)"
if [ ! -f "$SRC" ]; then
    echo "SKIPPED: the artifact did not contain build-log.txt"
    exit 3
fi

cp "$SRC" "$OUT/coverity-build-log.txt"
echo "saved:    analysis-results/coverity-build-log.txt ($(wc -l <"$SRC") lines)"

# The number that decides whether the submission was worth anything: cov-build exits 0
# even when it captured NOTHING, so the emitted-unit count is the real result.
UNITS="$(sed -n 's/.*Emitted \([0-9]\{1,\}\) .*compilation units.*/\1/p' "$SRC" | tail -1)"
if [ -n "$UNITS" ]; then
    echo "captured: $UNITS compilation units"
else
    echo "captured: the log names no compilation-unit count — the build may have been"
    echo "          watched without ever seeing the compiler"
fi
grep -iE "^\[(WARNING|ERROR)\]" "$SRC" | sort | uniq -c | sort -rn | head -5 |
    sed 's/^/  /' || true
