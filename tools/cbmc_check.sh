#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# CBMC bounded-model-checking proof(s) (tooling backlog item 7, optional and narrow
# by design): CBMC proves properties over EVERY possible input in its bound, not just
# the ones a test or a fuzzer happened to try — but it ships no model for Qt's
# headers, so it can only reach code with NO Qt dependency at all. That is a small
# slice of this Qt-heavy codebase; see cbmc/priceDecimals_proof.cpp for the one
# function pulled out specifically because it qualifies, and why the other
# candidates considered (PositionMath's other functions, Indicators, ConfirmGate)
# do not.
#
# DELIBERATELY INFORMATIONAL, not a gate — narrower even than Mull/libFuzzer's
# "measured, not yet enforced" stance, since this proves one property of one
# function rather than establishing a repeatable coverage metric. Exits 3
# ("skipped") when CBMC is not installed (./setup.sh cbmc). Linux only: CBMC IS
# cross-platform upstream, but this project's installer uses `apt-get download`
# against Ubuntu's own archive (no root needed — mirrors tools/mutation_test.sh's
# Mull install), so there is no Windows counterpart yet.
#
# Usage: tools/cbmc_check.sh

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

EXIT_SKIPPED=3
CBMC_DIR="${CBMC_DIR:-$HOME/.local/cbmc}"
CBMC_BIN="$CBMC_DIR/usr/bin/cbmc"
CBMC_LIB="$CBMC_DIR/usr/lib"

if command -v cbmc >/dev/null 2>&1; then
    CBMC_BIN="cbmc"
elif [ ! -x "$CBMC_BIN" ]; then
    echo "cbmc_check: cbmc not found — run ./setup.sh cbmc — skipped" >&2
    exit $EXIT_SKIPPED
fi

OUT_DIR="${CBMC_OUT_DIR:-$ROOT/analysis-results}"
mkdir -p "$OUT_DIR"
REPORT="$OUT_DIR/cbmc-report.txt"
: > "$REPORT"

PROOFS=(
    "priceDecimals:cbmc/priceDecimals_proof.cpp:src/domain/PriceDecimalsCore.cpp"
)

overall_status=0
for entry in "${PROOFS[@]}"; do
    name="${entry%%:*}"
    rest="${entry#*:}"
    harness="${rest%%:*}"
    impl="${rest#*:}"
    {
        echo "== $name =="
        LD_LIBRARY_PATH="$CBMC_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            "$CBMC_BIN" -I "$ROOT/src" -DTRADINGAPP_CBMC_PROOF --cpp11 "$harness" "$impl" 2>&1
        status=$?
        if [ $status -ne 0 ]; then
            overall_status=1
        fi
        echo
    } | tee -a "$REPORT"
done

echo "wrote $REPORT"
exit $overall_status
