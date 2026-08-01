<#
.SYNOPSIS
    Build the Android APK from the same CMake project the desktops use (REQ-N-001).

.DESCRIPTION
    The Windows counterpart of tools/build_android.sh — same contract, same output
    name, same exit codes (3 = SKIPPED when a prerequisite is missing, so a pipeline
    stays green on a machine without an Android toolchain).

    Needs, and checks rather than assumes:
      * a Qt for Android kit ($env:USERPROFILE\Qt\<ver>\<abi>). aqt serves Android
        from the `all_os` host for Qt >= 6.8, NOT `windows`:
            aqt install-qt all_os android <ver> <abi> -m qtcharts -O $HOME\Qt
      * the Android SDK plus the NDK VERSION QT WAS BUILT AGAINST — read out of the
        kit here instead of hardcoded, because a mismatched NDK links a broken APK.
      * a JDK 17+ for the gradle run androiddeployqt performs.

.PARAMETER Abi
    android_x86_64 (emulator on an x86_64 host) or android_arm64_v8a (devices).

.PARAMETER Release
    Release build, signed so the download is installable: with
    $env:TRADINGAPP_KEYSTORE / _KEYSTORE_PASS / _KEY_ALIAS when set, else with
    the standard Android debug keystore (created on demand). Only store
    distribution needs a real key.

.PARAMETER Run
    After building: boot an AVD, install, launch, screenshot (tools\run_android.ps1).

.EXAMPLE
    .\tools\build_android.ps1
    .\tools\build_android.ps1 -Abi android_arm64_v8a -Release
    .\tools\build_android.ps1 -Run
#>
[CmdletBinding()]
param(
    [ValidateSet('android_x86_64', 'android_arm64_v8a', 'android_armv7', 'android_x86')]
    [string]$Abi = 'android_x86_64',

    [switch]$Release,
    [switch]$Run,

    # Android SDK root. Default matches Android Studio's own location.
    [string]$Sdk
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\common.ps1"

$Root = Split-Path -Parent $PSScriptRoot
$EXIT_SKIPPED = 3

function Write-Skipped {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host "SKIPPED: $Message" -ForegroundColor Yellow
    exit $EXIT_SKIPPED
}

if (-not $Sdk) {
    if ($env:ANDROID_HOME) { $Sdk = $env:ANDROID_HOME }
    elseif ($env:ANDROID_SDK_ROOT) { $Sdk = $env:ANDROID_SDK_ROOT }
    else { $Sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
}

# --- the Qt kit for this ABI ------------------------------------------------
# Newest kit that has BOTH the ABI and Charts: this app cannot build without
# Charts, and picking "newest" blindly yields a configure error instead.
$qtRoot = Join-Path $env:USERPROFILE 'Qt'
$kit = $null
if (Test-Path $qtRoot) {
    $kit = Get-ChildItem -Path $qtRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object { $_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName $Abi } |
        Where-Object { Test-Path (Join-Path $_ 'lib\cmake\Qt6Charts\Qt6ChartsConfig.cmake') } |
        Select-Object -First 1
}
if (-not $kit) {
    Write-Skipped "no Qt for Android kit with Charts for $Abi (aqt install-qt all_os android <ver> $Abi -m qtcharts -O `"$qtRoot`")"
}
$qtVersion = Split-Path -Leaf (Split-Path -Parent $kit)

# The matching DESKTOP kit builds the host tools androiddeployqt runs.
$hostKit = Join-Path $qtRoot "$qtVersion\msvc2022_64"
if (-not (Test-Path $hostKit)) {
    $hostKit = Get-ChildItem -Path (Join-Path $qtRoot $qtVersion) -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'msvc*' -or $_.Name -like 'mingw*' } |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $hostKit) { Write-Skipped "the Android kit needs the matching desktop kit as its host: $qtRoot\$qtVersion\msvc2022_64" }

# --- SDK / NDK -------------------------------------------------------------
if (-not (Test-Path (Join-Path $Sdk 'platform-tools\adb.exe'))) {
    Write-Skipped "no Android SDK at $Sdk (see .\setup.ps1 android)"
}

# The NDK version stamped through the kit's CMake files is the one to use.
$ndkWant = Select-String -Path (Join-Path $kit 'lib\cmake\*\*.cmake') -Pattern '2[0-9]\.[0-9]\.[0-9]{8}' `
    -AllMatches -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Value } |
    Group-Object | Sort-Object Count -Descending | Select-Object -First 1 -ExpandProperty Name
$ndk = $null
if ($ndkWant -and (Test-Path (Join-Path $Sdk "ndk\$ndkWant"))) {
    $ndk = Join-Path $Sdk "ndk\$ndkWant"
} else {
    $ndk = Get-ChildItem -Path (Join-Path $Sdk 'ndk') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $ndk) { Write-Skipped "no NDK under $Sdk\ndk (Qt $qtVersion expects $ndkWant)" }
    if ($ndkWant -and ((Split-Path -Leaf $ndk) -ne $ndkWant)) {
        Write-Warning "Qt $qtVersion was built with NDK $ndkWant, using $(Split-Path -Leaf $ndk)"
    }
}

if (-not (Test-Tool 'java')) { Write-Skipped 'no JDK on PATH (androiddeployqt runs gradle)' }

$buildType = 'Debug'
if ($Release) { $buildType = 'Release' }
$buildDir = Join-Path $Root ("build-android-" + ($Abi -replace '^android_', ''))

Write-Host "Qt kit   : $kit"
Write-Host "host kit : $hostKit"
Write-Host "SDK / NDK: $Sdk  /  $(Split-Path -Leaf $ndk)"
Write-Host "build    : $buildDir ($buildType)"

# --- configure + build -----------------------------------------------------
# The kit's qt-cmake wrapper sets the NDK toolchain file, the ABI and the host
# tools; plain cmake here is the classic route to an APK that will not load.
$qtCmake = Join-Path $kit 'bin\qt-cmake.bat'
if (-not (Test-Path $qtCmake)) { $qtCmake = Join-Path $kit 'bin\qt-cmake' }

$ok = Invoke-Native -FilePath $qtCmake -Arguments @(
    '-S', $Root, '-B', $buildDir, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$buildType",
    "-DQT_HOST_PATH=$hostKit",
    "-DANDROID_SDK_ROOT=$Sdk",
    "-DANDROID_NDK_ROOT=$ndk")
if (-not $ok) { Write-Host 'configure FAILED' -ForegroundColor Red; exit 1 }

$ok = Invoke-Native -FilePath 'cmake' -Arguments @('--build', $buildDir, '--parallel')
if (-not $ok) { Write-Host 'build FAILED' -ForegroundColor Red; exit 1 }

# The plain build only produces the .so; the apk target runs androiddeployqt.
$ok = Invoke-Native -FilePath 'cmake' -Arguments @('--build', $buildDir, '--target', 'apk')
if (-not $ok) { Write-Host 'androiddeployqt FAILED' -ForegroundColor Red; exit 1 }

$apk = Get-ChildItem -Path (Join-Path $buildDir 'android-build\build\outputs\apk') -Filter '*.apk' `
    -Recurse -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
if (-not $apk) { Write-Host 'androiddeployqt produced no APK - see the log above' -ForegroundColor Red; exit 1 }

$version = '0.0.0'
$m = Select-String -Path (Join-Path $Root 'CMakeLists.txt') -Pattern 'VERSION ([0-9]+\.[0-9]+\.[0-9]+)' |
    Select-Object -First 1
if ($m) { $version = $m.Matches[0].Groups[1].Value }

$downloads = Join-Path $Root 'downloads'
New-Item -ItemType Directory -Path $downloads -Force | Out-Null
$out = Join-Path $downloads ("TradingApp-$version-" + ($Abi -replace '^android_', '') + "-" + $buildType.ToLower() + ".apk")
Copy-Item $apk.FullName $out -Force

# A Release APK leaves androiddeployqt UNSIGNED and Android refuses to install
# it — sign it here so the download is sideloadable (same contract as the .sh:
# $env:TRADINGAPP_KEYSTORE / _KEYSTORE_PASS / _KEY_ALIAS, defaulting to the
# standard Android debug keystore, created on demand). zipalign BEFORE
# apksigner (the v2+ signature covers the aligned bytes), then verify, so a
# broken signing config fails HERE rather than on the user's phone. An upgrade
# install needs the SAME key as the previous build - keep one keystore.
if ($buildType -eq 'Release') {
    $bt = Get-ChildItem -Path (Join-Path $Sdk 'build-tools') -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $bt) { Write-Skipped "no Android build-tools for signing (sdkmanager 'build-tools;36.0.0')" }
    $ks = if ($env:TRADINGAPP_KEYSTORE) { $env:TRADINGAPP_KEYSTORE }
          else { Join-Path $env:USERPROFILE '.android\debug.keystore' }
    $ksPass = if ($env:TRADINGAPP_KEYSTORE_PASS) { $env:TRADINGAPP_KEYSTORE_PASS } else { 'android' }
    $ksAlias = if ($env:TRADINGAPP_KEY_ALIAS) { $env:TRADINGAPP_KEY_ALIAS } else { 'androiddebugkey' }
    if (-not (Test-Path -LiteralPath $ks)) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $ks) -Force | Out-Null
        $ok = Invoke-Native -FilePath 'keytool' -Arguments @(
            '-genkeypair', '-keystore', $ks, '-storepass', $ksPass, '-alias', $ksAlias,
            '-keypass', $ksPass, '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
            '-dname', 'CN=Android Debug,O=Android,C=US')
        if (-not $ok) { Write-Host 'keystore creation FAILED' -ForegroundColor Red; exit 1 }
    }
    $ok = Invoke-Native -FilePath (Join-Path $bt 'zipalign.exe') -Arguments @('-f', '4', $out, "$out.aligned")
    if (-not $ok) { Write-Host 'zipalign FAILED' -ForegroundColor Red; exit 1 }
    Move-Item -Force -LiteralPath "$out.aligned" -Destination $out
    $ok = Invoke-Native -FilePath (Join-Path $bt 'apksigner.bat') -Arguments @(
        'sign', '--ks', $ks, '--ks-pass', "pass:$ksPass",
        '--ks-key-alias', $ksAlias, '--key-pass', "pass:$ksPass", $out)
    if (-not $ok) { Write-Host 'apksigner sign FAILED' -ForegroundColor Red; exit 1 }
    $ok = Invoke-Native -FilePath (Join-Path $bt 'apksigner.bat') -Arguments @('verify', $out)
    if (-not $ok) { Write-Host 'apksigner verify FAILED' -ForegroundColor Red; exit 1 }
    Write-Host "signed with $ks ($ksAlias)"
}

# Same convention as the AppImage: a checksum beside every downloadable.
$hash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path -Leaf $out)" | Set-Content -Path "$out.sha256" -NoNewline
Write-Host "APK: $out  ($([math]::Round((Get-Item $out).Length / 1MB, 1)) MB)" -ForegroundColor Green

if ($Run) { & "$PSScriptRoot\run_android.ps1" -Apk $out -Sdk $Sdk; exit $LASTEXITCODE }
exit 0
