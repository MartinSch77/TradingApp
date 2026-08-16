#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Unit tests + branch coverage for tools/*.py and tools/ml/*.py — the Python
# analogue of the C++ MC/DC gate (tools/coverage.sh mcdc), which clang-18/Coco
# never reach because they only ever see src/ and tests/. Two halves, two
# interpreters, because the ML modules need numpy/sklearn/xgboost/onnxruntime
# and the rest are stdlib-only on purpose:
#
#   tools/tests/         tools/*.py           run through the pipx `pytest` venv
#   tools/tests/ml/      tools/ml/*.py        run through the ML venv's own pytest
#                                             (tools/ml/requirements.txt, ./setup.sh ml)
#
# `--cov-branch` is the load-bearing flag: line coverage alone would call an
# `if` covered by hitting either arm once, which is exactly the gap MC/DC
# closes on the C++ side. Coverage is REPORTED, not gated on a hard number —
# see the threshold note below.
#
# Usage: tools/python_tests.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

EXIT_SKIPPED=3
mkdir -p analysis-results

find_pytest() {
    command -v pytest >/dev/null 2>&1 && { echo pytest; return; }
    [ -x "$HOME/.local/bin/pytest" ] && { echo "$HOME/.local/bin/pytest"; return; }
}

PYTEST_BIN="$(find_pytest)"
if [ -z "$PYTEST_BIN" ]; then
    echo "python_tests: 'pytest' not found — run ./setup.sh — skipped" >&2
    exit $EXIT_SKIPPED
fi

echo "== stdlib-only tool tests (tools/*.py) =="
STDLIB_STATUS=0
"$PYTEST_BIN" tools/tests --ignore=tools/tests/ml \
    --cov=tools --cov-branch --cov-report=term-missing \
    --cov-report="xml:analysis-results/pytest-cov-tools.xml" \
    --junitxml=analysis-results/pytest-tools.xml \
    -q || STDLIB_STATUS=$?

ML_VENV_DIR="${ML_VENV_DIR:-$HOME/.local/tradingapp-ml}"
ML_STATUS=0
if [ -x "$ML_VENV_DIR/bin/pytest" ]; then
    echo "== ML pipeline tests (tools/ml/*.py) =="
    "$ML_VENV_DIR/bin/pytest" tools/tests/ml \
        --cov=tools/ml --cov-branch --cov-report=term-missing \
        --cov-report="xml:analysis-results/pytest-cov-ml.xml" \
        --junitxml=analysis-results/pytest-ml.xml \
        -q || ML_STATUS=$?
else
    echo "== ML pipeline tests skipped: no ML venv ($ML_VENV_DIR) — ./setup.sh ml =="
fi

# A hard percentage floor is deliberately NOT enforced here yet: the suite is new
# (this file's own history: added 2026-08-16) and a ratchet needs a baseline run
# first, the same reasoning tools/lizard_metrics.py's own ratchet is built on.
# Report both figures either way so the gap is visible, not silent.
if [ "$STDLIB_STATUS" -ne 0 ]; then
    echo "python_tests: stdlib tool tests FAILED (exit $STDLIB_STATUS)" >&2
fi
if [ "$ML_STATUS" -ne 0 ]; then
    echo "python_tests: ML tool tests FAILED (exit $ML_STATUS)" >&2
fi
[ "$STDLIB_STATUS" -eq 0 ] && [ "$ML_STATUS" -eq 0 ]
