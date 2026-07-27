<#
.SYNOPSIS
    Windows counterpart of tools/supply_chain.sh — supply-chain evidence, using
    whichever of the free tools are installed.

.DESCRIPTION
      syft    software bill of materials for the repository in BOTH standard
              formats -> analysis-results\supply-chain\sbom.spdx.json and
              sbom.cyclonedx.json
      grype   known-vulnerability scan over that SBOM -> grype.txt
      trivy   repository scan: dependencies, misconfigurations and SECRETS
              -> trivy.txt (a leaked key in the repo fails the run)

    All three ship native Windows binaries; .\setup.ps1 install downloads them
    into %LOCALAPPDATA%\bin. Each missing tool is reported and skipped.

    Exit code 1 when trivy finds secrets or grype finds Critical vulnerabilities.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot
$Out = Join-Path $Root 'analysis-results\supply-chain'
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

$fail = 0
$sbom = Join-Path $Out 'sbom.spdx.json'

if (Test-Tool 'syft') {
    Write-Stage 'syft SBOM'
    & syft scan "dir:$Root" -o "spdx-json=$sbom" -o "cyclonedx-json=$(Join-Path $Out 'sbom.cyclonedx.json')" -q
    Write-Host "SBOM: $sbom + sbom.cyclonedx.json"
} else {
    Write-Skip "syft not installed — SBOM skipped (.\setup.ps1 install downloads it)"
}

if ((Test-Tool 'grype') -and (Test-Path $sbom)) {
    Write-Stage 'grype vulnerability scan (over the SBOM)'
    & grype "sbom:$sbom" -q --fail-on critical | Tee-Object -FilePath (Join-Path $Out 'grype.txt')
    if ($LASTEXITCODE -ne 0) { $fail = 1 }
} else {
    Write-Skip "grype not installed or no SBOM — vulnerability scan skipped"
}

if (Test-Tool 'trivy') {
    Write-Stage 'trivy repository scan (vuln, misconfig, secret)'
    # apiKeyEtoro.json is the INTENDED local secret store (git-ignored, never
    # committed — verified against the full history); skipping it locally keeps
    # the gate meaningful. The CI scan covers the repository content, where the
    # file never exists.
    & trivy fs --scanners vuln,misconfig,secret --exit-code 1 `
        --skip-files 'apiKeyEtoro.json' `
        --skip-dirs 'build,build-cov-gcc,build-cov-mcdc,build-cov-coco,build-cov-msvc,build-san,build-san-tsan,build-san-ubsan,build-release,build_axivion,docs/html,docs/strictdoc,docs/sphinx-html,tools/third-party' `
        --quiet $Root | Tee-Object -FilePath (Join-Path $Out 'trivy.txt')
    if ($LASTEXITCODE -ne 0) { $fail = 1 }
} else {
    Write-Skip "trivy not installed — repository scan skipped"
}

exit $fail
