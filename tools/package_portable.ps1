<#
.SYNOPSIS
    Windows counterpart of tools/package_appimage.sh — build a portable ZIP of
    the app, with every DLL it needs, into downloads\.

.DESCRIPTION
    1. Release build in build-portable\ (its own tree, so it cannot inherit the
       Debug/coverage/sanitizer flags of the others),
    2. `cmake --install` into a staging directory: that runs windeployqt through
       the install rules in CMakeLists.txt, which brings the Qt DLLs, the
       platform plugin (platforms\qwindows.dll), the Schannel TLS backend the
       eToro API needs, image formats, styles and the compiler runtime,
    3. CMAKE_INSTALL_BINDIR=. so TradingApp.exe sits at the root of the archive
       rather than in bin\ — that is what "portable" means to whoever unzips it,
    4. config.json + apiKeyEtoro.example.json + a short readme alongside it,
    5. output: downloads\TradingApp-<version>-windows-x64.zip (+ .sha256).

    The archive is self-contained: no Qt install, no MSVC redistributable and
    nothing on PATH is required on the target machine.

.PARAMETER SkipBuild
    Package what is already in build-portable\ instead of rebuilding.

.PARAMETER QtKit
    Qt kit to build against (passed to Resolve-QtPrefix). Default: auto-detect,
    same rule as build_all.ps1.

.EXAMPLE
    tools\package_portable.ps1
    tools\package_portable.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$QtKit = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot
$Build = Join-Path $Root 'build-portable'
$OutDir = Join-Path $Root 'downloads'

$qtPrefix = if ($QtKit) { Resolve-QtPrefix -Kit $QtKit } else { Resolve-QtPrefix }
if (-not $qtPrefix) {
    Write-Error "no Qt kit found — run .\setup.ps1 install (it installs Qt with -m qtcharts)"
    exit 1
}
if (-not (Test-Path (Join-Path $qtPrefix 'lib\cmake\Qt6Charts'))) {
    Write-Error "the Qt kit at $qtPrefix has no Charts module — the app links against it"
    exit 1
}

# Version from the single place that defines it: project(... VERSION x.y.z).
$version = '0.0.0'
$match = Select-String -Path (Join-Path $Root 'CMakeLists.txt') `
    -Pattern 'VERSION (\d+\.\d+\.\d+)' | Select-Object -First 1
if ($match) { $version = $match.Matches[0].Groups[1].Value }

$stageName = "TradingApp-$version-windows-x64"
$stage = Join-Path $Build $stageName
$zip = Join-Path $OutDir "$stageName.zip"

Write-Stage "portable ZIP: TradingApp $version (Qt at $qtPrefix)"

Reset-StaleCMakeCache -BuildDir $Build -SourceDir $Root -QtPrefix $qtPrefix

if (-not $SkipBuild) {
    if (-not (Invoke-Native -FilePath 'cmake' -Arguments @(
                '-S', $Root, '-B', $Build,
                "-DCMAKE_PREFIX_PATH=$qtPrefix",
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_INSTALL_BINDIR=.',
                '-DTRADINGAPP_WARNINGS_AS_ERRORS=ON'))) {
        Write-Error "configure failed"
        exit 1
    }
    if (-not (Invoke-Native -FilePath 'cmake' -Arguments @(
                '--build', $Build, '--target', 'TradingApp',
                '--config', 'Release', '-j', (Get-JobCount)))) {
        Write-Error "build failed"
        exit 1
    }
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (-not (Invoke-Native -FilePath 'cmake' -Arguments @(
            '--install', $Build, '--prefix', $stage, '--config', 'Release'))) {
    Write-Error "install (windeployqt) failed"
    exit 1
}

$exe = Join-Path $stage 'TradingApp.exe'
if (-not (Test-Path $exe)) {
    Write-Error "install produced no TradingApp.exe in $stage — nothing to package"
    exit 1
}
# windeployqt is the whole point of this archive: if the platform plugin is
# missing the app dies with "This application failed to start because no Qt
# platform plugin could be initialized" on the user's machine, not here.
if (-not (Test-Path (Join-Path $stage 'platforms\qwindows.dll'))) {
    Write-Error "no platforms\qwindows.dll in $stage — windeployqt did not run"
    exit 1
}

Copy-Item (Join-Path $Root 'config.json') $stage -Force
Copy-Item (Join-Path $Root 'apiKeyEtoro.example.json') $stage -Force
@"
eToro Trader $version — portable Windows build
==============================================

Unzip anywhere and run TradingApp.exe. Everything it needs is in this folder:
no Qt installation, no Visual C++ redistributable, nothing on PATH.

Without API keys the app runs in SIMULATION mode. To trade for real:
  1. Copy apiKeyEtoro.example.json to apiKeyEtoro.json (next to the exe).
  2. Fill in apiKey / userKey from https://api-portal.etoro.com/ .
  3. Set "mode": "real" in config.json — "demo" keeps it on the demo account.
Never share apiKeyEtoro.json: it is the key to a real trading account.

Source, documentation and the quality reports:
https://github.com/MartinSch77/TradingApp
"@ | Set-Content (Join-Path $stage 'README.txt') -Encoding UTF8

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

$size = '{0:N1} MB' -f ((Get-Item $zip).Length / 1MB)
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ASCII
Write-Host ""
Write-Host "portable ZIP: $zip ($size)" -ForegroundColor Green
Write-Host "$hash"
