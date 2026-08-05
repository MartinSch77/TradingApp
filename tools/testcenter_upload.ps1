<#
.SYNOPSIS
    Send every test result to Qt Test Center (Squish Test Center).

.DESCRIPTION
    The PowerShell counterpart of tools/testcenter_upload.sh, kept in lockstep.
    Uploads EVERY JUnit XML under test-results\ — the Qt Test suites and the Squish
    GUI suite alike — tagged with the short git sha so a run maps to a commit.

    Configuration comes from the environment, so no secret lands in the repository:
        TESTCENTER_URL      e.g. http://localhost:8800          (required)
        TESTCENTER_PROJECT  project name in Test Center         (default TradingApp)
        TESTCENTER_TOKEN    an API token from Test Center       (required)
        TESTCENTER_BATCH    batch label                         (default: git sha)

    Licence-bound like Squish itself: not configured or not reachable means the
    stage says why and exits 3 ("skipped"). Never a build gate — the quality PDF
    reports the missing licence instead.

.PARAMETER DryRun
    List exactly what would be uploaded and send nothing.
#>
param(
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Results = Join-Path $Root 'test-results'

$url = $env:TESTCENTER_URL
$project = if ($env:TESTCENTER_PROJECT) { $env:TESTCENTER_PROJECT } else { 'TradingApp' }
$token = $env:TESTCENTER_TOKEN
$batch = if ($env:TESTCENTER_BATCH) { $env:TESTCENTER_BATCH }
else {
    $sha = (& git -C $Root rev-parse --short HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $sha) { $sha } else { (Get-Date -Format 'yyyyMMdd-HHmm') }
}

$xml = @(Get-ChildItem -Path $Results -Filter '*.xml' -Recurse -File -ErrorAction SilentlyContinue |
    Sort-Object FullName)
if ($xml.Count -eq 0) {
    Write-Error "no test results in $Results - run tools\run_tests.ps1 first"
    exit 1
}

Write-Host '== Qt Test Center upload ==' -ForegroundColor Cyan
Write-Host "results:  $($xml.Count) XML file(s)"
foreach ($f in $xml) { Write-Host "  $($f.FullName.Substring($Root.Length + 1))" }
Write-Host "project:  $project"
Write-Host "batch:    $batch"

if ($DryRun) {
    Write-Host '(dry run - nothing sent)' -ForegroundColor DarkGray
    exit 0
}
if (-not $url -or -not $token) {
    Write-Host 'SKIPPED: Test Center not configured - set TESTCENTER_URL and TESTCENTER_TOKEN' -ForegroundColor Yellow
    Write-Host '         (licence-bound; see todo.txt). Never a build gate.' -ForegroundColor DarkGray
    exit 3
}

$headers = @{ Authorization = "Bearer $token" }
try {
    Invoke-RestMethod -Uri "$url/api/v1/projects" -Headers $headers -TimeoutSec 10 | Out-Null
}
catch {
    Write-Host "SKIPPED: Test Center at $url did not answer (wrong URL, server down, or bad token)" -ForegroundColor Yellow
    Write-Host '         Never a build gate.' -ForegroundColor DarkGray
    exit 3
}

$rc = 0
foreach ($f in $xml) {
    # One call per file so a single bad report cannot hide the rest.
    try {
        Invoke-RestMethod -Method Post -TimeoutSec 60 `
            -Uri "$url/api/v1/projects/$project/reports?batch=$batch" `
            -Headers $headers -ContentType 'application/xml' `
            -InFile $f.FullName | Out-Null
        Write-Host "uploaded: $($f.Name)"
    }
    catch {
        Write-Host "FAILED to upload $($f.Name): $($_.Exception.Message)" -ForegroundColor Red
        $rc = 1
    }
}
Write-Host "batch $batch is at $url (project $project)"
exit $rc
