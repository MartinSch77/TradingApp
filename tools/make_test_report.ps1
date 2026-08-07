# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    The FINAL test report, reproducibly: one command that produces every piece of test
    evidence this project can produce on this machine, then the PDF that summarises it.

.DESCRIPTION
    Counterpart of tools/make_test_report.sh - same steps, same order, same exit rules.

    WHY THIS EXISTS, given that .\build_all.ps1 already ends with a `report` stage: the
    default build_all run does not include the LICENCE-BOUND stages. Squish (the GUI
    suite), the GUI coverage measurement and the Test Center upload are extra stages that
    must be asked for by name, so the PDF from a default run honestly reports them as
    "no licence here" even on a machine that has one. This script is the ordered chain
    that collects all of it and renders the PDF LAST, so the report contains the GUI
    evidence too.

    ORDER IS THE WHOLE POINT and it is not arbitrary:
      1. test      - the unit + integration suites, JUnit XML per test function
      2. gui       - the Squish workflows against the real app, forced into SIMULATION
      3. coco-gui  - what those workflows COVER, in its own database and its own report,
                     never merged with the unit suites' coverage
      4. upload    - every JUnit XML in test-results\ to Test Center, one batch per commit
      5. axivion   - the Axivion findings as their OWN PDF (Axivion's report module)
      6. report    - the quality PDF, last, so it sees everything above

    A licence-bound step that cannot run exits 3 ("skipped"), is REPORTED as skipped, and
    does not fail the chain - the PDF then names it as a missing licence rather than
    pretending it passed.

.EXAMPLE
    tools\make_test_report.ps1
    tools\make_test_report.ps1 -NoUpload
    tools\make_test_report.ps1 -PdfOnly
#>
[CmdletBinding()]
param(
    [switch]$NoUpload,
    [switch]$PdfOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root

# Names and results, printed as one table at the end: a chain of five steps where three
# can legitimately skip needs a summary, or the reader cannot tell "clean" from "absent".
$steps = [System.Collections.ArrayList]::new()

function Invoke-Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Host ""
    Write-Host "==================== $Name ====================" -ForegroundColor Cyan
    & $Body
    $rc = $LASTEXITCODE
    $result = switch ($rc) {
        0 { 'ok' }
        # Exit 3 means "this step could not run", NOT "the licence is missing" - see the
        # counterpart note in make_test_report.sh. Measured 2026-08-07: the GUI-coverage
        # step exited 3 because the Coco runtime wrote no .csexe, on a machine whose licence
        # is fine, and the table still blamed the licence. The step prints its own reason
        # directly above this summary; the table must not contradict it.
        3 { "skipped - see the step's own reason above" }
        default { "FAILED (rc=$rc)" }
    }
    # A failing step must not stop the chain: partial evidence still beats none.
    $null = $steps.Add([pscustomobject]@{ Name = $Name; Result = $result })
}

try {
    if (-not $PdfOnly) {
        Invoke-Step 'unit + integration tests' { & "$Root\build_all.ps1" test }
        Invoke-Step 'Squish GUI suite' { & "$Root\build_all.ps1" gui }
        Invoke-Step 'GUI coverage (Coco, separate)' { & "$Root\tools\coverage.ps1" -Mode coco-gui }
        if (-not $NoUpload) {
            # Regenerated with the results, not on its own schedule: the requirement list
            # Test Center shows must describe the same commit as the batch beside it.
            Invoke-Step 'traceability data (from the sdoc)' {
                python "$Root\tools\testcenter_traceability.py"
            }
            Invoke-Step 'Test Center upload' { & "$Root\tools\testcenter_upload.ps1" }
        }
        Invoke-Step 'Axivion findings PDF' { & "$Root\tools\axivion_report.ps1" }
    }
    # The PDF last, and through the same tool build_all's report stage uses - there is
    # exactly one renderer, so the report cannot drift from the one CI publishes.
    Invoke-Step 'quality PDF' { python "$Root\tools\make_report.py" }

    Write-Host ""
    Write-Host "==================== summary ====================" -ForegroundColor Cyan
    foreach ($s in $steps) { Write-Host ('  {0,-34} {1}' -f $s.Name, $s.Result) }
    Write-Host ""
    Write-Host '  test results     test-results\*.xml'
    Write-Host '  unit coverage    coverage\coco\index.html   (+ msvc / mcdc when Coco is absent)'
    Write-Host '  GUI coverage     coverage\coco-gui\index.html'
    Write-Host '  final PDF        downloads\TradingApp-quality-report.pdf'
    Write-Host '  Axivion PDF      downloads\TradingApp-axivion-report.pdf'
    $url = if ($env:TESTCENTER_URL) { $env:TESTCENTER_URL } else { 'http://localhost:8800' }
    Write-Host "  Test Center      $url -> project TradingApp"
    Write-Host '  traceability     test-results\testcenter-traceability.csv  (upload in Global Settings)'
    # The one step no script here can do. Named rather than omitted, because a reader who
    # is not told it exists will assume the chain already produced it: Test Center's only
    # PDF route is the browser's print dialog (docs\qt-tools.md).
    $tcPdf = Join-Path $Root 'downloads\TradingApp-testcenter-report.pdf'
    if (Test-Path $tcPdf) {
        Write-Host '  Test Center PDF  downloads\TradingApp-testcenter-report.pdf'
    }
    else {
        Write-Host '  Test Center PDF  MANUAL: Explore -> the batch -> Printable Report -> Print to PDF'
        Write-Host '                   save as downloads\TradingApp-testcenter-report.pdf'
    }

    # Non-zero only when a step actually FAILED. A skipped licence-bound step is not a
    # failure - that rule is what keeps this runnable on a machine without Squish.
    if ($steps | Where-Object { $_.Result -like 'FAILED*' }) { exit 1 }
    exit 0
} finally {
    Pop-Location
}
