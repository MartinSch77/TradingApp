#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# The Axivion findings as their OWN PDF, reproducibly — separate from the quality report.
#
#   tools/axivion_report.sh                      # downloads/TradingApp-axivion-report.pdf
#   tools/axivion_report.sh --no-details         # summary only (much smaller)
#   tools/axivion_report.sh --output-dir DIR
#   tools/axivion_report.sh --dashboard URL --project NAME
#
# Every path and credential is a PARAMETER with an environment fallback, matching the rest
# of the pipeline:
#   --suite-dir  | AXIVION_HOME            the Suite (default ~/bauhaus-suite)
#   --dashboard  | AXIVION_DASHBOARD_URL   default http://localhost:9090/axivion/
#   --project    | AXIVION_PROJECT         default TradingApp
#   --username   | AXIVION_USERNAME        default admin   (local-dev default, as in make_report.py)
#   --password   | AXIVION_PASSWORD        default password
#   --token      | AXIVION_TOKEN           preferred over username/password when set
#
# THE REPORT IS AXIVION'S OWN, NOT A REIMPLEMENTATION. The Suite ships a reporting
# framework (`bin/report_runner`) plus report modules under `example/reports/`; this script
# drives `report_misra_pdf.py` through it. That matters for the same reason the Test Center
# upload goes through `testcentercmd`: the document's layout, its rule tables and its
# delta-versus-previous-version logic then remain the vendor's business rather than this
# repository's guess, and a Suite upgrade improves the report for free.
#
# WHY SEPARATE FROM THE QUALITY PDF: `tools/make_report.py` summarises the whole run in one
# colour PDF and quotes Axivion's finding COUNTS from the dashboard. This one is the
# findings themselves — per rule, per file, with the delta against the previous analysed
# version — which is a different document for a different reader, and far too long to fold
# into a run summary.
#
# Licence-bound, so it follows this project's rule: no Suite, or no dashboard answering,
# means the stage prints why and exits 3 ("skipped"). It is never a build gate.
#
# ORDER: run this AFTER `axivion/start_analysis.sh` (or `./build_all.sh axivion`), or the
# PDF describes the PREVIOUS analysis. The script prints which version it reported on, so
# a stale document is visible rather than silent.
#
# Counterpart: tools/axivion_report.ps1
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SUITE="${AXIVION_HOME:-$HOME/bauhaus-suite}"
DASHBOARD="${AXIVION_DASHBOARD_URL:-http://localhost:9090/axivion/}"
PROJECT="${AXIVION_PROJECT:-TradingApp}"
USERNAME="${AXIVION_USERNAME:-admin}"
PASSWORD="${AXIVION_PASSWORD:-password}"
TOKEN="${AXIVION_TOKEN:-}"
OUT_DIR="$ROOT/downloads"
DETAILS=true

while [ $# -gt 0 ]; do
    case "$1" in
    --suite-dir) SUITE="${2:-}"; shift ;;
    --dashboard) DASHBOARD="${2:-}"; shift ;;
    --project) PROJECT="${2:-}"; shift ;;
    --username) USERNAME="${2:-}"; shift ;;
    --password) PASSWORD="${2:-}"; shift ;;
    --token) TOKEN="${2:-}"; shift ;;
    --output-dir) OUT_DIR="${2:-}"; shift ;;
    --no-details) DETAILS=false ;;
    -h | --help)
        sed -n '2,33p' "$0"
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        exit 2
        ;;
    esac
    shift
done

RUNNER="$SUITE/bin/report_runner"
MODULE="$SUITE/example/reports/report_misra_pdf.py"

if [ ! -x "$RUNNER" ]; then
    echo "SKIPPED: no Axivion Suite at $SUITE (looked for bin/report_runner)"
    echo "         Licence-bound; ./setup.sh cannot install it — see docs/qt-tools.md."
    exit 3
fi
if [ ! -f "$MODULE" ]; then
    echo "SKIPPED: this Suite has no MISRA PDF report module at"
    echo "         $MODULE"
    echo "         (the reporting examples ship under example/reports/ — check the install)"
    exit 3
fi

# The dashboard has to be UP and hold this project: report_runner reads the analysis
# results from it. Checked here so the failure names the cause instead of surfacing as a
# stack trace from inside the framework.
probe="${DASHBOARD%/}/api/projects/$PROJECT"
if ! curl -s -f -m 20 -u "$USERNAME:$PASSWORD" "$probe" -o /dev/null 2>/dev/null; then
    echo "SKIPPED: no Axivion dashboard answering for project '$PROJECT' at $DASHBOARD"
    echo "         Start it and run an analysis first: axivion/start_analysis.sh"
    echo "         (or ./build_all.sh axivion). Probed: $probe"
    exit 3
fi

# Which version the report will describe — printed so a PDF generated before today's
# analysis is recognisable as stale rather than trusted.
version="$(curl -s -m 20 -u "$USERNAME:$PASSWORD" "$probe" |
    python3 -c 'import json,sys
try:
    versions = json.load(sys.stdin).get("versions") or []
    print(versions[-1].get("date", "?") if versions else "none")
except Exception:
    print("?")' 2>/dev/null)"

mkdir -p "$OUT_DIR"
echo "Axivion Suite:    $SUITE"
echo "dashboard:        $DASHBOARD (project $PROJECT)"
echo "analysed version: $version"
echo "report module:    $(basename "$MODULE") (Axivion's own)"

auth=(--authentication password --username "$USERNAME" --password "$PASSWORD")
if [ -n "$TOKEN" ]; then
    auth=(--authentication token --token "$TOKEN")
fi

# --noninteractive belongs to the RUNNER, before the subcommand — after it the parser
# rejects it ("unrecognized arguments"), which is easy to get wrong in CI.
if ! "$RUNNER" --noninteractive create_report \
    --dashboard "$DASHBOARD" --project "$PROJECT" "${auth[@]}" \
    --output_dir "$OUT_DIR" \
    "$MODULE" "details=$DETAILS"; then
    echo "the Axivion report module failed — see its output above" >&2
    exit 1
fi

# The module names the file itself (<project>_misra.pdf); rename to this project's
# artefact convention so publish_release.sh and the docs can refer to one stable name.
produced="$OUT_DIR/${PROJECT}_misra.pdf"
final="$OUT_DIR/TradingApp-axivion-report.pdf"
if [ -f "$produced" ]; then
    mv -f "$produced" "$final"
fi
if [ ! -f "$final" ]; then
    echo "the report module reported success but produced no PDF in $OUT_DIR" >&2
    exit 1
fi
echo ""
echo "Axivion report: $final ($(du -h "$final" | cut -f1)), analysis version $version"
