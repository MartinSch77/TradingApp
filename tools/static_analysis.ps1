# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/static_analysis.sh — static analysis over the
    app sources.

.DESCRIPTION
    cppcheck + clang-tidy + the Clang Static Analyzer + MSVC /analyze + code
    metrics (lizard) + copy-paste detection (PMD CPD), plus codespell when
    installed. The app AND test sources are analysed.
    Reports land in analysis-results\ as one plain-text log per tool — the next
    axivion_ci run imports those onto the Axivion dashboard (see
    axivion/external_import.py) — plus one merged CSV as a single-file overview.
    Exit code 1 when any tool reported findings.

    Tool mapping against the Linux script:
      cppcheck      same tool, same flags
      clang-tidy    same tool (LLVM for Windows, or the pip clang-tidy wheel)
      clang-analyzer  same shared driver (tools\clang_analyzer.py). It picks the
                      clang that matches the compile database's flag dialect —
                      clang-cl for an MSVC database — and reports "skipped" when
                      no clang driver is installed.
      lizard        same shared driver (tools\lizard_metrics.py), same
                    thresholds and the same ratchet baseline
      pmd-cpd       same shared driver (tools\cpd_scan.py); PMD is a Java tool,
                    fetched by .\setup.ps1 into tools\third-party\
      g++ -fanalyzer  ->  MSVC /analyze (tools\msvc_analyze.py), provider
                          "msvc-analyze" — the same role: a second, compiler-
                          native symbolic-execution pass over every TU
      clazy         no Windows build exists. Not a coverage gap: Axivion's
                    Qt-* ruleset (~180 rules incl. the clazy checks, active in
                    axivion/rule_config.json) checks the Qt coding rules on
                    every axivion_ci run.

.PARAMETER BuildDir
    Build directory holding compile_commands.json (configure with
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON). Default: build

.PARAMETER Fix
    First apply clang-tidy's automatic fixes (sequentially — the checks edit
    shared headers), then run the normal analysis pass over the fixed code.
    Rebuild and rerun the tests afterwards!

.EXAMPLE
    tools\static_analysis.ps1
    tools\static_analysis.ps1 -BuildDir build -Fix
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [switch]$Fix
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot
$Out = Join-Path $Root 'analysis-results'
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

$Db = Join-Path $Root "$BuildDir\compile_commands.json"
if (-not (Test-Path $Db)) {
    Write-Error "no compile_commands.json in $BuildDir — configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON and build first"
    exit 1
}

# The test sources are analysed like production code (tests\.clang-tidy exempts
# only what Qt Test's moc-driven layout forces).
$Sources = @()
foreach ($pat in @('src\domain\*.cpp', 'src\services\*.cpp', 'src\ui\*.cpp', 'src\main.cpp',
        'tests\*.cpp')) {
    $Sources += @(Get-ChildItem (Join-Path $Root $pat) -File -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })
}

# ---------------------------------------------------------------------------
# clang-tidy --fix
# ---------------------------------------------------------------------------
if ($Fix) {
    Write-Stage 'clang-tidy --fix (sequential: checks edit shared headers)'
    if (-not (Test-Tool 'clang-tidy')) {
        Write-Skip "clang-tidy not installed — nothing fixed"
    } else {
        foreach ($f in $Sources) {
            Write-Host "fixing $(Split-Path $f -Leaf)"
            & clang-tidy -p (Join-Path $Root $BuildDir) --fix $f *> $null
        }
        Write-Host "auto-fixes applied — rebuild and rerun the tests"
    }
}

# ---------------------------------------------------------------------------
# cppcheck
# ---------------------------------------------------------------------------
$cppcheckN = 0
$cppcheckRc = 0
$cppcheckLog = Join-Path $Out 'cppcheck.txt'
if (Test-Tool 'cppcheck') {
    Write-Stage "cppcheck ($(& cppcheck --version))"
    # Same core flags as the Linux script: --project (compile database, so Qt
    # include paths and defines match the real build), --enable=all (every check
    # class, i.e. style and information on top of warning/performance/
    # portability), --check-level=exhaustive (the deeper value-flow search),
    # --inconclusive, --error-exitcode=1, --library=qt, the id-scoped
    # suppressions with their written rationale, --checkers-report as evidence
    # of which checkers ran, and the pipe template the dashboard imports. Only
    # the build tree is excluded (-i) — that is where the moc/autogen output is.
    & cppcheck "--project=$Db" `
        --enable=all `
        --check-level=exhaustive `
        --inconclusive `
        --error-exitcode=1 `
        --inline-suppr `
        "--suppressions-list=$(Join-Path $Root 'tools\cppcheck-suppressions.txt')" `
        --library=qt `
        -i (Join-Path $Root $BuildDir) `
        "--checkers-report=$(Join-Path $Out 'cppcheck-checkers.txt')" `
        --template='{file}|{line}|{severity}|{id}|{message}' `
        "--output-file=$cppcheckLog" --quiet
    $cppcheckRc = $LASTEXITCODE
    # No output file = cppcheck never analyzed anything (e.g. the compile DB
    # refers to missing moc autogen files). That must fail loudly — an
    # empty-but-present file is the legitimate "ran and found nothing".
    if (-not (Test-Path $cppcheckLog)) {
        Write-Error "cppcheck did not run: project load failed — build the compile-DB build dir first"
        exit 1
    }
    $cppcheckN = Get-LineCount $cppcheckLog
    Write-Host "cppcheck findings: $cppcheckN (analysis-results\cppcheck.txt)"
} else {
    Write-Stage 'cppcheck'
    Write-Skip "cppcheck not installed (winget install Cppcheck.Cppcheck)"
    Write-TextFile $cppcheckLog ''
}

# ---------------------------------------------------------------------------
# clang-tidy
# ---------------------------------------------------------------------------
$tidyN = 0
$tidyLog = Join-Path $Out 'clang-tidy.txt'
if (Test-Tool 'clang-tidy') {
    Write-Stage "clang-tidy ($((& clang-tidy --version) -join ' ' -replace '\s+', ' '))"
    # One process per source file, in parallel; per-process temp logs keep the
    # concurrent output from interleaving mid-line.
    $sets = @()
    foreach ($f in $Sources) { $sets += , @('-p', (Join-Path $Root $BuildDir), $f) }
    $run = Invoke-ThrottledProcesses -FilePath (Get-ToolPath 'clang-tidy') -ArgumentSets $sets
    $lines = @()
    foreach ($log in $run.Logs) {
        if (Test-Path $log) {
            $lines += @(Get-Content $log -ErrorAction SilentlyContinue |
                    Where-Object { $_ -match 'warning:|error:' })
        }
    }
    Remove-Item $run.TempDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-TextFile $tidyLog (($lines | Sort-Object -Unique) -join "`n")
    $tidyN = Get-LineCount $tidyLog
    Write-Host "clang-tidy findings: $tidyN (analysis-results\clang-tidy.txt)"
} else {
    Write-Stage 'clang-tidy'
    Write-Skip "clang-tidy not installed (winget install LLVM.LLVM, or pip install clang-tidy)"
    Write-TextFile $tidyLog ''
}

# ---------------------------------------------------------------------------
# Clang Static Analyzer (standalone: off-by-default checkers + deeper search)
# ---------------------------------------------------------------------------
$csaN = 0
$csaLog = Join-Path $Out 'clang-analyzer.txt'
Write-Stage 'Clang Static Analyzer'
# The driver itself decides whether a usable clang exists and exits 3 ("stage
# skipped") when it does not, so there is nothing to probe here. Any OTHER
# nonzero exit is a real failure and must not pass as "no findings".
$global:LASTEXITCODE = 0
$csaRan = Invoke-Python -Arguments @((Join-Path $Root 'tools\clang_analyzer.py'), $Db, $Root, $csaLog)
$csaRc = $LASTEXITCODE
if (-not $csaRan -and $csaRc -ne 3) {
    Write-Error "clang_analyzer.py failed (rc=$csaRc)"
    exit 1
}
if (-not (Test-Path $csaLog)) { Write-TextFile $csaLog '' }
$csaN = Get-LineCount $csaLog
Write-Host "clang-analyzer findings: $csaN (analysis-results\clang-analyzer.txt)"

# ---------------------------------------------------------------------------
# lizard (code metrics: complexity, function length, parameter count)
# ---------------------------------------------------------------------------
# Ratcheted against tools\lizard_baseline.json, so the finding count is
# deliberately NOT summed into the total — the driver's exit code is the gate.
Write-Stage 'lizard (code metrics)'
$global:LASTEXITCODE = 0
$lizardRan = Invoke-Python -Arguments @((Join-Path $Root 'tools\lizard_metrics.py'), $Root, $Out)
# 0 = clean, 1 = new/regressed/stale metric debt (fails), 3 = lizard not
# installed (stage skipped, stays green).
$lizardRc = $LASTEXITCODE
$lizardOk = ($lizardRan -or ($lizardRc -eq 3))
$lizardLog = Join-Path $Out 'lizard.txt'
if (-not (Test-Path $lizardLog)) { Write-TextFile $lizardLog '' }
$lizardN = Get-LineCount $lizardLog

# ---------------------------------------------------------------------------
# PMD CPD (copy-paste detection)
# ---------------------------------------------------------------------------
Write-Stage 'PMD CPD (copy-paste detection)'
$cpdLog = Join-Path $Out 'pmd-cpd.txt'
$global:LASTEXITCODE = 0
$cpdRan = Invoke-Python -Arguments @((Join-Path $Root 'tools\cpd_scan.py'), $Root, $cpdLog)
$cpdRc = $LASTEXITCODE
if (-not $cpdRan -and $cpdRc -ne 3) {
    Write-Error "cpd_scan.py failed (rc=$cpdRc)"
    exit 1
}
if (-not (Test-Path $cpdLog)) { Write-TextFile $cpdLog '' }
$cpdN = Get-LineCount $cpdLog
Write-Host "pmd-cpd findings: $cpdN (analysis-results\pmd-cpd.txt)"

# ---------------------------------------------------------------------------
# MSVC /analyze  (the Windows stand-in for g++ -fanalyzer)
# ---------------------------------------------------------------------------
$msvcN = 0
$msvcLog = Join-Path $Out 'msvc-analyze.txt'
$hasCl = (Import-MsvcEnvironment) -and (Test-Tool 'cl')
if ($hasCl) {
    Write-Stage 'MSVC /analyze'
    if (-not (Invoke-Python -Arguments @((Join-Path $Root 'tools\msvc_analyze.py'), $Db, $Root, $msvcLog))) {
        Write-Warning "msvc_analyze.py failed"
    }
    $msvcN = Get-LineCount $msvcLog
    Write-Host "MSVC /analyze findings: $msvcN (analysis-results\msvc-analyze.txt)"
} else {
    Write-Stage 'MSVC /analyze'
    Write-Skip "no MSVC toolset — the compiler-native analyzer pass is unavailable on this kit"
    Write-TextFile $msvcLog ''
}

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# qmllint - Qt's own QML static analysis over the declarative cockpit (REQ-F-038).
#
# Without this the Quick surface would be the one part of the application no analyzer
# looks at, and its characteristic defects (unqualified access inside a delegate, a
# property that does not exist on the target type) only surface at run time, in a
# binding, on a window nobody had open.
#
# -i <qmldir>: qmllint needs the generated MODULE to resolve TradingApp.Cockpit types and
# the Theme singleton. Without it every cross-file reference reports as unknown, which is
# noise rather than signal.
$qmllintN = 0
$qmllintLog = Join-Path $Out 'qmllint.txt'
$qmllintBin = $null
# Beside qmake6 first, then the Qt kits by explicit path. NOT a -Recurse over ~\Qt: that
# walks a multi-gigabyte install to find a file whose location is known.
$qmllintCandidates = @()
$qmakeCmd = Get-Command qmake6 -ErrorAction SilentlyContinue
if ($qmakeCmd) { $qmllintCandidates += (Join-Path (Split-Path -Parent $qmakeCmd.Source) 'qmllint.exe') }
$qmllintCandidates += @(Get-ChildItem -Path (Join-Path $env:USERPROFILE 'Qt') -Directory `
    -ErrorAction SilentlyContinue | ForEach-Object {
        Get-ChildItem -Path $_.FullName -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'bin\qmllint.exe' } })
foreach ($candidate in $qmllintCandidates) {
    if ($candidate -and (Test-Path $candidate)) { $qmllintBin = $candidate; break }
}
$qmldirFile = Get-ChildItem -Path (Join-Path $Root $BuildDir) -Filter 'qmldir' -Recurse `
    -ErrorAction SilentlyContinue | Where-Object { $_.FullName -like '*Cockpit*' } |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $qmllintBin) {
    Write-Stage 'qmllint'
    # Said out loud rather than skipped quietly: no qmllint means the QML is UNCHECKED,
    # which is not the same as clean.
    Write-Skip 'qmllint not found (it ships with the Qt kit) - the QML is unchecked'
    Write-TextFile $qmllintLog ''
} elseif (-not $qmldirFile) {
    Write-Stage 'qmllint'
    Write-Skip "no generated qmldir under $BuildDir - build the app first"
    Write-TextFile $qmllintLog ''
} else {
    Write-Stage "qmllint ($(& $qmllintBin --version))"
    $qmlFiles = @(Get-ChildItem -Path (Join-Path $Root 'src\quick\qml') -Filter '*.qml' `
        -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    $raw = @(& $qmllintBin -i $qmldirFile @qmlFiles 2>&1)
    $rows = @()
    foreach ($line in $raw) {
        # Normalised to the pipe template every other tool here emits, so the merged CSV
        # and the Axivion import treat qmllint exactly like cppcheck.
        #
        # The path group is LAZY and anchored on the ":line:col:" tail. A greedy or
        # [^:]+ group would take the drive letter of "C:\path\File.qml" as the filename
        # and the rest as the line number - the awk in static_analysis.sh matches the
        # same way, for the same reason.
        if ("$line" -match '^(?<sev>Warning|Error): (?<file>.+?):(?<line>\d+):\d+: (?<msg>.*)$') {
            $severity = if ($matches['sev'] -eq 'Error') { 'error' } else { 'warning' }
            $message = $matches['msg']
            $rule = 'qmllint'
            if ($message -match '^(?<text>.*) \[(?<rule>[a-z-]+)\]$') {
                $rule = $matches['rule']; $message = $matches['text']
            }
            $file = $matches['file'].Replace("$Root\", '').Replace('\', '/')
            $rows += "$file|$($matches['line'])|$severity|$rule|$message"
        }
    }
    Write-TextFile $qmllintLog (($rows -join "`n"))
    $qmllintN = Get-LineCount $qmllintLog
    Write-Host "qmllint findings: $qmllintN (analysis-results\qmllint.txt)"
}

# codespell
# ---------------------------------------------------------------------------
$codespellN = 0
$codespellLog = Join-Path $Out 'codespell.txt'
if (Test-Tool 'codespell') {
    Write-Stage "codespell ($(& codespell --version))"
    # Typos in comments, docs and scripts; config in .codespellrc. Output is
    # normalized to the pipe format so it lands on the Axivion dashboard.
    $targets = @('src', 'tests', 'tools', 'requirements', '.github', '.claude') |
        Where-Object { Test-Path (Join-Path $Root $_) }
    $targets += @(Get-ChildItem (Join-Path $Root 'docs') -Filter '*.md' -File -ErrorAction SilentlyContinue |
            ForEach-Object { "docs\$($_.Name)" })
    $targets += @(Get-ChildItem $Root -Filter '*.md' -File | ForEach-Object { $_.Name })
    $targets += @(Get-ChildItem $Root -Filter '*.sh' -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    $targets += @(Get-ChildItem $Root -Filter '*.ps1' -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })

    Push-Location $Root
    $raw = @(& codespell @targets 2>$null)
    Pop-Location
    $rows = @()
    foreach ($line in $raw) {
        # codespell prints "path:line: wrong ==> right"; on Windows the path
        # carries a drive letter, so split on the LAST colon-digit pair.
        if ($line -match '^(?<file>.+?):(?<line>\d+):\s*(?<msg>.*)$') {
            $rows += "$($matches['file'])|$($matches['line'])|warning|codespell|$($matches['msg'])"
        }
    }
    Write-TextFile $codespellLog (($rows -join "`n"))
    $codespellN = Get-LineCount $codespellLog
    Write-Host "codespell findings: $codespellN (analysis-results\codespell.txt)"
} else {
    Write-Stage 'codespell'
    Write-Skip "codespell not installed (pip install --user codespell) — typo check skipped"
    Write-TextFile $codespellLog ''
}

# ---------------------------------------------------------------------------
# clazy: Linux-only
# ---------------------------------------------------------------------------
Write-Stage 'clazy'
Write-Host "no Windows build of clazy exists — skipped." -ForegroundColor Yellow
Write-Host "Not a coverage gap: Axivion's Qt-* ruleset (~180 rules incl. the clazy checks," -ForegroundColor DarkGray
Write-Host "active in axivion/rule_config.json) checks the Qt coding rules on every axivion_ci run." -ForegroundColor DarkGray
Write-TextFile (Join-Path $Out 'clazy.txt') ''

# ---------------------------------------------------------------------------
# merged CSV for the dashboard import
# ---------------------------------------------------------------------------
Invoke-Python -Arguments @((Join-Path $Root 'tools\merge_findings.py'), $Out) | Out-Null

# Tool identity for the report: WHICH tool produced each verdict, at what version,
# invoked how, writing where. The versions were written to the console and nowhere
# else, so the qualification bundle could not answer that from its artefacts. The very
# same Python tool the Linux script calls, so the two platforms cannot drift.
Invoke-Python -Arguments @((Join-Path $Root 'tools\record_tool_versions.py'), $Out) |
    Out-Null

$total = $cppcheckN + $tidyN + $csaN + $cpdN + $msvcN + $codespellN + $qmllintN
Write-Host ""
Write-Host "TOTAL findings: $total" -ForegroundColor $(if ($total -eq 0) { 'Green' } else { 'Yellow' })
# The lizard metrics are reported separately: their violations are ratcheted
# against a recorded baseline, so a nonzero count is expected — the gate is
# whether the driver succeeded, not the count.
Write-Host "code metrics: $lizardN over-threshold findings, ratchet $(if ($lizardOk) { 'clean' } else { 'FAILED — see the lizard GATE lines above' })"
if ($total -eq 0 -and $cppcheckRc -eq 0 -and $lizardOk) { exit 0 }
exit 1
