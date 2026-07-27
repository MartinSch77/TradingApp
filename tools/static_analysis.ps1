<#
.SYNOPSIS
    Windows counterpart of tools/static_analysis.sh — static analysis over the
    app sources.

.DESCRIPTION
    cppcheck + clang-tidy + MSVC /analyze, plus codespell when installed.
    Reports land in analysis-results\ as one plain-text log per tool — the next
    axivion_ci run imports those onto the Axivion dashboard (see
    axivion/external_import.py) — plus one merged CSV as a single-file overview.
    Exit code 1 when any tool reported findings.

    Tool mapping against the Linux script:
      cppcheck      same tool, same flags
      clang-tidy    same tool (LLVM for Windows, or the pip clang-tidy wheel)
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

$Sources = @()
foreach ($pat in @('src\domain\*.cpp', 'src\services\*.cpp', 'src\ui\*.cpp', 'src\main.cpp')) {
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
    # include paths and defines match the real build), the warning/performance/
    # portability set, --inconclusive, --error-exitcode=1, --library=qt, the
    # autogen/tests suppressions and the pipe template the dashboard imports.
    & cppcheck "--project=$Db" `
        --enable=warning,performance,portability `
        --inconclusive `
        --error-exitcode=1 `
        --inline-suppr `
        "--suppressions-list=$(Join-Path $Root 'tools\cppcheck-suppressions.txt')" `
        --library=qt `
        -i (Join-Path $Root $BuildDir) --suppress='*:*autogen*' --suppress='*:*/tests/*' `
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

$total = $cppcheckN + $tidyN + $msvcN + $codespellN
Write-Host ""
Write-Host "TOTAL findings: $total" -ForegroundColor $(if ($total -eq 0) { 'Green' } else { 'Yellow' })
if ($total -eq 0 -and $cppcheckRc -eq 0) { exit 0 }
exit 1
