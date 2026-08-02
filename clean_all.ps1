<#
.SYNOPSIS
    Windows counterpart of ./clean_all.sh — remove everything build_all.ps1
    (and the tools\ scripts) generate.

.DESCRIPTION
    All build trees, test results, coverage and static-analysis reports, and
    the generated documentation. Everything here is reproducible with
    .\build_all.ps1.

    Kept by default (pass -Deep to remove them too):
      .axivion-cache\ + .fslckout   Axivion incremental-analysis state — wiping it
                                    forces the next Axivion run to re-analyze from
                                    scratch and loses the local finding history
      tools\third-party\            pinned plantuml.jar (tools\fetch_plantuml.ps1
                                    re-downloads it when the docs are built)

.EXAMPLE
    .\clean_all.ps1
    .\clean_all.ps1 -Deep
#>
[CmdletBinding()]
param([switch]$Deep)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\tools\common.ps1"
$Root = Get-RepoRoot

$Generated = @(
    'build'
    'build-cov-gcc'
    'build-cov-mcdc'
    'build-cov-coco'
    'build-cov-msvc'
    'build-san'
    'build-san-tsan'
    'build-san-ubsan'
    'build-release'
    'build-vs'
    '.vs'
    'build_axivion'
    'build-android'
    'build-ios'
    'dist'
    'test-results'
    'coverage'
    'analysis-results'
    'docs/html'
    'docs/traceability.html'
    'docs/strictdoc'
    'docs/sphinx-html'
)
$DeepTargets = @(
    '.axivion-cache'
    '.fslckout'
    '_FOSSIL_'      # the Windows name of the Axivion Shadow checkout database
    'tools/third-party'
)

# Pattern-matched leftovers (absent = simply nothing added):
#   build-android-<abi>   the per-ABI Android build trees tools\build_android.ps1
#                         creates (the fixed 'build-android' entry above never
#                         matched them)
#   *.log                 stray tool logs at the repo root — aqt writes
#                         aqtinstall.log into the CWD on every Qt-kit install
#                         (setup, CI, release), and nothing else puts a .log here
$patternTargets = @(Get-ChildItem -Path $Root -Force -ErrorAction SilentlyContinue |
    Where-Object { ($_.Name -like 'build-android-*') -or ($_.Name -like '*.log') } |
    ForEach-Object { $_.Name })

$targets = $Generated + $patternTargets
if ($Deep) { $targets += $DeepTargets }

$existing = @($targets | Where-Object { Test-Path (Join-Path $Root $_) })
if ($existing.Count -eq 0) {
    Write-Host "already clean — nothing to remove"
    exit 0
}

# The `du -shc` equivalent: report what is about to go, then remove it.
# Sizes are summed by hand rather than with Measure-Object: for a directory that
# contains no files at all, Measure-Object returns $null, and reading .Sum off
# that under Set-StrictMode is a PropertyNotFoundStrict error — which is exactly
# the state a half-finished clean, or a build tree holding only subdirectories,
# leaves behind.
$total = 0L
foreach ($p in $existing) {
    $full = Join-Path $Root $p
    $bytes = 0L
    if (Test-Path $full -PathType Container) {
        foreach ($f in @(Get-ChildItem $full -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            $bytes += $f.Length
        }
    } else {
        $bytes = (Get-Item $full).Length
    }
    $total += $bytes
    Write-Host ("  {0,10:N1} MB  {1}" -f ($bytes / 1MB), $p)
}
Write-Host ("  {0,10:N1} MB  total" -f ($total / 1MB))
Write-Host ""

# Report what actually happened. A build tree can be locked by a running
# analysis, a debugger or an open Explorer window, and silently claiming
# "removed" when the directory is still there is worse than saying so.
$failed = @()
foreach ($p in $existing) {
    $full = Join-Path $Root $p
    Remove-Item $full -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $full) {
        Write-Host "COULD NOT REMOVE $p (in use?)" -ForegroundColor Red
        $failed += $p
    } else {
        Write-Host "removed $p"
    }
}

if ($failed.Count -gt 0) {
    Write-Host ""
    Write-Host "$($failed.Count) of $($existing.Count) targets could not be removed." -ForegroundColor Red
    Write-Host "Something is still holding files open — a running axivion_ci or build," -ForegroundColor DarkGray
    Write-Host "Visual Studio with the solution open, or a debugger on a test binary." -ForegroundColor DarkGray
    exit 1
}
