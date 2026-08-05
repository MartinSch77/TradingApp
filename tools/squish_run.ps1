<#
.SYNOPSIS
    Run the Squish GUI suite on Windows — and never touch a real account doing it.

.DESCRIPTION
    The PowerShell counterpart of tools/squish_run.sh, kept in lockstep with it.

    Licence-bound, so it follows this project's rule for such tools: when Squish is
    not installed or not licensed the stage prints why and exits 3 ("skipped"). It
    is never a build gate; the missing licence is reported in the quality PDF.

    THE SAFETY PROPERTY, which is why this wrapper exists rather than a bare
    squishrunner call: every run is forced into SIMULATION.
    TRADINGAPP_FORCE_SIMULATION makes Config::hasCredentials() answer false, so the
    app has no credentials, cannot be LIVE and has no order path to the broker —
    whatever apiKeyEtoro.json on this machine says. That is checked by a unit test
    (TS-CFG-007) and asserted again from the outside by the suite's first test case.
    On top of it the run gets its own APPDATA, so the developer's keys and bot books
    are not even visible.

    Results are written as JUnit XML next to the unit suite's, so Qt Test Center
    ingests both from one place (tools\testcenter_upload.ps1).

.PARAMETER BuildDir
    The build tree holding TradingApp.exe. Defaults to build.
#>
param(
    [string]$BuildDir = 'build'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Aut = Join-Path $Root "$BuildDir\TradingApp.exe"
$Suite = Join-Path $Root 'squish\suite_gui'
$Results = Join-Path $Root 'test-results\squish'
$Scratch = Join-Path $env:TEMP 'tradingapp-squish'

# %SQUISH_DIR% wins; otherwise the installer's own default location.
$SquishDir = if ($env:SQUISH_DIR) { $env:SQUISH_DIR }
elseif ($env:SQUISH_PREFIX) { $env:SQUISH_PREFIX }
else { 'C:\Program Files\froglogic\Squish' }

function Resolve-SquishTool {
    param([Parameter(Mandatory)][string]$Name)
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $candidate = Join-Path $SquishDir "bin\$Name.exe"
    if (Test-Path $candidate) { return $candidate }
    return $null
}

$runner = Resolve-SquishTool 'squishrunner'
$server = Resolve-SquishTool 'squishserver'
if (-not $runner -or -not $server) {
    Write-Host "SKIPPED: Squish not found (looked on PATH and in $SquishDir\bin)" -ForegroundColor Yellow
    Write-Host '         Licence-bound; .\setup.ps1 cannot install it. See todo.txt.' -ForegroundColor DarkGray
    exit 3
}
if (-not (Test-Path $Aut)) {
    Write-Error "no AUT at $Aut - build it first (.\build_all.ps1 build)"
    exit 1
}

# A licence check that needs no display: --version fails without a valid licence.
& $runner --version *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host 'SKIPPED: Squish is installed but not usable - licence missing or expired' -ForegroundColor Yellow
    Write-Host "         (check: `"$runner`" --version). Never a build gate." -ForegroundColor DarkGray
    exit 3
}

New-Item -ItemType Directory -Force -Path $Results | Out-Null
if (Test-Path $Scratch) { Remove-Item -Recurse -Force $Scratch }
New-Item -ItemType Directory -Force -Path "$Scratch\config" | Out-Null

# A run must not inherit the developer's account, books or model settings.
$env:TRADINGAPP_FORCE_SIMULATION = '1'
$env:APPDATA = "$Scratch\config"
$env:LOCALAPPDATA = "$Scratch\config"
$env:ETORO_MODE = 'demo'
$env:ETORO_API_KEY = ''
$env:ETORO_USER_KEY = ''
$env:TRADINGAPP_BOT_AI = 'off'
$env:TRADINGAPP_BOT_NET = 'off'

Write-Host '== Squish ==' -ForegroundColor Cyan
Write-Host "AUT:    $Aut"
Write-Host "suite:  $Suite"
Write-Host 'mode:   FORCED SIMULATION (TRADINGAPP_FORCE_SIMULATION=1, isolated APPDATA)'

& $server --stop *> $null
& $server --config addAUT TradingApp (Join-Path $Root $BuildDir) *> $null
Start-Process -FilePath $server -ArgumentList '--daemon' -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 2

# Squish's AI-assisted object lookup (Squish 8.x) can find a widget whose properties
# moved instead of failing the step. Opt-in via SQUISH_AI=1, not on by default: this
# object map addresses widgets by objectName and tools\check_object_names.py
# guarantees every widget has one, so there is little for the AI to repair — and a
# lookup that "heals" a genuinely wrong name would hide the very breakage the suite
# exists to catch. The switch name is from the documentation and is NOT verified
# against a licensed run; if this version rejects it the run continues without it.
$aiArgs = @()
if ($env:SQUISH_AI -eq '1') {
    $help = (& $runner --help 2>&1) -join "`n"
    if ($help -match '(?i)objectnotfounddebugging|ai') {
        $aiArgs += '--objectNotFoundDebugging=ai'
        Write-Host 'AI object lookup: enabled (SQUISH_AI=1)'
    }
    else {
        Write-Host 'AI object lookup: requested but this squishrunner does not offer it - continuing without' -ForegroundColor DarkGray
    }
}

$xml = Join-Path $Results 'squish-suite_gui.xml'
& $runner --testsuite $Suite @aiArgs --reportgen "junit,$xml" --reportgen stdout
$rc = $LASTEXITCODE
& $server --stop *> $null

Write-Host "JUnit XML: $xml"
if ($rc -ne 0) { Write-Host "Squish reported failures (rc=$rc)" -ForegroundColor Red }
exit $rc
