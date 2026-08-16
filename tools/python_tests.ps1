# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/python_tests.sh — unit tests + branch coverage
    for tools/*.py and tools/ml/*.py.

.DESCRIPTION
    The Python analogue of the C++ MC/DC gate (tools/coverage.ps1 mcdc), which
    clang-cl/llvm-cov never reach because they only ever see src/ and tests/.
    Two halves, two interpreters, since the ML modules need
    numpy/sklearn/xgboost/onnxruntime and the rest are stdlib-only on purpose:

      tools/tests/        tools/*.py       run through the pip `pytest` ($PipPkgs)
      tools/tests/ml/     tools/ml/*.py    run through the ML venv's own pytest
                                           (tools/ml/requirements.txt, .\setup.ps1 ml)

    --cov-branch is load-bearing: line coverage alone would call an `if`
    covered by hitting either arm once, exactly the gap MC/DC closes on the
    C++ side. Coverage is REPORTED, not gated on a hard number yet — see the
    threshold note below.
#>

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$ExitSkipped = 3
New-Item -ItemType Directory -Force -Path analysis-results | Out-Null

if (-not (Get-Command pytest -ErrorAction SilentlyContinue)) {
    Write-Host "python_tests: 'pytest' not found - run .\setup.ps1 - skipped"
    exit $ExitSkipped
}

Write-Host '== stdlib-only tool tests (tools\*.py) ==' -ForegroundColor Cyan
& pytest tools/tests --ignore=tools/tests/ml `
    --cov=tools --cov-branch --cov-report=term-missing `
    --cov-report=xml:analysis-results/pytest-cov-tools.xml `
    --junitxml=analysis-results/pytest-tools.xml -q
$stdlibStatus = $LASTEXITCODE

$mlVenv = if ($env:ML_VENV_DIR) { $env:ML_VENV_DIR } else { Join-Path $env:USERPROFILE '.local\tradingapp-ml' }
$mlPytest = Join-Path $mlVenv 'Scripts\pytest.exe'
$mlStatus = 0
if (Test-Path $mlPytest) {
    Write-Host '== ML pipeline tests (tools\ml\*.py) ==' -ForegroundColor Cyan
    & $mlPytest tools/tests/ml `
        --cov=tools/ml --cov-branch --cov-report=term-missing `
        --cov-report=xml:analysis-results/pytest-cov-ml.xml `
        --junitxml=analysis-results/pytest-ml.xml -q
    $mlStatus = $LASTEXITCODE
} else {
    Write-Host "== ML pipeline tests skipped: no ML venv ($mlVenv) - .\setup.ps1 ml ==" -ForegroundColor DarkGray
}

# A hard percentage floor is deliberately NOT enforced here yet - the suite is new
# and a ratchet needs a baseline run first, the same reasoning tools/lizard_metrics.py's
# own ratchet is built on. Report both figures either way so the gap is visible.
if ($stdlibStatus -ne 0) { Write-Warning "python_tests: stdlib tool tests FAILED (exit $stdlibStatus)" }
if ($mlStatus -ne 0) { Write-Warning "python_tests: ML tool tests FAILED (exit $mlStatus)" }
if ($stdlibStatus -eq 0 -and $mlStatus -eq 0) { exit 0 } else { exit 1 }
