#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# The FINAL test report, reproducibly: one command that produces every piece of test
# evidence this project can produce on this machine and then the PDF that summarises it.
#
#   tools/make_test_report.sh                  # the full evidence chain, then the PDF
#   tools/make_test_report.sh --no-upload      # skip the Test Center upload
#   tools/make_test_report.sh --pdf-only       # re-render the PDF from existing artefacts
#
# WHY THIS EXISTS, given that ./build_all.sh already ends with a `report` stage: the
# default build_all run does not include the LICENCE-BOUND stages. Squish (the GUI
# suite), the GUI coverage measurement and the Test Center upload are extra stages that
# have to be asked for by name, so the PDF from a default run honestly reports them as
# "no licence here" even on a machine that has one. This script is the ordered chain that
# collects all of it and renders the PDF LAST, so the report contains the GUI evidence
# too.
#
# ORDER IS THE WHOLE POINT and it is not arbitrary:
#   1. test      — the unit + integration suites, JUnit XML per test function
#   2. gui       — the Squish workflows against the real app, forced into SIMULATION
#   3. coco-gui  — what those workflows COVER, in its own database and its own report,
#                  never merged with the unit suites' coverage
#   4. upload    — every JUnit XML in test-results/ to Test Center, one batch per commit
#   5. axivion   — the Axivion findings as their OWN PDF (Axivion's report module), which
#                  is a different document for a different reader than the run summary
#   6. report    — the quality PDF, last, so it sees everything above
#
# Reproducibility: every step reads only files under this repository and writes only to
# test-results/, coverage/ and downloads/. Re-running it on an unchanged tree produces the
# same evidence; the PDF's own timestamp is the only thing that moves. A licence-bound
# step that cannot run exits 3 ("skipped"), is REPORTED as skipped, and does not fail the
# chain — the PDF then names it as a missing licence rather than pretending it passed.
#
# Counterpart: tools/make_test_report.ps1 (same steps, same order, same exit rules).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

UPLOAD=1
PDF_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
    --no-upload) UPLOAD=0 ;;
    --pdf-only) PDF_ONLY=1 ;;
    -h | --help)
        sed -n '2,30p' "$0"
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        exit 2
        ;;
    esac
    shift
done

# Names and results, printed as one table at the end: a chain of five steps where three
# can legitimately skip needs a summary, or the reader cannot tell "clean" from "absent".
STEP_NAMES=()
STEP_RESULTS=()

run_step() {
    local name="$1"
    shift
    echo ""
    echo "==================== $name ===================="
    "$@"
    local rc=$?
    case $rc in
    0) STEP_RESULTS+=("ok") ;;
    # Exit 3 means "this step could not run", NOT "the licence is missing". Those are
    # different diagnoses and the summary used to assert the second one for every skip.
    # Measured 2026-08-07: the GUI-coverage step exited 3 saying "the GUI run produced no
    # execution report (.csexe) — the suite ran (rc=0) but the Coco runtime wrote
    # nothing", on a machine whose Coco licence is fine and which had just built an
    # instrumented tree with csg++. The summary still read "no licence / not installed",
    # which sends a reader hunting for a licence problem that does not exist. The step's
    # own message is printed directly above this table; the table must not contradict it.
    3) STEP_RESULTS+=("skipped — see the step's own reason above") ;;
    *) STEP_RESULTS+=("FAILED (rc=$rc)") ;;
    esac
    STEP_NAMES+=("$name")
    return 0    # a failing step must not stop the chain: partial evidence still beats none
}

if [ "$PDF_ONLY" -eq 0 ]; then
    run_step "unit + integration tests" ./build_all.sh test
    run_step "Squish GUI suite" ./build_all.sh gui
    run_step "GUI coverage (Coco, separate)" tools/coverage.sh coco-gui
    if [ "$UPLOAD" -eq 1 ]; then
        # Regenerated with the results, not on its own schedule: the requirement list
        # Test Center shows must describe the same commit as the batch beside it.
        run_step "traceability data (from the sdoc)" \
            python3 tools/testcenter_traceability.py
        # The analyzer GATES as test cases, so Test Center can chart them per batch.
        # Findings themselves stay on the Axivion dashboard; the reports ride along as
        # batch attachments, which testcenter_upload.sh picks up from downloads/.
        run_step "quality gates as test cases" python3 tools/gates_to_junit.py
        run_step "Test Center upload" tools/testcenter_upload.sh
    fi
    # Reports on whatever the dashboard last analysed and prints that version, so a PDF
    # made before today's analysis is recognisable as stale rather than trusted.
    run_step "Axivion findings PDF" tools/axivion_report.sh
fi
# The PDF last, and through the same tool build_all's report stage uses — there is exactly
# one renderer, so the report cannot drift from the one CI publishes.
run_step "quality PDF" python3 tools/make_report.py

echo ""
echo "==================== summary ===================="
i=0
while [ "$i" -lt "${#STEP_NAMES[@]}" ]; do
    printf '  %-34s %s\n' "${STEP_NAMES[$i]}" "${STEP_RESULTS[$i]}"
    i=$((i + 1))
done
echo ""
echo "  test results     test-results/*.xml"
echo "  unit coverage    coverage/coco/index.html   (+ gcov / mcdc when Coco is absent)"
echo "  GUI coverage     coverage/coco-gui/index.html"
echo "  final PDF        downloads/TradingApp-quality-report.pdf"
echo "  Axivion PDF      downloads/TradingApp-axivion-report.pdf"
echo "  Test Center      \${TESTCENTER_URL:-http://localhost:8800} -> project TradingApp"
echo "  traceability     test-results/testcenter-traceability.csv  (upload in Global Settings)"
# The one step no script here can do. Named rather than omitted, because a reader who is
# not told it exists will assume the chain already produced it: Test Center's only PDF
# route is the browser's print dialog (docs/qt-tools.md), and this host has no browser.
if [ -f "$ROOT/downloads/TradingApp-testcenter-report.pdf" ]; then
    echo "  Test Center PDF  downloads/TradingApp-testcenter-report.pdf"
else
    echo "  Test Center PDF  MANUAL: Explore -> the batch -> Printable Report -> Print to PDF"
    echo "                   save as downloads/TradingApp-testcenter-report.pdf"
fi

# Non-zero only when a step actually FAILED. A skipped licence-bound step is not a failure
# — that rule is what keeps this runnable on a machine without Squish.
for result in ${STEP_RESULTS[@]+"${STEP_RESULTS[@]}"}; do
    case "$result" in FAILED*) exit 1 ;; esac
done
exit 0
