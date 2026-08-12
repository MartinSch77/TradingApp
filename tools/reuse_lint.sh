#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# REUSE/SPDX license-compliance lint (tooling backlog item 4) — every tracked file
# needs a copyright + SPDX license, either an inline header (the project-wide
# convention) or an entry in REUSE.toml for the formats where a header is
# impractical (JSON has no comment syntax) or risky to hand-edit (binaries,
# generated files). This is the LOCAL counterpart of the `reuse` job in
# .github/workflows/ci.yml (fsfe/reuse-action) — same tool, same repo root, so a
# failure here is the same failure CI would report, before pushing.
#
# `reuse` itself is a pure-Python CLI with no native deps: ./setup.sh installs it
# via pipx (PIPX_PKGS in setup.sh / $PipPkgs in setup.ps1). Exits 3 ("skipped")
# when it is not on PATH, the project's own convention for a tool-bound check that
# stays green without failing anything that does not need it.
#
# Usage: tools/reuse_lint.sh

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

EXIT_SKIPPED=3

REUSE_BIN="reuse"
if ! command -v "$REUSE_BIN" >/dev/null 2>&1; then
    # pipx installs land in ~/.local/bin, which may not be on a non-interactive PATH.
    if [ -x "$HOME/.local/bin/reuse" ]; then
        REUSE_BIN="$HOME/.local/bin/reuse"
    else
        echo "reuse_lint: 'reuse' not found — run ./setup.sh — skipped" >&2
        exit $EXIT_SKIPPED
    fi
fi

mkdir -p analysis-results
"$REUSE_BIN" lint | tee analysis-results/reuse.txt
