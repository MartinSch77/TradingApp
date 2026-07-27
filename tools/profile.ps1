<#
.SYNOPSIS
    Windows counterpart of tools/profile.sh — CPU profiling for TradingApp
    (REQ-N-006).

.DESCRIPTION
    Runs the release binary under the best available profiler and prints the
    hotspot report.

      tools\profile.ps1 [-Binary <exe>] [-Seconds 30]
        -Binary   default: build-release\TradingApp.exe (.\build_all.ps1 release)
        -Seconds  default: 30 — the app is profiled offscreen for this long

    Profiler pick, in order:
      VSDiagnostics  the CPU sampler shipped with Visual Studio; produces a
                     .diagsession openable in Visual Studio
      wpr / wpa      Windows Performance Recorder (ships with the Windows SDK /
                     ADK); produces an .etl trace for Windows Performance Analyzer

    There is no Windows equivalent of `perf report --stdio`, so unlike the Linux
    script this one records a trace for a GUI analyzer rather than printing a
    text hotspot table. Output lands in analysis-results\profile\.

    The domain hot paths are also benchmarked deterministically by
    build\tests\tst_benchmarks.exe (QBENCHMARK) — no profiler needed for those,
    and that is the measurement /perf-check compares against.
#>
[CmdletBinding()]
param(
    [string]$Binary,
    [int]$Seconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot

if (-not $Binary) { $Binary = Join-Path $Root 'build-release\TradingApp.exe' }
$Out = Join-Path $Root 'analysis-results\profile'
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

if (-not (Test-Path $Binary)) {
    Write-Error "binary not found: $Binary  (build it with: .\build_all.ps1 release)"
    exit 2
}

$qt = Resolve-QtPrefix -Quiet
if ($qt) {
    Add-QtToPath $qt
    # A MinGW-built binary also needs the toolchain runtime DLLs on PATH.
    Initialize-KitToolchain -QtPrefix $qt | Out-Null
}
if (-not $env:QT_QPA_PLATFORM) { $env:QT_QPA_PLATFORM = 'offscreen' }

function Find-VsDiagnostics {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    foreach ($install in (& $vswhere -latest -products * -property installationPath)) {
        $p = Join-Path $install 'Team Tools\DiagnosticsHub\Collector\VSDiagnostics.exe'
        if (Test-Path $p) { return $p }
    }
    return $null
}

$vsdiag = Find-VsDiagnostics
if ($vsdiag) {
    Write-Stage "VSDiagnostics CPU sampling ($Seconds s, offscreen)"
    $session = 1
    $agent = Join-Path (Split-Path $vsdiag -Parent) 'AgentConfigs\CpuUsageBase.json'
    $outFile = Join-Path $Out 'cpu.diagsession'
    Remove-Item $outFile -Force -ErrorAction SilentlyContinue

    $proc = Start-Process -FilePath $Binary -PassThru -WindowStyle Hidden
    try {
        & $vsdiag start $session /attach:$($proc.Id) /loadConfig:$agent | Out-Null
        Start-Sleep -Seconds $Seconds
        & $vsdiag stop $session /output:$outFile | Out-Null
    } finally {
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    }
    if (Test-Path $outFile) {
        Write-Host "profile: $outFile   (open in Visual Studio: Debug > Performance Profiler > Open)"
        exit 0
    }
    Write-Warning "VSDiagnostics produced no session — falling through to wpr"
}

if (Test-Tool 'wpr') {
    Write-Stage "Windows Performance Recorder ($Seconds s, offscreen)"
    $etl = Join-Path $Out 'cpu.etl'
    & wpr -start CPU -filemode
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "wpr -start failed (it needs an elevated shell)"
    } else {
        $proc = Start-Process -FilePath $Binary -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds $Seconds
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
        & wpr -stop $etl
        Write-Host "profile: $etl   (open in Windows Performance Analyzer: wpa $etl)"
        exit 0
    }
}

Write-Host "no profiler found — install one of:" -ForegroundColor Yellow
Write-Host "  Visual Studio (any edition) — provides VSDiagnostics.exe"
Write-Host "  the Windows Performance Toolkit (Windows SDK / ADK) — provides wpr.exe and wpa.exe"
Write-Host "The domain hot paths are also benchmarked deterministically by"
Write-Host "build\tests\tst_benchmarks.exe (QBENCHMARK) — no profiler needed for those."
exit 2
