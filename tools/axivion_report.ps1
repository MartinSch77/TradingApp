# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    The Axivion findings as their OWN PDF, reproducibly - separate from the quality report.

.DESCRIPTION
    Counterpart of tools/axivion_report.sh. Same parameters, same environment fallbacks,
    same exit rules (3 = skipped, never a build gate).

    THE REPORT IS AXIVION'S OWN, NOT A REIMPLEMENTATION. The Suite ships a reporting
    framework (bin/report_runner) plus report modules under example/reports/; this script
    drives report_misra_pdf.py through it, for the same reason the Test Center upload goes
    through testcentercmd: the layout, the rule tables and the delta-versus-previous-version
    logic stay the vendor's business, and a Suite upgrade improves the report for free.

    WHY SEPARATE FROM THE QUALITY PDF: tools\make_report.py summarises the whole run and
    quotes Axivion's finding COUNTS. This one is the findings themselves - per rule, per
    file, with the delta against the previous analysed version - a different document for a
    different reader, and far too long to fold into a run summary.

    ORDER: run AFTER axivion\start_analysis.ps1 (or .\build_all.ps1 axivion), or the PDF
    describes the PREVIOUS analysis. The script prints which version it reported on, so a
    stale document is visible rather than silent.

.EXAMPLE
    tools\axivion_report.ps1
    tools\axivion_report.ps1 -NoDetails
    tools\axivion_report.ps1 -Dashboard http://buildhost:9090/axivion/ -Project TradingApp
#>
[CmdletBinding()]
param(
    [string]$SuiteDir = $(if ($env:AXIVION_HOME) { $env:AXIVION_HOME } else { Join-Path $HOME 'bauhaus-suite' }),
    [string]$Dashboard = $(if ($env:AXIVION_DASHBOARD_URL) { $env:AXIVION_DASHBOARD_URL } else { 'http://localhost:9090/axivion/' }),
    [string]$Project = $(if ($env:AXIVION_PROJECT) { $env:AXIVION_PROJECT } else { 'TradingApp' }),
    [string]$Username = $(if ($env:AXIVION_USERNAME) { $env:AXIVION_USERNAME } else { 'admin' }),
    [string]$Password = $(if ($env:AXIVION_PASSWORD) { $env:AXIVION_PASSWORD } else { 'password' }),
    [string]$Token = $env:AXIVION_TOKEN,
    [string]$OutputDir,
    [switch]$NoDetails
)

Set-StrictMode -Version Latest
$Root = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $Root 'downloads' }

$runner = Join-Path $SuiteDir 'bin\report_runner.cmd'
if (-not (Test-Path $runner)) { $runner = Join-Path $SuiteDir 'bin\report_runner.exe' }
if (-not (Test-Path $runner)) { $runner = Join-Path $SuiteDir 'bin\report_runner' }
$module = Join-Path $SuiteDir 'example\reports\report_misra_pdf.py'

if (-not (Test-Path $runner)) {
    Write-Host "SKIPPED: no Axivion Suite at $SuiteDir (looked for bin\report_runner)"
    Write-Host '         Licence-bound; .\setup.ps1 cannot install it - see docs/qt-tools.md.'
    exit 3
}
if (-not (Test-Path $module)) {
    Write-Host "SKIPPED: this Suite has no MISRA PDF report module at $module"
    exit 3
}

# The dashboard has to be UP and hold this project. Probed here so the failure names the
# cause instead of surfacing as a stack trace from inside the reporting framework.
$probe = "$($Dashboard.TrimEnd('/'))/api/projects/$Project"
$pair = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${Username}:${Password}"))
$version = '?'
try {
    $meta = Invoke-RestMethod -Uri $probe -Headers @{ Authorization = "Basic $pair" } -TimeoutSec 20
    if ($meta.versions -and $meta.versions.Count -gt 0) { $version = $meta.versions[-1].date }
} catch {
    Write-Host "SKIPPED: no Axivion dashboard answering for project '$Project' at $Dashboard"
    Write-Host '         Start it and run an analysis first: axivion\start_analysis.ps1'
    Write-Host "         (or .\build_all.ps1 axivion). Probed: $probe"
    exit 3
}

if (-not (Test-Path $OutputDir)) { New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null }
Write-Host "Axivion Suite:    $SuiteDir"
Write-Host "dashboard:        $Dashboard (project $Project)"
Write-Host "analysed version: $version"
Write-Host "report module:    $(Split-Path $module -Leaf) (Axivion's own)"

$auth = if ($Token) { @('--authentication', 'token', '--token', $Token) }
        else { @('--authentication', 'password', '--username', $Username, '--password', $Password) }
$details = if ($NoDetails) { 'details=false' } else { 'details=true' }

# --noninteractive belongs to the RUNNER, before the subcommand - after it the parser
# rejects it ("unrecognized arguments"), which is easy to get wrong in CI.
& $runner --noninteractive create_report `
    --dashboard $Dashboard --project $Project @auth `
    --output_dir $OutputDir $module $details
if ($LASTEXITCODE -ne 0) {
    Write-Error 'the Axivion report module failed - see its output above'
    exit 1
}

# The module names the file itself (<project>_misra.pdf); rename to this project's artefact
# convention so publish_release and the docs can refer to one stable name.
$produced = Join-Path $OutputDir "${Project}_misra.pdf"
$final = Join-Path $OutputDir 'TradingApp-axivion-report.pdf'
if (Test-Path $produced) { Move-Item -Force $produced $final }
if (-not (Test-Path $final)) {
    Write-Error "the report module reported success but produced no PDF in $OutputDir"
    exit 1
}
$size = '{0:N1} MB' -f ((Get-Item $final).Length / 1MB)
Write-Host ''
Write-Host "Axivion report: $final ($size), analysis version $version"
exit 0
