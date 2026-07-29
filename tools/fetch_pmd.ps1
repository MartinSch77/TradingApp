<#
.SYNOPSIS
    Windows counterpart of tools/fetch_pmd.sh — fetch the pinned PMD release
    used for copy-paste detection (tools/cpd_scan.py).

.DESCRIPTION
    The dist is not committed (tools\third-party\ is git-ignored); see
    docs\tools.md for the tool inventory. Keep $Version in sync with
    tools/fetch_pmd.sh.
#>
[CmdletBinding()]
param([string]$Version = '7.19.0')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$destDir = Join-Path $PSScriptRoot 'third-party'
$target = Join-Path $destDir "pmd-bin-$Version"
if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }

if (Test-Path (Join-Path $target 'bin\pmd.bat')) {
    Write-Host "pmd $Version already present"
} else {
    $url = "https://github.com/pmd/pmd/releases/download/pmd_releases%2F$Version/pmd-dist-$Version-bin.zip"
    Write-Host "downloading $url" -ForegroundColor Gray
    $zip = Join-Path $env:TEMP "pmd-$Version.zip"
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest $url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $destDir -Force
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    # Drop any other PMD version so tools/cpd_scan.py cannot pick up a stale one.
    Get-ChildItem $destDir -Directory -Filter 'pmd-bin-*' |
        Where-Object { $_.FullName -ne $target } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

if (Get-Command java -ErrorAction SilentlyContinue) {
    & (Join-Path $target 'bin\pmd.bat') --version
} else {
    Write-Warning "java not found — PMD was downloaded but cannot run"
}
