# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/sonar_scan.sh — conditional SonarQube scan.

.DESCRIPTION
    Runs ONLY when a SonarQube server is actually reachable and sonar-scanner is
    installed — otherwise it reports why and exits 0 (the pipeline is not failed
    by an absent optional tool).

    After a successful scan the open issues are pulled from the server's Web API
    and normalized into analysis-results\sonarqube.txt (file|line|severity|rule|
    message), which axivion/external_import.py brings onto the Axivion dashboard
    as provider "sonarqube".

    Env: SONAR_HOST_URL (default http://localhost:9000), SONAR_TOKEN (if the
    server requires authentication).
#>
[CmdletBinding()]
param(
    [string]$HostUrl = $(if ($env:SONAR_HOST_URL) { $env:SONAR_HOST_URL } else { 'http://localhost:9000' })
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot
$Out = Join-Path $Root 'analysis-results'
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

# sonar-scanner ships as a .bat wrapper on Windows.
$scanner = $null
foreach ($n in @('sonar-scanner.bat', 'sonar-scanner')) {
    $p = Get-ToolPath $n
    if ($p) { $scanner = $p; break }
}
if (-not $scanner) {
    Write-Host "sonar-scanner not installed — SonarQube scan skipped."
    Write-Host "(install: https://docs.sonarsource.com/sonarqube/latest/analyzing-source-code/scanners/sonarscanner/)"
    exit 0
}

try {
    $status = Invoke-RestMethod "$HostUrl/api/system/status" -TimeoutSec 5 -ErrorAction Stop
} catch {
    Write-Host "no SonarQube server reachable at $HostUrl — scan skipped (start one, or set SONAR_HOST_URL)."
    exit 0
}
if ($status.status -ne 'UP') {
    Write-Host "SonarQube at $HostUrl reports status '$($status.status)' — scan skipped."
    exit 0
}

Write-Stage "sonar-scanner against $HostUrl"
$scanArgs = @("-Dsonar.host.url=$HostUrl")
if ($env:SONAR_TOKEN) { $scanArgs += "-Dsonar.token=$env:SONAR_TOKEN" }
Push-Location $Root
& $scanner @scanArgs
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { exit 1 }

# Export open issues -> pipe format for the dashboard import.
$headers = @{}
if ($env:SONAR_TOKEN) {
    $pair = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("$($env:SONAR_TOKEN):"))
    $headers['Authorization'] = "Basic $pair"
}
try {
    $data = Invoke-RestMethod "$HostUrl/api/issues/search?componentKeys=TradingApp&resolved=false&ps=500" -Headers $headers -ErrorAction Stop
} catch {
    Write-Warning "could not fetch issues from $HostUrl : $($_.Exception.Message)"
    exit 1
}

$rows = @()
foreach ($issue in $data.issues) {
    $comp = $issue.component                       # e.g. TradingApp:src/x.cpp
    $path = $comp
    if ($comp -match ':') { $path = ($comp -split ':', 2)[1] }
    $line = 1
    if ($issue.PSObject.Properties.Name -contains 'line' -and $issue.line) { $line = $issue.line }
    $sev = 'major'
    if ($issue.PSObject.Properties.Name -contains 'severity' -and $issue.severity) { $sev = $issue.severity.ToLower() }
    $rule = 'sonarqube'
    if ($issue.PSObject.Properties.Name -contains 'rule' -and $issue.rule) { $rule = $issue.rule }
    $msg = ''
    if ($issue.PSObject.Properties.Name -contains 'message' -and $issue.message) { $msg = $issue.message -replace '\|', '/' }
    $rows += "$path|$line|$sev|$rule|$msg"
}
$target = Join-Path $Out 'sonarqube.txt'
Write-TextFile $target (($rows -join "`n"))
Write-Host "sonarqube: $($rows.Count) open issues -> $target"
