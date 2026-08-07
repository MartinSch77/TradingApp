# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# What this machine can and cannot do — every external program the pipeline uses,
# what it is FOR, and how to get it. The PowerShell counterpart of
# tools/check_prerequisites.sh; the two are meant to stay in lockstep.
#
#   tools\check_prerequisites.ps1              # everything, grouped
#   tools\check_prerequisites.ps1 -Release     # only what a RELEASE needs
#   tools\check_prerequisites.ps1 -Quiet       # one line per missing item
#
# Why this exists rather than a page in the README: a list of prerequisites in prose
# goes stale silently, while a check that runs says what is true today.
#
# THE RULE THIS PROJECT FOLLOWS: everything OPEN SOURCE that the pipeline needs is
# installable by .\setup.ps1. The licence-bound tools (Squish, Squish Coco, Axivion,
# Qt Test Center) cannot be, so they report SKIPPED and are listed as MISSING LICENCES
# in the quality PDF. A missing licence is never a build failure here.
[CmdletBinding()]
param(
    [switch]$Release,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot

$script:MissingRequired = 0
$script:MissingOptional = 0

function Write-Item {
    param(
        [ValidateSet('ok', 'missing-required', 'missing-optional', 'missing-licence')]
        [string]$State,
        [string]$Name,
        [string]$Purpose,
        [string]$HowTo = ''
    )
    switch ($State) {
        'ok' {
            if (-not $Quiet) {
                Write-Host ("  [ok]      {0,-22} {1}" -f $Name, $Purpose) -ForegroundColor Green
            }
        }
        'missing-required' {
            $script:MissingRequired++
            Write-Host ("  [MISSING] {0,-22} {1}" -f $Name, $Purpose) -ForegroundColor Red
            Write-Host ("            install: {0}" -f $HowTo) -ForegroundColor DarkGray
        }
        'missing-optional' {
            $script:MissingOptional++
            Write-Host ("  [absent]  {0,-22} {1}" -f $Name, $Purpose) -ForegroundColor Yellow
            Write-Host ("            install: {0}" -f $HowTo) -ForegroundColor DarkGray
        }
        'missing-licence' {
            $script:MissingOptional++
            Write-Host ("  [absent]  {0,-22} {1}" -f $Name, $Purpose) -ForegroundColor Yellow
            Write-Host ("            licence-bound - {0}" -f $HowTo) -ForegroundColor DarkGray
        }
    }
}

function Test-Item {
    param([string]$Command, [ValidateSet('required', 'optional')][string]$Need,
          [string]$Name, [string]$Purpose, [string]$HowTo)
    if (Test-Tool $Command) { Write-Item ok $Name $Purpose }
    else { Write-Item "missing-$Need" $Name $Purpose $HowTo }
}

function Write-Section {
    param([string]$Title)
    if (-not $Quiet) { Write-Host ""; Write-Host "== $Title ==" }
}

if (-not $Release) {
    Write-Section 'build + test (.\build_all.ps1 build test)'
    Test-Item 'cmake' required 'CMake' 'configures and builds everything' '.\setup.ps1'
    Test-Item 'ninja' optional 'Ninja' 'faster builds; CMake falls back to the VS generator' '.\setup.ps1'
    Test-Item 'python' required 'Python 3' 'traceability, metrics, the report, the trainer' 'winget install Python.Python.3.12'

    $qt = Resolve-QtPrefix -Quiet
    if ($qt) { Write-Item ok 'Qt 6' $qt }
    else { Write-Item missing-required 'Qt 6' 'the toolkit the app is written in' '.\setup.ps1 - installs a Qt kit with Charts and Graphs' }

    $msvc = Get-VsInstallPath
    if ($msvc) { Write-Item ok 'MSVC' "the compiler ($msvc)" }
    else { Write-Item missing-required 'MSVC' 'the compiler' 'Visual Studio 2022 Build Tools' }

    Write-Section 'static analysis (.\build_all.ps1 analysis)'
    Test-Item 'cppcheck' required 'cppcheck' 'one of the seven gated analyzers' '.\setup.ps1'
    Test-Item 'clang-tidy' required 'clang-tidy' 'gated analyzer + the .clang-tidy rules' 'winget install LLVM.LLVM'
    Test-Item 'codespell' optional 'codespell' 'spelling in code and docs (gated)' 'python -m pip install codespell'
    Test-Item 'java' optional 'Java' 'runs PMD CPD, the clone gate' 'winget install EclipseAdoptium.Temurin.21.JRE'
    Test-Item 'lizard' optional 'lizard' 'the code-metrics ratchet' 'python -m pip install lizard'
    Test-Item 'clang-format' optional 'clang-format' 'the CI formatting check' 'winget install LLVM.LLVM'
    # No clazy on Windows - said out loud rather than skipped quietly (docs/windows.md).
    Write-Item missing-optional 'clazy' 'Qt-specific analyzer' 'not available on Windows - the Linux runs cover it'

    Write-Section 'coverage (.\build_all.ps1 coverage)'
    Test-Item 'OpenCppCoverage' optional 'OpenCppCoverage' 'line coverage for the MSVC build' 'winget install OpenCppCoverage.OpenCppCoverage'
    Test-Item 'llvm-cov' optional 'llvm-cov' 'clang MC/DC (needs clang-cl >= 18)' 'winget install LLVM.LLVM'
    $cocoDir = if ($env:COCO_DIR) { $env:COCO_DIR } else { 'C:\Program Files\squishcoco' }
    if (Test-Path (Join-Path $cocoDir 'csg++.exe')) {
        if (Test-CocoUsable) { Write-Item ok 'Squish Coco' 'statement/decision/condition + MC/DC, licensed' }
        else { Write-Item missing-licence 'Squish Coco' 'statement/decision/condition + MC/DC' "installed at $cocoDir but the licence check fails" }
    } else {
        Write-Item missing-licence 'Squish Coco' 'statement/decision/condition + MC/DC' 'qt.io download + licence'
    }

    Write-Section 'dynamic analysis (.\build_all.ps1 sanitize)'
    Test-Item 'clang-cl' optional 'clang-cl' 'the ASan build' 'winget install LLVM.LLVM'
    # No TSan and no valgrind on Windows - also said out loud.
    Write-Item missing-optional 'TSan / valgrind' 'thread and memory checkers' 'not available on Windows - the Linux runs cover them'

    Write-Section 'GUI testing (licence-bound, never a gate)'
    $squish = $null
    foreach ($c in @($env:SQUISH_DIR, $env:SQUISH_PREFIX,
                     (Join-Path $env:USERPROFILE 'squish-for-qt-*'),
                     'C:\Squish*')) {
        if (-not $c) { continue }
        $hit = Get-Item $c -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName 'bin\squishrunner.exe') } |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($hit) { $squish = $hit.FullName; break }
    }
    if ($squish) { Write-Item ok 'Squish' "the GUI suite ($squish), forced into simulation" }
    else { Write-Item missing-licence 'Squish' 'the GUI suite (tools\squish_run.ps1)' 'qt.io download + licence' }

    if ($env:TESTCENTER_URL -and $env:TESTCENTER_TOKEN) {
        Write-Item ok 'Qt Test Center' "results upload configured ($env:TESTCENTER_URL)"
    } else {
        Write-Item missing-licence 'Qt Test Center' 'collects every JUnit XML in one place' 'set TESTCENTER_URL and TESTCENTER_TOKEN'
    }

    Write-Section 'optional runtime features'
    if (Test-Tool 'ollama') { Write-Item ok 'Ollama' 'the LOCAL model the bot can take its picks from' }
    else { Write-Item missing-optional 'Ollama' 'the LOCAL model the bot can take its picks from' 'winget install Ollama.Ollama, then: ollama pull qwen2.5:1.5b' }
}

Write-Section 'packaging a release (tools\package_portable.ps1, tools\publish_release.ps1)'
Test-Item 'git' required 'git' 'the version, the tag and the clean-tree check' 'winget install Git.Git'
Test-Item 'gh' required 'GitHub CLI' 'creates the release and uploads the assets' 'winget install GitHub.cli, then: gh auth login'
Test-Item 'windeployqt' optional 'windeployqt' 'collects the Qt runtime into the portable ZIP' 'ships with Qt - .\setup.ps1'
if (python -c "import reportlab" 2>$null; $LASTEXITCODE -eq 0) {
    Write-Item ok 'reportlab' 'renders the quality PDF (tools\make_report.py)'
} else {
    Write-Item missing-required 'reportlab' 'renders the quality PDF - a release needs it' 'python -m pip install reportlab'
}
if ((Test-Tool 'strictdoc') -or (python -c "import strictdoc" 2>$null; $LASTEXITCODE -eq 0)) {
    Write-Item ok 'StrictDoc' 'regenerates docs/requirements.md from the .sdoc source'
} else {
    Write-Item missing-optional 'StrictDoc' 'regenerates docs/requirements.md' 'python -m pip install strictdoc'
}

if (-not $Quiet) {
    Write-Host ""
    Write-Host "== the other platforms =="
    Write-Host "  The Linux AppImages (x86-64 and ARM64) and the signed Android APK are built by"
    Write-Host "  .github/workflows/release.yml when a v* tag is pushed. Nothing on this machine"
    Write-Host "  can produce them all, and the release script does not pretend otherwise."
    Write-Host ""
    Write-Host "  Axivion Suite is x86-64 Linux/Windows only; its stage reports skipped elsewhere."
}

Write-Host ""
if ($script:MissingRequired -gt 0) {
    Write-Host ("{0} required tool(s) missing - {1} optional/licence-bound" -f $script:MissingRequired, $script:MissingOptional) -ForegroundColor Red
    exit 1
}
if ($script:MissingOptional -gt 0) {
    Write-Host ("everything required is present - {0} optional/licence-bound tool(s) absent, their stages will report SKIPPED" -f $script:MissingOptional) -ForegroundColor Green
    exit 0
}
Write-Host "every tool this pipeline can use is present" -ForegroundColor Green
