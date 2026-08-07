# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/mcp_env.sh — resolve the three environment
    variables .mcp.json needs for the Axivion MCP servers (axdocumentation,
    axdashboard).

.DESCRIPTION
      AXIVION_SUITE_DIR      the Suite root (holds bin\rfgscript.exe + mcps\)
      AXIVION_MCP_PYTHON     interpreter of the Suite's MCP venv
      AXIVION_DATABASES_DIR  dashboard database directory

    Why via the environment at all: .mcp.json must not carry machine-specific
    absolute paths, and Claude Code's ${VAR} / ${VAR:-default} interpolation
    cannot branch on the platform — the Suite's MCP venv is
    .venv\Scripts\python.exe here and .venv/bin/python on Linux. Resolving that
    difference HERE keeps .mcp.json byte-identical on both.

    NOTE: `$(VAR)` is Axivion's OWN config syntax (see axivion\ci_config.json)
    and does NOT work in .mcp.json — Claude Code expands `${VAR}` only, and
    passes `$(VAR)` through verbatim without even a warning. The servers then die
    with a bare "The system cannot find the path specified" from the shell.

    Exit 3 = "skipped" (the repo-wide convention for an absent license-bound
    tool), so a machine without the Axivion Suite is not a failure.

.PARAMETER Persist
    Write the variables to the User environment scope (permanent) instead of
    only reporting them.

.PARAMETER Export
    Print `$env:VAR = '…'` lines for the current session, for Invoke-Expression.
#>
[CmdletBinding()]
param(
    [switch]$Persist,
    [switch]$Export
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"

$EXIT_SKIPPED = 3

# Get-AxivionSuite enumerates every install and takes the NEWEST — on this
# reference machine 7.12.1 sits in "Program Files" and 7.12.3 in
# "Program Files (x86)", so picking the first match is wrong. An explicit
# AXIVION_SUITE_DIR wins so an exotic install can override the search.
$suite = $null
if ($env:AXIVION_SUITE_DIR -and (Test-Path (Join-Path $env:AXIVION_SUITE_DIR 'bin\axivion_ci.exe'))) {
    $suite = $env:AXIVION_SUITE_DIR
} else {
    $found = Get-AxivionSuite
    if ($found) { $suite = $found.Root }
}

if (-not $suite) {
    Write-Skip "the Axivion Suite is not installed (license-bound). The axdocumentation / axdashboard MCP servers stay unavailable; Claude Code reports them as 'Missing environment variables'."
    exit $EXIT_SKIPPED
}

# Forward slashes: the value lands in JSON-interpolated command lines, where a
# backslash is an escape character. Windows accepts either separator.
$suite = $suite.Replace('\', '/').TrimEnd('/')
$mcpPython = "$suite/mcps/axivion-mcps/.venv/Scripts/python.exe"

# The Axivion Dashboard installer sets AXIVIONDATABASESDIR (no underscores);
# honour that, then the name .mcp.json passes on, then the default install path.
$databasesDir = $env:AXIVION_DATABASES_DIR
if (-not $databasesDir) { $databasesDir = $env:AXIVIONDATABASESDIR }
if (-not $databasesDir) { $databasesDir = 'C:/AxivionDashboard/config' }
$databasesDir = $databasesDir.Replace('\', '/').TrimEnd('/')

# The MCP servers are a Suite technology preview — an older Suite has no mcps\.
if (-not (Test-Path $mcpPython)) {
    Write-Skip "$suite has no MCP venv ($mcpPython). The Axivion MCP servers ship with Suite 7.12 and newer."
    exit $EXIT_SKIPPED
}

$vars = [ordered]@{
    AXIVION_SUITE_DIR     = $suite
    AXIVION_MCP_PYTHON    = $mcpPython
    AXIVION_DATABASES_DIR = $databasesDir
}

if ($Export) {
    foreach ($k in $vars.Keys) { Write-Output "`$env:$k = '$($vars[$k])'" }
    exit 0
}

if ($Persist) {
    foreach ($k in $vars.Keys) {
        [Environment]::SetEnvironmentVariable($k, $vars[$k], 'User')
        Set-Item -Path "Env:$k" -Value $vars[$k]
        Write-Host ("  {0,-22} {1}" -f $k, $vars[$k])
    }
    Write-Host ""
    Write-Host "Written to the User environment scope. Restart VS Code / the shell so" -ForegroundColor Yellow
    Write-Host "Claude Code inherits them, then check with: claude mcp list" -ForegroundColor Yellow
    exit 0
}

foreach ($k in $vars.Keys) {
    $note = ''
    if ($k -eq 'AXIVION_DATABASES_DIR' -and -not (Test-Path $vars[$k])) { $note = '  (does not exist yet)' }
    Write-Host ("{0,-22} {1}{2}" -f $k, $vars[$k], $note)
}
$stale = @($vars.Keys | Where-Object { [Environment]::GetEnvironmentVariable($_, 'User') -ne $vars[$_] })
if ($stale.Count -gt 0) {
    Write-Host ""
    Write-Host "not in the User environment yet — run: .\tools\mcp_env.ps1 -Persist" -ForegroundColor Yellow
}
