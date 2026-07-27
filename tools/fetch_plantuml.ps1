<#
.SYNOPSIS
    Windows counterpart of tools/fetch_plantuml.sh — fetch the pinned PlantUML
    release used for the Doxygen diagrams.

.DESCRIPTION
    The jar is not committed; see docs\tools.md for the tool inventory.
    Keep $Version in sync with tools/fetch_plantuml.sh.
#>
[CmdletBinding()]
param([string]$Version = '1.2026.0')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$dest = Join-Path $PSScriptRoot 'third-party\plantuml.jar'
$dir = Split-Path $dest -Parent
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

$url = "https://github.com/plantuml/plantuml/releases/download/v$Version/plantuml-$Version.jar"
Write-Host "downloading $url" -ForegroundColor Gray
$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest $url -OutFile $dest -UseBasicParsing

if (Get-Command java -ErrorAction SilentlyContinue) {
    & java -jar $dest -version | Select-Object -First 1
} else {
    Write-Warning "java not found — the jar was downloaded but cannot be verified or used by Doxygen"
}
