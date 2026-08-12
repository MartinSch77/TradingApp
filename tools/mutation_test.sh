#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# Mull mutation testing pilot (2026-08-12): measures whether a test suite's assertions are
# actually SENSITIVE to a change in the code they exercise, not just whether they execute it
# — the gap plain coverage cannot see. Runs mull-runner over a small, curated set of domain
# test programs, each scoped (via a per-run mull.yml) to the ONE source file it is meant to
# pilot: without that scoping Mull mutates the WHOLE linked trading_domain library, because
# every test binary links the whole static library, and the resulting score would answer "how
# much of the unrelated codebase happened to be exercised" rather than the question this pilot
# exists to ask (measured: an unscoped run on tst_pathoutcome reported 435 mutants across the
# entire domain library and a 4% score — almost none of them in PathOutcome.cpp itself).
#
# DELIBERATELY INFORMATIONAL, not a gate: this is a pilot establishing the capability and a
# first baseline, not yet a ratchet like tools/lizard_metrics.py. Exits 0 whenever the runs
# complete, whatever the mutation scores were — the same "measured, not yet enforced" stance
# CLAUDE.md documents for SonarCloud/Coverity, until a real threshold policy exists to enforce.
#
# Linux/clang only — Mull needs an LLVM pass plugin matched to the host's clang major version;
# there is no Windows counterpart (mirrors the clazy/TSan/valgrind split already in this
# project). Exits 3 ("skipped") when clang >= 18 or Mull itself (./setup.sh mull) is missing —
# the project's own convention for a tool-bound stage that stays green without failing the build.
#
# Usage: tools/mutation_test.sh [target:source-regex ...]
#   Defaults to this pilot's own four programs when no arguments are given.
#   target        a CMake test target name (e.g. tst_confirmgate)
#   source-regex  matched against the mutated file's path (mull.yml includePaths) — scope
#                 this to the ONE file the target is meant to pilot, or the score means little

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/common.sh
source "$ROOT/tools/common.sh"

EXIT_SKIPPED=3

suffix="$(llvm_suffix)" || { echo "mutation_test: no clang >= 18 found — skipped" >&2; exit $EXIT_SKIPPED; }
major="${suffix#-}"
MULL_DIR="${MULL_DIR:-$HOME/.local/mull}"
RUNNER="$MULL_DIR/usr/bin/mull-runner-$major"
IR_FRONTEND="$MULL_DIR/usr/lib/mull-ir-frontend-$major"
if [ ! -x "$RUNNER" ] || [ ! -f "$IR_FRONTEND" ]; then
    echo "mutation_test: Mull not installed for clang$suffix — run ./setup.sh mull — skipped" >&2
    exit $EXIT_SKIPPED
fi

DEFAULT_PILOT=(
    "tst_confirmgate:.*ConfirmGate\\.cpp"
    "tst_positionmath:.*PositionMath\\.cpp"
    "tst_money:.*domain/Money\\.cpp"
    "tst_pathoutcome:.*PathOutcome\\.cpp"
)
if [ "$#" -eq 0 ]; then
    PAIRS=("${DEFAULT_PILOT[@]}")
else
    PAIRS=("$@")
fi

BUILD_DIR="${MULL_BUILD_DIR:-$ROOT/build-mull}"
QT_PREFIX="$(qt_prefix)"
echo "== configuring $BUILD_DIR (clang++$suffix, Mull IR frontend) =="
# shellcheck disable=SC2191  # the trailing array element is conditional on QT_PREFIX
CONFIG_ARGS=(-S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_CXX_FLAGS="-O0 -fpass-plugin=$IR_FRONTEND -g -grecord-command-line")
[ -n "$QT_PREFIX" ] && CONFIG_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
CXX="clang++$suffix" CC="clang$suffix" cmake "${CONFIG_ARGS[@]}" \
    >/tmp/mutation_test_configure.log 2>&1 ||
    { echo "configure failed — see /tmp/mutation_test_configure.log" >&2; exit 1; }

targets=()
for pair in "${PAIRS[@]}"; do
    targets+=("${pair%%:*}")
done
echo "== building: ${targets[*]} =="
cmake --build "$BUILD_DIR" --target "${targets[@]}" -j"$(nproc)" \
    >/tmp/mutation_test_build.log 2>&1 ||
    { echo "build failed — see /tmp/mutation_test_build.log" >&2; exit 1; }

OUT_DIR="${MULL_OUT_DIR:-$ROOT/analysis-results}"
mkdir -p "$OUT_DIR"
REPORT="$OUT_DIR/mutation-pilot.txt"
: > "$REPORT"

for pair in "${PAIRS[@]}"; do
    target="${pair%%:*}"
    pattern="${pair#*:}"
    binary="$BUILD_DIR/tests/$target"
    if [ ! -x "$binary" ]; then
        echo "$target: binary not found ($binary) — skipped" | tee -a "$REPORT"
        continue
    fi
    cat > "$ROOT/mull.yml" <<YAML
includePaths:
  - $pattern
YAML
    {
        echo "== $target (scoped to $pattern) =="
        "$RUNNER" --workers "$(nproc)" "$binary" 2>&1 |
            grep -Ev "Could not find dynamic library|cannot find config|^[[:space:]]*\[#"
        echo
    } | tee -a "$REPORT"
    rm -f "$ROOT/mull.yml"
done

echo "wrote $REPORT"
