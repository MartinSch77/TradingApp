# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Send every test result to Squish Test Center.

.DESCRIPTION
    The PowerShell counterpart of tools/testcenter_upload.sh, kept in lockstep.
    Uploads EVERY JUnit XML under test-results\ - the Qt Test suites and the Squish
    GUI suite alike - into one named batch.

    UPLOADS GO THROUGH testcentercmd.exe, which ships with Test Center and with
    Squish. An earlier version of this script POSTed JUnit XML to an invented REST
    path; that path does not exist. The server answers every unregistered REST
    caller with 'Only known clients permitted for REST access' (error 1006), so the
    old code could not have worked. The product has a supported command-line client
    and using it makes the protocol the vendor's business, not this repository's guess.

    THREE ways to authenticate, in order of preference:
      1. testcentercmd's OWN credential store - `testcentercmd config token <value>`.
         Nothing secret then appears in a command line, an environment variable or
         this repository, so this is the route docs\qt-tools.md recommends. The
         script simply passes no credentials and lets the client find its own.
      2. TESTCENTER_TOKEN - for CI, where a secret arrives as an environment variable.
      3. TESTCENTER_USER + TESTCENTER_PASSWORD - a plain login, which also works.

    Labels are how a batch stays readable months later. Three names are the
    PRODUCT'S and are not free choices: `.git.revision` drives the Commit Summary
    section of the printable report, `.git.branch` selects the branch for repository
    lookups, and `.reference.url` becomes a clickable link in the References column.

    Settings, each a parameter with an environment fallback:
        TESTCENTER_DIR      installation directory (auto-discovered)
        TESTCENTER_URL      default http://localhost:8800
        TESTCENTER_PROJECT  project name in Test Center      (default TradingApp)
        TESTCENTER_BATCH    batch name                       (default: the git sha)
        TESTCENTER_TOKEN    an upload token from Test Center
        TESTCENTER_USER / TESTCENTER_PASSWORD
        TESTCENTER_SUITE    which suite these results are    (default unit+integration)
        TESTCENTER_LABELS   extra labels, space-separated

    Licence-bound like Squish itself: not installed, not reachable or not configured
    means the stage says why and exits 3 ("skipped"). Never a build gate - the
    quality PDF reports the missing licence instead.

.PARAMETER DryRun
    List exactly what would be uploaded, with the labels, and send nothing.
#>
param(
    [switch]$DryRun,
    [string]$TestCenterDir = $env:TESTCENTER_DIR,
    [string]$Url = $(if ($env:TESTCENTER_URL) { $env:TESTCENTER_URL } else { 'http://localhost:8800' }),
    [string]$Project = $(if ($env:TESTCENTER_PROJECT) { $env:TESTCENTER_PROJECT } else { 'TradingApp' }),
    [string]$Batch = $env:TESTCENTER_BATCH,
    [string]$Token = $env:TESTCENTER_TOKEN,
    [string]$User = $env:TESTCENTER_USER,
    [string]$Password = $env:TESTCENTER_PASSWORD,
    [string]$Suite = $(if ($env:TESTCENTER_SUITE) { $env:TESTCENTER_SUITE } else { 'unit+integration' }),
    [string[]]$Label = @()
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Results = Join-Path $Root 'test-results'

if (-not $Batch) {
    $sha = (& git -C $Root rev-parse --short HEAD 2>$null)
    $Batch = if ($LASTEXITCODE -eq 0 -and $sha) { "$sha" } else { (Get-Date -Format 'yyyyMMdd-HHmm') }
}

# Where testcentercmd is. In order: an explicit directory, PATH, the Test Center
# installer's own layout, then a Squish install, which bundles the same client.
# Newest version wins.
function Find-TestCenterCmd {
    if ($TestCenterDir) {
        foreach ($candidate in @(
                (Join-Path $TestCenterDir 'bin\testcentercmd.exe'),
                (Join-Path $TestCenterDir 'testcentercmd.exe'))) {
            if (Test-Path $candidate) { return $candidate }
        }
        return $null
    }
    $onPath = Get-Command testcentercmd.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $roots = @($env:USERPROFILE, 'C:\', ${env:ProgramFiles}, ${env:ProgramFiles(x86)}) |
        Where-Object { $_ -and (Test-Path $_) }
    foreach ($base in $roots) {
        $found = Get-ChildItem -Path $base -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^(testcenter|[Ss]quish)' } |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'bin\testcentercmd.exe' } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}

$cmd = Find-TestCenterCmd

# The git labels. `.git.revision` is the product's own name for the commit a batch
# describes, so it is filled in automatically rather than left to whoever runs this.
#
# A DIRTY WORKING TREE STILL GETS THE REVISION, plus a `worktree=dirty` label beside
# it: the report needs a commit to anchor on, but a batch whose sources differ from
# that commit must say so, or a green run gets read as evidence for code that was
# never tested.
$labels = New-Object System.Collections.Generic.List[string]
if ($env:TESTCENTER_LABELS) {
    foreach ($l in ($env:TESTCENTER_LABELS -split '\s+')) { if ($l) { $labels.Add($l) } }
}
foreach ($l in $Label) { if ($l) { $labels.Add($l) } }

$fullSha = (& git -C $Root rev-parse HEAD 2>$null)
if ($LASTEXITCODE -eq 0 -and $fullSha) {
    $labels.Add(".git.revision=$fullSha")
    $branch = (& git -C $Root rev-parse --abbrev-ref HEAD 2>$null)
    if ($branch -and $branch -ne 'HEAD') { $labels.Add(".git.branch=$branch") }
    & git -C $Root diff --quiet HEAD 2>$null
    $dirty = ($LASTEXITCODE -ne 0)
    $untracked = (& git -C $Root ls-files --others --exclude-standard 2>$null)
    if ($dirty -or $untracked) { $labels.Add('worktree=dirty') }
    # A browsable link for the References column, derived from the remote so no URL
    # is hardcoded. Both remote spellings appear in practice; normalise ssh to https.
    $remote = (& git -C $Root remote get-url origin 2>$null)
    if ($LASTEXITCODE -eq 0 -and $remote) {
        $webUrl = $null
        if ($remote.StartsWith('git@')) { $webUrl = 'https://' + $remote.Substring(4).Replace(':', '/') }
        elseif ($remote.StartsWith('http')) { $webUrl = $remote }
        if ($webUrl) {
            if ($webUrl.EndsWith('.git')) { $webUrl = $webUrl.Substring(0, $webUrl.Length - 4) }
            $labels.Add(".reference.url=$webUrl/commit/$fullSha")
        }
    }
}
$labels.Add("suite=$Suite")
$labels.Add("platform=windows-$($env:PROCESSOR_ARCHITECTURE.ToLower())")

$xml = @(Get-ChildItem -Path $Results -Filter '*.xml' -Recurse -File -ErrorAction SilentlyContinue |
    Sort-Object FullName)
if ($xml.Count -eq 0) {
    Write-Error "no test results in $Results - run tools\run_tests.ps1 first"
    exit 1
}

Write-Host '== Squish Test Center upload ==' -ForegroundColor Cyan
Write-Host "results:  $($xml.Count) XML file(s)"
foreach ($f in $xml) { Write-Host "  $($f.FullName.Substring($Root.Length + 1))" }
Write-Host "client:   $(if ($cmd) { $cmd } else { '<not found>' })"
Write-Host "server:   $Url"
Write-Host "project:  $Project"
Write-Host "batch:    $Batch"
Write-Host "labels:   $($labels -join ' ')"

if ($DryRun) {
    Write-Host '(dry run - nothing sent)' -ForegroundColor DarkGray
    exit 0
}

if (-not $cmd) {
    Write-Host 'SKIPPED: testcentercmd.exe not found.' -ForegroundColor Yellow
    Write-Host '         Squish Test Center is licence-bound (qt.io). Pass -TestCenterDir' -ForegroundColor DarkGray
    Write-Host '         or set TESTCENTER_DIR. See docs\qt-tools.md. Never a build gate.' -ForegroundColor DarkGray
    exit 3
}

# Is a server actually there? Without this check testcentercmd waits for interactive
# credentials, which in a pipeline is a HUNG stage rather than a reported one.
try {
    Invoke-WebRequest -Uri $Url -TimeoutSec 10 -UseBasicParsing -MaximumRedirection 0 `
        -ErrorAction Stop | Out-Null
}
catch {
    # A redirect to the login page means the server IS answering, which is what this
    # check is for. Only a genuine connection failure is a skip.
    $status = $null
    if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
    if (-not ($status -ge 200 -and $status -lt 400)) {
        Write-Host "SKIPPED: no Test Center answering at $Url" -ForegroundColor Yellow
        Write-Host '         Start it with:  <install-dir>\bin\testcenter.exe start' -ForegroundColor DarkGray
        Write-Host "         then open $Url once to create the first user. Never a build gate." -ForegroundColor DarkGray
        exit 3
    }
}

$auth = @()
if ($Token) {
    $auth += "--token=$Token"
    Write-Host 'auth:     token from the environment'
}
elseif ($User -and $Password) {
    $auth += @("--user=$User", "--password=$Password")
    Write-Host 'auth:     user + password from the environment'
}
else {
    # No credential was handed to us, which is NOT the same as having none: the client
    # keeps its own store (`testcentercmd config token`) and finds it without help. So
    # attempt the upload and let the server rule on it - a client with an empty store
    # fails immediately with "No authentication provided" rather than prompting, and
    # the rejected-login branch below turns that into a skip. Skipping here up front
    # would make the RECOMMENDED credential route the one that never runs.
    Write-Host "auth:     none passed - using testcentercmd's own credential store"
}

$labelArgs = @($labels | ForEach-Object { "--label=$_" })

# --interactive=no is what keeps a pipeline from stopping at a credential prompt.
# One call with every file: Test Center groups them into the named batch itself.
#
# The output is captured as well as shown, because a REJECTED LOGIN has to be told
# apart from a broken upload: "no access to this server yet" is a skip like any other
# licence-bound obstacle, while a server that accepted the credentials and then failed
# is a real error worth stopping for.
$arguments = @("--url=$Url") + $auth + @('--interactive=no', 'upload', $Project,
    "--batch=$Batch") + $labelArgs + @($xml | ForEach-Object { $_.FullName })
$output = & $cmd @arguments 2>&1 | Out-String
$rc = $LASTEXITCODE
Write-Host $output

if ($rc -ne 0) {
    if ($output -match '(?i)unauthor|forbidden|not authenticated|invalid (user|password|token|credential)|authentication|401|403|login') {
        Write-Host ''
        Write-Host 'SKIPPED: Test Center did not accept the login, so nothing was uploaded.' -ForegroundColor Yellow
        Write-Host '         Store a token once - no secret in a command line or a variable:' -ForegroundColor DarkGray
        Write-Host "             $cmd config token <value>" -ForegroundColor DarkGray
        Write-Host '         The value comes from the Test Center UI under ADMIN -> USER' -ForegroundColor DarkGray
        Write-Host '         MANAGEMENT, where the product calls it an UPLOAD TOKEN (route' -ForegroundColor DarkGray
        Write-Host '         /admin/accesstokens). It is NOT in the user menu, and it is NOT' -ForegroundColor DarkGray
        Write-Host '         the public RSA key shown for the Jira application link.' -ForegroundColor DarkGray
        Write-Host '         TESTCENTER_TOKEN or TESTCENTER_USER + TESTCENTER_PASSWORD also work.' -ForegroundColor DarkGray
        Write-Host "         Not a build gate: the results stay in $Results for a later upload." -ForegroundColor DarkGray
        exit 3
    }
    Write-Error "Test Center upload failed (rc=$rc) - the results are still in $Results"
    exit $rc
}
Write-Host "batch $Batch is at $Url (project $Project)" -ForegroundColor Green
exit 0
