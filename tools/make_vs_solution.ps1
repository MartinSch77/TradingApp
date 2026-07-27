<#
.SYNOPSIS
    Generate a Visual Studio 2022 solution for TradingApp.

.DESCRIPTION
    CMake is the single source of truth for the build (DES-BLD-CMAKE), so the
    .sln is GENERATED rather than committed — a hand-maintained solution would
    be a second, drifting description of the same targets. This script runs the
    Visual Studio generator over the same CMakeLists.txt:

        build-vs\TradingApp.sln     — TradingApp, trading_domain,
                                      trading_services and all 12 tst_* test
                                      projects, in Debug/Release/RelWithDebInfo

    Re-run it after adding or removing source files; Visual Studio also
    re-runs CMake itself when CMakeLists.txt changes.

    Two other ways to open this project in Visual Studio, both without a .sln:
      * File > Open > Folder on the repository root — VS reads CMakePresets.json
        and offers the windows-msvc-debug / visual-studio presets directly.
      * File > Open > CMake... and pick CMakeLists.txt.

.PARAMETER StartupProject
    Project selected as the startup project in the generated solution.
    Default: TradingApp (so F5 runs the app, not a test binary).

.PARAMETER Open
    Open the generated solution in Visual Studio afterwards.

.EXAMPLE
    tools\make_vs_solution.ps1
    tools\make_vs_solution.ps1 -Open
    tools\make_vs_solution.ps1 -StartupProject tst_tradeplan
#>
[CmdletBinding()]
param(
    [string]$StartupProject = 'TradingApp',
    [string]$QtKit,
    [switch]$Open
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot
$BuildDir = Join-Path $Root 'build-vs'

$QtPrefix = Resolve-QtPrefix -Kit $QtKit
if (-not $QtPrefix) {
    Write-Error "no usable Qt 6 kit found (needs Qt6Charts). Set QT_PREFIX, or run .\setup.ps1 install"
    exit 2
}
if (-not (Test-QtIsMsvcKit $QtPrefix)) {
    Write-Error "the Visual Studio generator needs an MSVC Qt kit; got $QtPrefix. Use -QtKit msvc2022_64."
    exit 2
}
$env:QT_PREFIX = $QtPrefix

# The Visual Studio generator locates the toolset itself, so no vcvars import is
# needed here — but importing it keeps the Qt tools and cl on PATH for anything
# that runs afterwards in this session.
Import-MsvcEnvironment | Out-Null

Write-Stage 'Visual Studio 2022 solution'

# VS_STARTUP_PROJECT is a directory property CMake maps onto the .sln, so the
# solution opens with the app selected rather than the alphabetically-first
# target (ALL_BUILD).
Reset-StaleCMakeCache -BuildDir $BuildDir -SourceDir $Root -Generator 'Visual Studio 17 2022'

$ok = Invoke-Native -FilePath 'cmake' -Arguments @(
    '-S', $Root, '-B', $BuildDir,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$QtPrefix",
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    "-DTRADINGAPP_VS_STARTUP_PROJECT=$StartupProject"
)
if (-not $ok) {
    Write-Error "CMake configure failed — see the output above"
    exit 1
}

$sln = Join-Path $BuildDir 'TradingApp.sln'
if (-not (Test-Path $sln)) {
    Write-Error "the generator did not produce $sln"
    exit 1
}

Write-Host ""
Write-Host "solution: $sln" -ForegroundColor Green
Write-Host "  startup project: $StartupProject   (F5 runs it)"
Write-Host "  configurations : Debug, Release, RelWithDebInfo, MinSizeRel"
Write-Host "  tests          : Test > Run All Tests, or  ctest --test-dir build-vs -C Debug"
Write-Host ""
Write-Host "The .sln is generated from CMakeLists.txt — do not edit project settings in the IDE," -ForegroundColor DarkGray
Write-Host "change CMakeLists.txt and re-run this script." -ForegroundColor DarkGray

if ($Open) { Start-Process $sln }
