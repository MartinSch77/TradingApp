# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/make_requirements.sh — the requirements-as-code
    toolchain (StrictDoc; see strictdoc_config.py).

.DESCRIPTION
      1. Validate + export requirements\requirements.sdoc to HTML with
         requirement <-> source traceability (the tests carry
         @relation(REQ-…, scope=function) markers), a traceability matrix and
         project statistics -> docs\strictdoc\html\index.html
      2. Regenerate docs\requirements.md — the Doxygen page and one input leg
         of tools\trace_report.py — from the same .sdoc (tools\sdoc_to_md.py).

    The .sdoc file is the single source of truth; never edit
    docs\requirements.md by hand.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot

Add-PythonScriptsToPath

if (-not (Test-Tool 'strictdoc')) {
    Write-Skip "strictdoc not installed (pip install --user strictdoc) — export skipped, docs\requirements.md left as committed."
    exit 0
}

& strictdoc export $Root --output-dir (Join-Path $Root 'docs\strictdoc')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not (Invoke-Python -Arguments @((Join-Path $Root 'tools\sdoc_to_md.py')))) { exit 1 }

Write-Host "requirements: docs\strictdoc\html\index.html (+ docs\requirements.md regenerated)"
