# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Publish a release: the binaries, the documentation and the QUALIFICATION
    evidence — but only when there is proof that the pipeline ran green.

.DESCRIPTION
    The PowerShell counterpart of tools/publish_release.sh, kept in lockstep.

    A release is a claim: "this build passed". This repository exists to back such
    claims with artefacts, so nothing is uploaded until the artefacts are present,
    consistent and NEWER than the sources they describe. Publishing yesterday's
    evidence for today's code is the one failure a packaging script can have that
    nobody notices.

    Verified first:
      * a clean working tree (a release built from uncommitted code cannot be
        reproduced by anyone, including you — -AllowDirty exists for a dry run)
      * JUnit results present, no failures, newer than the newest tracked source
      * every analyzer output present and totalling ZERO findings
      * the metrics ratchet and requirements traceability still pass (seconds, so
        they are re-run rather than trusted)
      * the quality-report PDF present and newer than the sources

    Published:
      binaries        downloads\*.AppImage, *.zip, *.apk (+ .sha256) whose NAME
                      carries this version; others are listed and skipped, because
                      .github\workflows\release.yml rebuilds all four platforms on
                      the tag and those are the ones to attach
      documentation   docs\*.md, the Doxygen HTML and the traceability matrix, as
                      TradingApp-<version>-docs.zip
      qualification   the quality PDF, requirements/design/test-spec documents,
                      every JUnit XML and every analyzer output, as
                      TradingApp-<version>-qualification.zip — the bundle CI cannot
                      produce alone, since its Axivion section needs a licensed Suite

.PARAMETER Tag
    The release tag. Defaults to v<version from CMakeLists.txt>.

.PARAMETER DryRun
    Say exactly what would be published and upload nothing.

.PARAMETER AllowDirty
    Proceed with a modified working tree (for a dry run).
#>
param(
    [string]$Tag = '',
    [switch]$DryRun,
    [switch]$AllowDirty
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$version = (Select-String -Path (Join-Path $Root 'CMakeLists.txt') `
        -Pattern 'VERSION (\d+\.\d+\.\d+)' | Select-Object -First 1).Matches.Groups[1].Value
if (-not $Tag) { $Tag = "v$version" }
$out = Join-Path $Root 'downloads'
$fail = $false

function Write-Ok { param([string]$m) Write-Host "  ok    $m" -ForegroundColor Green }
function Write-Bad { param([string]$m) Write-Host "  FAIL  $m" -ForegroundColor Red; $script:fail = $true }
function Write-Note { param([string]$m) Write-Host "  note  $m" -ForegroundColor DarkGray }

Write-Host "== publishing TradingApp $version as $Tag ==" -ForegroundColor Cyan
Write-Host ''
Write-Host '-- evidence that the pipeline ran --'

# 1. A reproducible starting point.
if ((& git status --porcelain)) {
    if ($AllowDirty) { Write-Note 'working tree is modified (-AllowDirty)' }
    else { Write-Bad 'working tree is modified - commit first, or pass -AllowDirty for a dry run' }
}
else { Write-Ok "working tree clean at $(& git rev-parse --short HEAD)" }

# 2. The newest tracked source file is the yardstick.
$tracked = @(& git ls-files 'src/*' 'tests/*' 'CMakeLists.txt' 'tests/CMakeLists.txt')
$newestSrc = [datetime]::MinValue
$newestSrcFile = ''
foreach ($f in $tracked) {
    $p = Join-Path $Root $f
    if (Test-Path $p) {
        $t = (Get-Item $p).LastWriteTimeUtc
        if ($t -gt $newestSrc) { $newestSrc = $t; $newestSrcFile = $f }
    }
}
function Test-NewerThanSources {
    param([Parameter(Mandatory)][string]$Path)
    return ((Get-Item $Path).LastWriteTimeUtc -ge $newestSrc)
}

# 3. Tests: present, no failures, newer than the sources.
$xml = @(Get-ChildItem (Join-Path $Root 'test-results') -Filter '*.xml' -Recurse -File -ErrorAction SilentlyContinue)
$cases = 0
if ($xml.Count -eq 0) { Write-Bad 'no JUnit results in test-results\ - run .\build_all.ps1 test' }
else {
    $failures = 0; $errors = 0
    foreach ($f in $xml) {
        [xml]$doc = Get-Content $f.FullName
        foreach ($suite in $doc.SelectNodes('//testsuite')) {
            $failures += [int]$suite.failures
            $errors += [int]$suite.errors
        }
        $cases += $doc.SelectNodes('//testcase').Count
    }
    $newestXml = ($xml | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1)
    if ($failures -ne 0 -or $errors -ne 0) {
        Write-Bad "$($xml.Count) suites: $failures failure(s), $errors error(s)"
    }
    elseif (Test-NewerThanSources $newestXml.FullName) {
        Write-Ok "$($xml.Count) suites, $cases cases, 0 failures"
    }
    else { Write-Bad "test results are older than $newestSrcFile - re-run .\build_all.ps1 test" }
}

# 4. Analyzers: every output present, and every one of them empty.
$analyzers = @('cppcheck', 'clang-tidy', 'msvc-analyze', 'pmd-cpd', 'codespell')
$total = 0; $missing = $false
foreach ($a in $analyzers) {
    $p = Join-Path $Root "analysis-results\$a.txt"
    if (-not (Test-Path $p)) { $missing = $true; continue }
    $total += @(Get-Content $p | Where-Object { $_.Trim() }).Count
}
if ($missing) { Write-Bad 'analyzer outputs missing from analysis-results\ - run .\build_all.ps1 analysis' }
elseif ($total -ne 0) { Write-Bad "$total analyzer finding(s) - a release claims zero" }
else { Write-Ok "$($analyzers.Count) analyzers, 0 findings" }

# 5. The two gates that cost seconds are re-run rather than trusted.
& python tools\lizard_metrics.py . analysis-results *> $null
if ($LASTEXITCODE -eq 0) { Write-Ok 'code-metrics ratchet clean' }
else { Write-Bad 'code-metrics ratchet - see python tools\lizard_metrics.py . analysis-results' }
$trace = (& python tools\trace_report.py 2>&1) -join "`n"
if ($LASTEXITCODE -eq 0 -and $trace -match '0 hard gaps') {
    $reqs = ([regex]'(\d+) requirements').Match($trace).Value
    Write-Ok "requirements traceability: $reqs, 0 hard gaps"
}
else { Write-Bad 'requirements traceability has hard gaps - python tools\trace_report.py' }

# 6. The report itself.
$pdf = Join-Path $out 'TradingApp-quality-report.pdf'
if (-not (Test-Path $pdf)) { Write-Bad 'no quality report - run python tools\make_report.py' }
elseif (Test-NewerThanSources $pdf) {
    Write-Ok "quality report $((Get-Item $pdf).LastWriteTime.ToString('yyyy-MM-dd HH:mm'))"
}
else { Write-Bad "quality report is older than $newestSrcFile - re-run python tools\make_report.py" }

Write-Host ''
if ($fail) {
    Write-Host 'REFUSING to publish: the evidence above does not support the claim a release makes.' -ForegroundColor Red
    Write-Host 'Run .\build_all.ps1 (or the named stages) and try again.'
    exit 1
}

# --- assemble ---------------------------------------------------------------
Write-Host '-- assembling --'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$docsZip = Join-Path $out "TradingApp-$version-docs.zip"
$qualZip = Join-Path $out "TradingApp-$version-qualification.zip"

# The BINARIES are collected FIRST, before this script writes any archive of its
# own: the archives carry the same version in their name, so a scan afterwards
# would match them too and attach each one twice.
$ownNames = @([IO.Path]::GetFileName($docsZip), [IO.Path]::GetFileName($qualZip),
    "$([IO.Path]::GetFileName($docsZip)).sha256", "$([IO.Path]::GetFileName($qualZip)).sha256",
    'TradingApp-quality-report.pdf')
$binAssets = @(); $skipped = @()
foreach ($f in @(Get-ChildItem $out -File | Where-Object {
            $_.Extension -in @('.AppImage', '.zip', '.apk', '.sha256') -or $_.Name -like '*.AppImage'
        } | Sort-Object Name)) {
    if ($ownNames -contains $f.Name) { continue }
    if ($f.Name -like "*$version*") { $binAssets += $f.FullName }
    else { $skipped += $f.Name }
}

Remove-Item -Force -ErrorAction SilentlyContinue $docsZip, $qualZip, "$docsZip.sha256", "$qualZip.sha256"

# The CORRESPONDING SOURCE, which GPL-3.0-or-later obliges us to offer alongside every
# binary (see THIRD_PARTY_LICENSES.md). git archive exports exactly what the tag holds,
# so a local edit that never got committed cannot slip into the published source.
$sourceTgz = Join-Path $out "TradingApp-$version-source.tar.gz"
Remove-Item -Force -ErrorAction SilentlyContinue $sourceTgz, "$sourceTgz.sha256"
& git archive --format=tar.gz --prefix="TradingApp-$version/" -o $sourceTgz HEAD
if ($LASTEXITCODE -eq 0 -and (Test-Path $sourceTgz)) {
    Write-Ok "source         $([IO.Path]::GetFileName($sourceTgz)) ($([math]::Round((Get-Item $sourceTgz).Length/1MB,1)) MB)"
} else {
    Write-Bad "could not produce the corresponding source archive - GPL-3.0-or-later requires it"
}

# The licence texts travel with the docs zip too, so a recipient of that alone has the terms.
$docItems = @(Get-ChildItem 'docs' -Filter '*.md' -File | ForEach-Object { $_.FullName })
$docItems += (Join-Path $Root 'README.md'), (Join-Path $Root 'LICENSE'),
    (Join-Path $Root 'THIRD_PARTY_LICENSES.md'), (Join-Path $Root 'LICENSES')
foreach ($extra in @('docs\traceability.html', 'docs\html', 'docs\uml')) {
    $p = Join-Path $Root $extra
    if (Test-Path $p) { $docItems += $p }
}
Compress-Archive -Path $docItems -DestinationPath $docsZip -Force
Write-Ok "documentation  $([IO.Path]::GetFileName($docsZip)) ($([math]::Round((Get-Item $docsZip).Length/1MB,1)) MB)"

$qualItems = @($pdf)
foreach ($item in @('docs\requirements.md', 'docs\design.md', 'docs\test_spec.md',
        'docs\verification.md', 'docs\vmodel.md', 'requirements\requirements.sdoc',
        'test-results', 'analysis-results', 'docs\traceability.html', 'coverage')) {
    $p = Join-Path $Root $item
    if (Test-Path $p) { $qualItems += $p }
}
Compress-Archive -Path $qualItems -DestinationPath $qualZip -Force
Write-Ok "qualification  $([IO.Path]::GetFileName($qualZip)) ($([math]::Round((Get-Item $qualZip).Length/1MB,1)) MB)"

foreach ($z in @($docsZip, $qualZip, $sourceTgz)) {
    if (-not (Test-Path $z)) { continue }
    (Get-FileHash $z -Algorithm SHA256).Hash.ToLower() + "  " + [IO.Path]::GetFileName($z) |
        Set-Content -Path "$z.sha256" -Encoding ascii
}

$assets = @($pdf, $docsZip, "$docsZip.sha256", $qualZip, "$qualZip.sha256") + $binAssets
# The source archive is what makes shipping the GPL binaries lawful, so it is always attached.
if (Test-Path $sourceTgz) { $assets += @($sourceTgz, "$sourceTgz.sha256") }
foreach ($f in $binAssets) { Write-Ok "binary         $([IO.Path]::GetFileName($f))" }
if ($binAssets.Count -eq 0) {
    Write-Note "no binary for $version in downloads\ - .github\workflows\release.yml builds"
    Write-Note '    all four platforms on the tag and attaches them there'
}
foreach ($f in $skipped) { Write-Note "skipped (built for another version): $f" }

Write-Host ''
Write-Host '-- publishing --'
if ($DryRun) {
    Write-Host "(dry run) would attach $($assets.Count) asset(s) to $Tag"
    foreach ($f in $assets) { Write-Host "    $([IO.Path]::GetFileName($f))" }
    exit 0
}
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Error 'gh CLI not installed - the assets are in downloads\, attach them by hand'
    exit 1
}

$notes = New-TemporaryFile
@"
Built and verified on $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm')) UTC from $(& git rev-parse --short HEAD).

| Evidence | Result |
|---|---|
| Test suite | $cases cases, 0 failures |
| Static analysis | 0 findings |
| Code metrics | ratchet clean |
| Traceability | 0 hard gaps |

``TradingApp-$version-qualification.zip`` holds the full evidence set: the quality
report PDF (including the Axivion MISRA C++ 2023 result, which a public runner cannot
produce), the requirements/design/test-spec documents, every JUnit XML and every
analyzer output. ``TradingApp-$version-docs.zip`` holds the documentation and the
generated API reference.
"@ | Set-Content -Path $notes.FullName -Encoding utf8

& gh release view $Tag *> $null
if ($LASTEXITCODE -ne 0) {
    & gh release create $Tag --title "eToro Trader $Tag" --notes-file $notes.FullName --latest
    if ($LASTEXITCODE -ne 0) { Write-Error 'gh release create failed - does the tag exist on the remote?'; exit 1 }
    Write-Host "created release $Tag"
}
else {
    & gh release edit $Tag --notes-file $notes.FullName --prerelease=false --draft=false --latest *> $null
    Write-Host "updated release $Tag"
}
Remove-Item $notes.FullName -Force

# gh release upload can fail PARTWAY through its own batch (measured on Linux: a
# transient API 404 on one asset aborted the whole call, silently skipping every
# asset still queued after it). Retry the whole batch on failure - --clobber makes a
# retry safe - then verify by NAME that every asset this run means to publish
# actually landed, rather than trusting the command's exit code alone.
$uploadAttempts = 0
while ($true) {
    & gh release upload $Tag @assets --clobber
    if ($LASTEXITCODE -eq 0) { break }
    $uploadAttempts++
    if ($uploadAttempts -ge 3) {
        Write-Error "gh release upload failed after $uploadAttempts attempts - see the error above"
        exit 1
    }
    Write-Host "gh release upload failed (attempt $uploadAttempts) - retrying the batch"
    Start-Sleep -Seconds 5
}

Write-Host ''
Write-Host "attached $($assets.Count) asset(s) to $Tag"
$published = & gh release view $Tag --json assets -q '.assets[].name'
$missing = @()
foreach ($f in $assets) {
    $base = Split-Path -Leaf $f
    if ($published -notcontains $base) { $missing += $base }
}
if ($missing.Count -gt 0) {
    Write-Error "upload reported success but $($missing.Count) asset(s) are NOT on the release:"
    foreach ($f in $missing) { Write-Host "    $f" }
    exit 1
}
$published | ForEach-Object { Write-Host "    $_" }
