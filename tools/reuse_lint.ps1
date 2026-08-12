# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/reuse_lint.sh — REUSE/SPDX license-compliance
    lint (tooling backlog item 4).

.DESCRIPTION
    Every tracked file needs a copyright + SPDX license, either an inline header
    (the project-wide convention) or an entry in REUSE.toml for the formats where
    a header is impractical or risky to hand-edit. This is the LOCAL counterpart
    of the `reuse` job in .github/workflows/ci.yml (fsfe/reuse-action) — same
    tool, same repo root, so a failure here is the same failure CI would report,
    before pushing.

    `reuse` is a pure-Python CLI with no native deps: setup.ps1 installs it via
    pip ($PipPkgs). Exits 3 ("skipped") when it is not on PATH — this project's
    own convention for a tool-bound check that stays green without it.
#>

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$ExitSkipped = 3

if (-not (Get-Command reuse -ErrorAction SilentlyContinue)) {
    Write-Host "reuse_lint: 'reuse' not found - run .\setup.ps1 - skipped"
    exit $ExitSkipped
}

New-Item -ItemType Directory -Force -Path analysis-results | Out-Null
reuse lint | Tee-Object -FilePath analysis-results\reuse.txt
exit $LASTEXITCODE
