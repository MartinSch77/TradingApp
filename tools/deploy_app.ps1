<#
.SYNOPSIS
    Copy the Qt runtime next to TradingApp.exe so it runs straight from the
    build directory, with nothing on PATH.

.DESCRIPTION
    Runs windeployqt against the built executable: Qt DLLs, the platform plugin
    (platforms\qwindows.dll), the TLS backend Schannel needs for the eToro API,
    image formats, styles — plus the compiler runtime.

    The Qt kit is read from the build tree's CMakeCache.txt, NOT from the
    current PATH or from kit auto-detection. Deploying Qt 6.11.1 release DLLs
    next to a binary linked against 6.9.2 debug ones produces an executable
    that starts and then dies on the first Qt call, so the deployment has to
    follow the binary, not the environment.

    A MinGW build additionally needs libstdc++-6.dll, libgcc_s_seh-1.dll and
    libwinpthread-1.dll from the toolchain. windeployqt --compiler-runtime
    finds them only when the toolchain is on PATH, so they are copied
    explicitly here.

.PARAMETER BuildDir
    Build tree holding the executable. Default: build

.PARAMETER Target
    Executable to deploy around. Default: TradingApp.exe

.PARAMETER IncludeTests
    Also deploy next to the Qt Test binaries, so they run standalone too.

.PARAMETER Config
    Configuration subdirectory to prefer in a multi-config build tree
    (the Visual Studio generator puts the exe in build-vs\Debug\ etc.).
    Default: auto — the first configuration that actually contains the binary.

.EXAMPLE
    tools\deploy_app.ps1
    tools\deploy_app.ps1 -BuildDir build-release
    tools\deploy_app.ps1 -BuildDir build-vs                 # VS solution, auto-picks Debug
    tools\deploy_app.ps1 -BuildDir build-vs -Config Release
    tools\deploy_app.ps1 -IncludeTests
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$Target = 'TradingApp.exe',
    [string]$Config,
    [switch]$IncludeTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot

if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $Root $BuildDir }

# Ninja puts the binary in the build root; the Visual Studio generator puts it
# in a per-configuration subdirectory. Look in both, so the same command works
# for build\, build-release\ and build-vs\.
$candidates = @()
if ($Config) { $candidates += (Join-Path $BuildDir (Join-Path $Config $Target)) }
$candidates += (Join-Path $BuildDir $Target)
foreach ($c in @('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')) {
    $candidates += (Join-Path $BuildDir (Join-Path $c $Target))
}
$exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) {
    Write-Error "not found: $Target under $BuildDir  (build it with: .\build_all.ps1 app, or open the VS solution)"
    exit 2
}

# --- the kit this binary was actually built against ------------------------
$cache = Join-Path $BuildDir 'CMakeCache.txt'
$qtPrefix = $null
if (Test-Path $cache) {
    foreach ($line in (Get-Content $cache)) {
        if ($line -match '^CMAKE_PREFIX_PATH:[A-Z]+=(.+)$') { $qtPrefix = $matches[1].Trim(); break }
    }
}
if (-not $qtPrefix -or -not (Test-Path $qtPrefix)) {
    Write-Warning "no usable CMAKE_PREFIX_PATH in $cache - falling back to kit auto-detection"
    $qtPrefix = Resolve-QtPrefix -Quiet
}
if (-not $qtPrefix) { Write-Error "cannot determine the Qt kit to deploy from"; exit 2 }

$windeployqt = Join-Path $qtPrefix 'bin\windeployqt.exe'
if (-not (Test-Path $windeployqt)) {
    Write-Error "windeployqt not found at $windeployqt"
    exit 2
}

Write-Stage "deploy Qt runtime next to $Target"
Write-Host "kit: $qtPrefix" -ForegroundColor DarkGray

# windeployqt itself needs the kit's bin on PATH (it loads Qt6Core to inspect
# the binary), and for a MinGW build the toolchain too.
Add-QtToPath $qtPrefix
$kitCxx = Initialize-KitToolchain -QtPrefix $qtPrefix
if ($null -eq $kitCxx) { exit 2 }

$targets = @($exe)
if ($IncludeTests) {
    # tests\ for Ninja, tests\<Config>\ for the Visual Studio generator.
    $testRoot = Join-Path $BuildDir 'tests'
    $targets += @(Get-ChildItem $testRoot -Filter 'tst_*.exe' -File -Recurse -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })
}

$failed = 0
foreach ($t in $targets) {
    # --compiler-runtime brings the CRT along; the debug/release flavour is
    # detected from the binary itself, so it is not passed explicitly.
    $ok = Invoke-Native -FilePath $windeployqt -Arguments @(
        '--compiler-runtime'
        '--no-translations'      # ~30 .qm files this app never loads
        '--no-system-d3d-compiler'
        '--verbose', '1'
        $t
    )
    if (-not $ok) { Write-Warning "windeployqt failed for $t"; $failed++ }
}

# --- MinGW runtime ---------------------------------------------------------
if (-not (Test-QtIsMsvcKit $qtPrefix)) {
    $mingwBin = $null
    if ($kitCxx) { $mingwBin = Split-Path ($kitCxx -replace '/', '\') -Parent }
    if ($mingwBin -and (Test-Path $mingwBin)) {
        foreach ($dll in @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
            $src = Join-Path $mingwBin $dll
            if (Test-Path $src) {
                foreach ($t in $targets) { Copy-Item $src (Split-Path $t -Parent) -Force }
                Write-Host "  copied $dll" -ForegroundColor DarkGray
            }
        }
    }
}

# --- report ----------------------------------------------------------------
$dir = Split-Path $exe -Parent
$dlls = @(Get-ChildItem $dir -Filter '*.dll' -File -ErrorAction SilentlyContinue)
$plugins = @(Get-ChildItem $dir -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @('platforms', 'tls', 'imageformats', 'styles', 'iconengines', 'networkinformation', 'generic') })
$bytes = 0L
foreach ($f in @(Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue)) { $bytes += $f.Length }

Write-Host ""
Write-Host "$($dlls.Count) DLLs + $($plugins.Count) plugin directories in $dir" -ForegroundColor Green
Write-Host ("  plugin dirs: " + (($plugins | ForEach-Object { $_.Name }) -join ', '))
Write-Host ("  total size : {0:N1} MB" -f ($bytes / 1MB))
Write-Host ""
# A debug build gets the debug-suffixed plugin (qwindowsd.dll), a release build
# qwindows.dll - accept either, and fail loudly if neither is there, because
# without it the app aborts with "no Qt platform plugin could be initialized".
$platformPlugin = @(Get-ChildItem (Join-Path $dir 'platforms') -Filter 'qwindows*.dll' -File -ErrorAction SilentlyContinue)
if ($platformPlugin.Count -eq 0) {
    Write-Warning "no platforms\qwindows*.dll - the app will abort with 'no Qt platform plugin could be initialized'"
    $failed++
} else {
    Write-Host "  platform plugin: platforms\$($platformPlugin[0].Name)" -ForegroundColor DarkGray
}
$tlsBackends = @(Get-ChildItem (Join-Path $dir 'tls') -Filter '*.dll' -File -ErrorAction SilentlyContinue)
if ($tlsBackends.Count -eq 0) {
    Write-Warning "no tls\ backend - HTTPS to the eToro API will fail (SIMULATION mode still works)"
} else {
    Write-Host "  TLS backends   : $((($tlsBackends | ForEach-Object { $_.Name }) -join ', '))" -ForegroundColor DarkGray
}

Write-Host ""
if ($failed -gt 0) { exit 1 }
Write-Host "$Target is now self-contained: run it directly, no PATH setup needed." -ForegroundColor Green
