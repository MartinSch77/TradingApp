# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/make_docs.sh — build the HTML documentation.

.DESCRIPTION
    Refresh the traceability matrix (so the docs always ship the current trace
    state), fetch PlantUML if missing, run doxygen, then render the collected
    PlantUML blocks and the optional Sphinx handbook.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot
Push-Location $Root
try {
    $jar = Join-Path $Root 'tools\third-party\plantuml.jar'
    if (-not (Test-Path $jar)) { & (Join-Path $Root 'tools\fetch_plantuml.ps1') }

    # StrictDoc export + regenerated requirements.md
    & (Join-Path $Root 'tools\make_requirements.ps1')

    # gaps are reported inside the matrix, so a non-zero exit is not fatal here
    Invoke-Python -Arguments @((Join-Path $Root 'tools\trace_report.py')) | Out-Null

    if (-not (Test-Tool 'doxygen')) {
        Write-Skip "doxygen not installed (winget install DimitriVanHeesch.Doxygen) — API docs skipped"
        exit 3
    }
    & doxygen (Join-Path $Root 'Doxyfile')
    $doxRc = $LASTEXITCODE

    # Belt-and-braces: render the collected PlantUML blocks explicitly (doxygen
    # writes them to one .pu file; the named @startuml blocks become <name>.svg).
    $pu = Join-Path $Root 'docs\html\inline_umlgraph_svghtml.pu'
    if ((Test-Path $pu) -and (Test-Tool 'java')) {
        # The -D property MUST be quoted: unquoted, PowerShell splits
        # -Djava.awt.headless=true at the dot and java then tries to run
        # ".awt.headless=true" as the main class.
        & java '-Djava.awt.headless=true' '-jar' $jar '-tsvg' '-o' (Join-Path $Root 'docs\html') $pu
    }

    # Sphinx developer handbook over the same markdown (optional tool).
    if (Test-Tool 'sphinx-build') {
        & sphinx-build -q -b html (Join-Path $Root 'docs') (Join-Path $Root 'docs\sphinx-html')
        Write-Host "handbook: $Root\docs\sphinx-html\sphinx_index.html"
    } else {
        Write-Host "sphinx-build not installed — handbook skipped (pip install --user sphinx myst-parser)"
    }

    Write-Host "docs: $Root\docs\html\index.html"
    exit $doxRc
} finally {
    Pop-Location
}
