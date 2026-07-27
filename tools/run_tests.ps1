<#
.SYNOPSIS
    Windows counterpart of tools/run_tests.sh — run the whole test suite and
    record per-test-function results as JUnit XML in test-results\.

.DESCRIPTION
    The result leg of the traceability chain (tools\trace_report.py joins them
    with the specs). ctest alone only records pass/fail per executable, so each
    Qt Test binary is run with its own junitxml writer.

.EXAMPLE
    tools\run_tests.ps1
    tools\run_tests.ps1 -BuildDir build-release
#>
[CmdletBinding()]
param([string]$BuildDir = 'build')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot

# The test executables link Qt dynamically; without the kit's bin\ on PATH they
# fail to start with a silent 0xC0000135 (DLL not found). A MinGW build needs
# the toolchain's bin too (libstdc++-6, libgcc_s_seh-1, libwinpthread-1), which
# is why the kit toolchain is initialised here as well and not only in
# build_all.ps1 — this script is also run on its own.
$qt = Resolve-QtPrefix -Quiet
if ($qt) {
    Add-QtToPath $qt
    Initialize-KitToolchain -QtPrefix $qt | Out-Null
}

$out = Join-Path $Root 'test-results'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force -Path $out | Out-Null }
Get-ChildItem $out -Filter '*.xml' -ErrorAction SilentlyContinue | Remove-Item -Force

$testDir = Join-Path $Root "$BuildDir\tests"
if (-not (Test-Path $testDir)) {
    Write-Error "no test directory: $testDir  (build it with: .\build_all.ps1 build)"
    exit 2
}

$exes = @(Get-ChildItem $testDir -Filter 'tst_*.exe' -File -ErrorAction SilentlyContinue)
if ($exes.Count -eq 0) {
    Write-Error "no test executables in $testDir  (build it with: .\build_all.ps1 build)"
    exit 2
}

$fail = 0
foreach ($exe in $exes) {
    $name = $exe.BaseName
    Write-Host "=== $name ===" -ForegroundColor Cyan
    & $exe.FullName -o "$out\$name.xml,junitxml" -o '-,txt'
    if ($LASTEXITCODE -ne 0) { $fail = 1 }
}

Write-Host ""
Write-Host "JUnit results in $out\"
exit $fail
