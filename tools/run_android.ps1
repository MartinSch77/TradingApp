<#
.SYNOPSIS
    Boot an Android emulator, install the APK, launch it and grab a screenshot.

.DESCRIPTION
    The Windows counterpart of tools/run_android.sh — same contract, same exit codes
    (3 = SKIPPED when there is no SDK, no system image or no hardware acceleration).

    Windows specifics:
      * acceleration comes from WHPX (Windows Hypervisor Platform) rather than KVM.
        It needs the "Windows Hypervisor Platform" optional feature AND virtualisation
        enabled in firmware; the emulator reports it via `emulator -accel-check`, which
        is what this script asks instead of guessing.
      * headless (-no-window) on purpose: a screenshot pulled with
        `adb exec-out screencap` needs no display and is reproducible.

.EXAMPLE
    .\tools\run_android.ps1
    .\tools\run_android.ps1 -Apk downloads\TradingApp-1.0.0-x86_64-debug.apk -Keep
#>
[CmdletBinding()]
param(
    [string]$Apk,
    [string]$Avd = 'tradingapp_x86_64',
    [string]$Sdk,
    [string]$Shot,
    [switch]$Keep
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
if (-not $Shot) { $Shot = Join-Path $Root 'downloads\android-screenshot.png' }

$adb = Join-Path $Sdk 'platform-tools\adb.exe'
$emu = Join-Path $Sdk 'emulator\emulator.exe'
if (-not (Test-Path $adb)) { Write-Skipped "no adb at $adb (see .\setup.ps1 android)" }
if (-not (Test-Path $emu)) { Write-Skipped "no emulator at $emu (sdkmanager --install emulator)" }

$image = Get-ChildItem -Path (Join-Path $Sdk 'system-images') -Filter 'x86_64' -Recurse -Directory `
    -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
if (-not $image) { Write-Skipped "no x86_64 system image (sdkmanager --install 'system-images;android-35;google_apis;x86_64')" }

# Hardware acceleration: ask the emulator, do not infer. Without it a boot takes
# tens of minutes, which looks like a hang rather than a missing feature.
$accel = & $emu -accel-check 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Skipped "no hardware acceleration: $($accel -join ' ') — enable the 'Windows Hypervisor Platform' feature and virtualisation in firmware"
}

if (-not $Apk) {
    $newest = Get-ChildItem -Path (Join-Path $Root 'downloads') -Filter '*.apk' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($newest) { $Apk = $newest.FullName }
}
if (-not $Apk -or -not (Test-Path $Apk)) { Write-Skipped 'no APK given and none in downloads\ (tools\build_android.ps1)' }

# --- the AVD ---------------------------------------------------------------
$avdManager = Join-Path $Sdk 'cmdline-tools\latest\bin\avdmanager.bat'
$existing = & $avdManager list avd -c 2>$null
if ($existing -notcontains $Avd) {
    # system-images\<platform>\<tag>\x86_64  ->  system-images;<platform>;<tag>;x86_64
    $tag = Split-Path -Leaf (Split-Path -Parent $image.FullName)
    $platform = Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $image.FullName))
    $imageId = "system-images;$platform;$tag;x86_64"
    Write-Host "creating AVD $Avd from $imageId"
    'no' | & $avdManager create avd -n $Avd -k $imageId --device pixel_5 --force | Out-Null
}

Write-Host "booting $Avd (headless, swiftshader)…"
$emuArgs = @('-avd', $Avd, '-no-window', '-no-audio', '-no-boot-anim', '-no-snapshot',
    '-gpu', 'swiftshader_indirect', '-netdelay', 'none', '-netspeed', 'full')
$emuProc = Start-Process -FilePath $emu -ArgumentList $emuArgs -PassThru -WindowStyle Hidden

try {
    & $adb start-server | Out-Null
    & $adb wait-for-device
    # wait-for-device returns when adbd answers; the framework is usable only once
    # sys.boot_completed flips, which on a cold boot is another minute or two.
    $booted = $false
    foreach ($i in 1..180) {
        $prop = (& $adb shell getprop sys.boot_completed 2>$null) -replace '\s', ''
        if ($prop -eq '1') { $booted = $true; break }
        Start-Sleep -Seconds 2
    }
    if (-not $booted) { Write-Host 'emulator did not finish booting' -ForegroundColor Red; exit 1 }

    $release = (& $adb shell getprop ro.build.version.release) -replace '\s', ''
    $api = (& $adb shell getprop ro.build.version.sdk) -replace '\s', ''
    Write-Host "booted: Android $release (API $api)"

    Write-Host "installing $(Split-Path -Leaf $Apk)"
    & $adb install -r -g $Apk

    $pkg = 'org.qtproject.example.TradingApp'
    Write-Host "launching $pkg"
    & $adb shell am start -n "$pkg/org.qtproject.qt.android.bindings.QtActivity" | Out-Host
    Start-Sleep -Seconds 12

    New-Item -ItemType Directory -Path (Split-Path -Parent $Shot) -Force | Out-Null
    # -Encoding Byte is PowerShell 5.1 syntax; screencap output is binary PNG.
    & $adb exec-out screencap -p | Set-Content -Path $Shot -Encoding Byte
    Write-Host "screenshot: $Shot"

    Write-Host '--- logcat (app only) ---'
    & $adb logcat -d -s TradingApp:V QtCore:V Qt:V AndroidRuntime:E | Select-Object -Last 25

    $pid_ = (& $adb shell pidof $pkg) -replace '\s', ''
    if ($pid_) {
        Write-Host "RUNNING: $pkg is alive on the emulator" -ForegroundColor Green
    } else {
        Write-Host "WARNING: $pkg is not running - see the logcat above" -ForegroundColor Red
        exit 1
    }
} finally {
    if (-not $Keep) {
        & $adb emu kill 2>$null | Out-Null
        if ($emuProc -and -not $emuProc.HasExited) { Stop-Process -Id $emuProc.Id -Force -ErrorAction SilentlyContinue }
    }
}
exit 0
