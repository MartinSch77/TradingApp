#!/usr/bin/env bash
# Supply-chain evidence, using whichever of the free tools are installed
# (./setup.sh installs all three; each missing tool is reported and skipped):
#
#   syft    software bill of materials for the repository in BOTH standard
#           formats -> analysis-results/supply-chain/sbom.spdx.json and
#           sbom.cyclonedx.json
#   grype   known-vulnerability scan over that SBOM -> grype.txt
#   trivy   repository scan: dependencies, misconfigurations and SECRETS
#           -> trivy.txt (a leaked key in the repo fails the run)
#
# Exit code 1 when trivy finds secrets or grype finds Critical vulnerabilities.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/analysis-results/supply-chain"
mkdir -p "$OUT"
export PATH="$HOME/.local/bin:$PATH"
FAIL=0

if command -v syft >/dev/null 2>&1; then
    echo "== syft SBOM =="
    syft scan "dir:$ROOT" -o spdx-json="$OUT/sbom.spdx.json" \
        -o cyclonedx-json="$OUT/sbom.cyclonedx.json" -q
    echo "SBOM: $OUT/sbom.spdx.json + sbom.cyclonedx.json"
else
    echo "syft not installed — SBOM skipped (./setup.sh installs it)"
fi

if command -v grype >/dev/null 2>&1 && [ -f "$OUT/sbom.spdx.json" ]; then
    echo "== grype vulnerability scan (over the SBOM) =="
    grype "sbom:$OUT/sbom.spdx.json" -q --fail-on critical | tee "$OUT/grype.txt" || FAIL=1
else
    echo "grype not installed or no SBOM — vulnerability scan skipped"
fi

if command -v trivy >/dev/null 2>&1; then
    echo "== trivy repository scan (vuln, misconfig, secret) =="
    # apiKeyEtoro.json is the INTENDED local secret store (git-ignored, never
    # committed — verified against the full history); skipping it locally keeps
    # the gate meaningful. The CI scan covers the repository content, where the
    # file never exists.
    trivy fs --scanners vuln,misconfig,secret --exit-code 1 \
        --skip-files "apiKeyEtoro.json" \
        --skip-dirs "build,build-cov-gcc,build-cov-mcdc,build-cov-coco,build-san,build-san-tsan,build-release,build_axivion,docs/html,docs/strictdoc,docs/sphinx-html,tools/third-party" \
        --quiet "$ROOT" | tee "$OUT/trivy.txt" || FAIL=1
else
    echo "trivy not installed — repository scan skipped"
fi

exit $FAIL
