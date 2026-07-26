#!/usr/bin/env bash
# Requirements-as-code toolchain (StrictDoc; see strictdoc_config.py):
#
#   1. Validate + export requirements/requirements.sdoc to HTML with
#      requirement ↔ source traceability (the tests carry
#      @relation(REQ-…, scope=function) markers), a traceability matrix and
#      project statistics → docs/strictdoc/html/index.html
#   2. Regenerate docs/requirements.md — the Doxygen page and one input leg
#      of tools/trace_report.py — from the same .sdoc (tools/sdoc_to_md.py).
#
# The .sdoc file is the single source of truth; never edit
# docs/requirements.md by hand.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="$HOME/.local/bin:$PATH" # pipx-installed strictdoc

if ! command -v strictdoc >/dev/null 2>&1; then
    echo "strictdoc not installed (pipx install strictdoc) — export skipped," >&2
    echo "docs/requirements.md left as committed." >&2
    exit 0
fi

strictdoc export "$ROOT" --output-dir "$ROOT/docs/strictdoc"
python3 "$ROOT/tools/sdoc_to_md.py"
echo "requirements: docs/strictdoc/html/index.html (+ docs/requirements.md regenerated)"
