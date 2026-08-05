<#
.SYNOPSIS
    Windows counterpart of ./build_all.sh — build every artefact this project
    produces, in dependency order.

.DESCRIPTION
      build      build\                    app executable + test binaries (Debug,
                                           with compile_commands.json for the analyzers)
      app        build\TradingApp.exe      the app executable ONLY (convenience
                                           stage, not part of the default run)
      release    build-release\TradingApp.exe  optimized RelWithDebInfo build for
                                           daily use / profiling (extra stage)
      vs         build-vs\TradingApp.sln   generated Visual Studio 2022 solution
                                           (extra stage; tools\make_vs_solution.ps1)
      deploy     build\*.dll + plugins     Qt runtime copied next to the exe so it
                                           runs with nothing on PATH (extra stage;
                                           tools\deploy_app.ps1)
      test       test-results\             JUnit XML per Qt Test function (tools\run_tests.ps1)
      trace      docs\traceability.html    REQ <-> DES <-> TS <-> result matrix
      docs       docs\html\                Doxygen + PlantUML (ships the trace matrix)
      coverage   coverage\…                OpenCppCoverage (line/branch, MSVC) and/or
                                           clang-cl + llvm-cov MC/DC reports
      analysis   analysis-results\         cppcheck + clang-tidy (+ MSVC /analyze) logs
                                           + merged CSV
      sanitize   analysis-results\         ASan run over the test suite; normalized
                                           findings log feeds the Axivion dashboard
      axivion    dashboard                 MISRA C++ 2023 + architecture analysis via
                                           axivion_ci (runs late: picks up fresh logs)
      report     downloads\*.pdf           one colour PDF summarising the whole run:
                                           test results per suite AND per function,
                                           traceability highlights, analyzer findings,
                                           metrics, coverage, sanitizers
                                           (tools\make_report.py; needs reportlab)

    A failing stage does not stop the later ones; the summary at the end lists
    every stage's result and the exit code is non-zero if anything failed.

.EXAMPLE
    .\build_all.ps1
    .\build_all.ps1 -Skip axivion
    .\build_all.ps1 build test
    .\build_all.ps1 vs                       # just the Visual Studio solution
    $env:QT_PREFIX = 'C:\Qt\6.9.2\msvc2022_64'; .\build_all.ps1

.NOTES
    Kit selection: the newest Qt kit containing Qt6Charts, MSVC preferred.
    Override with $env:QT_PREFIX, or pick a flavour with -QtKit mingw_64.
    The axivion stage alone drops back to the newest kit below 6.10, because
    the Suite's C++ front end asserts on Qt >= 6.10 headers (docs\windows.md).

    Counterpart: .\clean_all.ps1 removes everything these stages generate.
#>
[CmdletBinding()]
param(
    # Position = 0 is load-bearing: without an explicit position, PowerShell
    # also binds positionally to -Skip and -QtKit, so `build_all.ps1 build test
    # trace` would silently become Stages=build, Skip=test, QtKit=trace.
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Stages = @(),

    # Stage(s) to leave out of the default run — the Axivion run is by far the slowest.
    [string[]]$Skip = @(),

    # Qt kit directory name, e.g. mingw_64. Ignored when QT_PREFIX is set.
    [string]$QtKit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\tools\common.ps1"

$Root = Get-RepoRoot
$Jobs = Get-JobCount

$AllStages = @('build', 'test', 'trace', 'docs', 'coverage', 'analysis', 'sanitize', 'axivion', 'report')
# gui and testcenter are extra stages because both are licence-bound: Squish drives
# the GUI suite, Test Center stores every result. Each exits 3 ("skipped") without
# its licence, so naming them on a machine that has none reports skipped rather than
# failing — the quality PDF then lists which licence was missing. Lockstep with
# build_all.sh.
$ExtraStages = @('app', 'release', 'android', 'vs', 'deploy', 'gui', 'testcenter')   # selectable by name, not part of the default run

# ---------------------------------------------------------------------------
# toolchain
# ---------------------------------------------------------------------------

$QtPrefix = Resolve-QtPrefix -Kit $QtKit
if (-not $QtPrefix) {
    Write-Error "no usable Qt 6 kit found (needs Qt6Charts). Set QT_PREFIX, or run .\setup.ps1 install"
    exit 2
}
$env:QT_PREFIX = $QtPrefix
Add-QtToPath $QtPrefix

$IsMsvc = Test-QtIsMsvcKit $QtPrefix
$CxxCompiler = Initialize-KitToolchain -QtPrefix $QtPrefix
if ($null -eq $CxxCompiler) { exit 2 }

$Generator = 'Ninja'
if (-not (Test-Tool 'ninja')) {
    Write-Warning "ninja not found — falling back to the default CMake generator"
    $Generator = $null
}

function Invoke-CMakeConfigure {
    param([string]$BuildDir, [string[]]$ExtraArgs = @())
    Reset-StaleCMakeCache -BuildDir $BuildDir -SourceDir $Root -Generator $Generator `
        -QtPrefix $QtPrefix -Compiler $CxxCompiler
    $a = @('-S', $Root, '-B', $BuildDir)
    if ($Generator) { $a += @('-G', $Generator) }
    $a += @("-DCMAKE_PREFIX_PATH=$QtPrefix")
    if ($CxxCompiler) { $a += "-DCMAKE_CXX_COMPILER=$CxxCompiler" }
    $a += $ExtraArgs
    return (Invoke-Native -FilePath 'cmake' -Arguments $a)
}

function Invoke-CMakeBuild {
    param([string]$BuildDir, [string]$Target)
    $a = @('--build', $BuildDir, '-j', "$Jobs")
    if ($Target) { $a += @('--target', $Target) }
    return (Invoke-Native -FilePath 'cmake' -Arguments $a)
}

# ---------------------------------------------------------------------------
# stages
# ---------------------------------------------------------------------------

# Every Invoke-*Stage must return one of 'ok' / 'skipped' / 'FAILED' — NOT a
# boolean, or the summary prints "True" instead of "ok".
function Invoke-BuildStage {
    if (-not (Invoke-CMakeConfigure -BuildDir "$Root\build" -ExtraArgs @(
                '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                '-DTRADINGAPP_WARNINGS_AS_ERRORS=ON'))) { return 'FAILED' }
    return (ConvertFrom-Bool (Invoke-CMakeBuild -BuildDir "$Root\build"))
}

function Invoke-AppStage {
    if (-not (Invoke-CMakeConfigure -BuildDir "$Root\build" -ExtraArgs @(
                '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                '-DTRADINGAPP_WARNINGS_AS_ERRORS=ON'))) { return 'FAILED' }
    return (ConvertFrom-Bool (Invoke-CMakeBuild -BuildDir "$Root\build" -Target 'TradingApp'))
}

function Invoke-ReleaseStage {
    # Optimized build for daily use and profiling. Frame pointers stay in so
    # the profilers produce usable stacks (see tools\profile.ps1).
    $flags = '/Oy-'
    if (-not $IsMsvc) { $flags = '-fno-omit-frame-pointer' }
    if (-not (Invoke-CMakeConfigure -BuildDir "$Root\build-release" -ExtraArgs @(
                '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DCMAKE_CXX_FLAGS=$flags"))) { return 'FAILED' }
    if (-not (Invoke-CMakeBuild -BuildDir "$Root\build-release")) { return 'FAILED' }
    Write-Host "release binary: build-release\TradingApp.exe"
    return 'ok'
}

# Stage outcomes are tri-state: 'ok', 'skipped' or 'FAILED'. Exit code 3 from a
# child script means "skipped": it needs a tool that is license-bound (Axivion
# Suite, Squish Coco) or otherwise absent. That must NOT fail the pipeline, so a
# machine without those licenses still finishes green. Same convention as
# build_all.sh.
$EXIT_SKIPPED = 3

function ConvertTo-StageResult {
    param([int]$Code)
    if ($Code -eq 0) { return 'ok' }
    if ($Code -eq $EXIT_SKIPPED) { return 'skipped' }
    return 'FAILED'
}

function ConvertFrom-Bool {
    param([bool]$Ok)
    if ($Ok) { return 'ok' }
    return 'FAILED'
}

# Child output is funnelled through Write-Host for the same reason as in
# Invoke-Native: it must not land in this function's return value (or the stage
# reports ok merely for being chatty), yet must stay redirectable so
# `build_all.ps1 *> log.txt` and the CI artifact upload capture it. Out-Host
# would satisfy only the first and produce an almost-empty log.
function Invoke-TestStage { & "$Root\tools\run_tests.ps1" -BuildDir 'build' | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
function Invoke-TraceStage { return (ConvertFrom-Bool (Invoke-Python -Arguments @("$Root\tools\trace_report.py"))) }
function Invoke-DocsStage { & "$Root\tools\make_docs.ps1" | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
function Invoke-CoverageStage { & "$Root\tools\coverage.ps1" -Mode auto | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
function Invoke-AnalysisStage { & "$Root\tools\static_analysis.ps1" -BuildDir 'build' | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
function Invoke-SanitizeStage { & "$Root\tools\sanitize.ps1" -Mode all | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
function Invoke-AxivionStage { & "$Root\axivion\start_analysis.ps1" | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
# LAST stage on purpose: it summarises the artefacts every stage above wrote into one
# colour PDF (tools/make_report.py — shared verbatim with build_all.sh). The script
# itself exits 3 when reportlab is missing, so a machine without it reports `skipped`
# instead of failing a pipeline whose actual checks passed.
function Invoke-ReportStage {
    $py = Get-Python
    if (-not $py) { Write-Warning 'no Python interpreter found — no PDF report'; return 'skipped' }
    & $py.Exe @($py.Args + @("$Root\tools\make_report.py", '--build-dir', 'build')) |
        ForEach-Object { Write-Host $_ }
    return (ConvertTo-StageResult $LASTEXITCODE)
}
# Extra stage (named only): the Android APK, via the shared tools\build_android.ps1.
# Exits 3 = skipped without a Qt Android kit or SDK, so naming it on a desktop-only
# machine reports skipped rather than failing.
function Invoke-AndroidStage { & "$Root\tools\build_android.ps1" -Abi android_arm64_v8a | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
function Invoke-VsStage { & "$Root\tools\make_vs_solution.ps1" | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
function Invoke-DeployStage { & "$Root\tools\deploy_app.ps1" -BuildDir 'build' | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
# Extra stages (named only), both licence-bound and both exit 3 without a licence:
#  gui         the Squish GUI suite, FORCED into simulation so it can never reach a
#              real account (tools\squish_run.ps1 explains the guarantee)
#  testcenter  every JUnit XML in test-results\ uploaded to Qt Test Center
function Invoke-GuiStage { & "$Root\tools\squish_run.ps1" -BuildDir 'build' | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }
function Invoke-TestcenterStage { & "$Root\tools\testcenter_upload.ps1" | ForEach-Object { Write-Host $_ }; return (ConvertTo-StageResult $LASTEXITCODE) }

# ---------------------------------------------------------------------------
# stage selection
# ---------------------------------------------------------------------------

$known = $AllStages + $ExtraStages
foreach ($s in ($Stages + $Skip)) {
    if ($known -notcontains $s) {
        Write-Host "unknown stage: $s" -ForegroundColor Red
        Write-Host "usage: .\build_all.ps1 [stage ...] [-Skip stage[,stage]]   stages: $($AllStages -join ', ')   (default: all)"
        Write-Host "       extra stages (only when named): $($ExtraStages -join ', ')"
        exit 2
    }
}
if ($Stages.Count -eq 0) { $Stages = $AllStages }
if ($Skip.Count -gt 0) { $Stages = @($Stages | Where-Object { $Skip -notcontains $_ }) }

$result = [ordered]@{}
$fail = 0
$skipped = 0
foreach ($s in $Stages) {
    Write-Stage $s
    $outcome = 'FAILED'
    try { $outcome = & "Invoke-$($s)Stage" } catch { Write-Host $_.Exception.Message -ForegroundColor Red }
    $result[$s] = $outcome
    if ($outcome -eq 'FAILED') { $fail = 1 }
    elseif ($outcome -eq 'skipped') { $skipped++ }
}

Write-Stage 'summary'
foreach ($s in $Stages) {
    $color = 'Red'
    if ($result[$s] -eq 'ok') { $color = 'Green' }
    elseif ($result[$s] -eq 'skipped') { $color = 'Yellow' }
    Write-Host ("  {0,-10} {1}" -f $s, $result[$s]) -ForegroundColor $color
}
if ($skipped -gt 0) {
    Write-Host "  ($skipped stage(s) skipped - a required tool is unavailable; see the log above)" -ForegroundColor DarkGray
}
exit $fail
