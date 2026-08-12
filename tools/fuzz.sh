#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# libFuzzer runner (tooling backlog item 3): builds the harnesses under fuzz/ into a
# SEPARATE build-fuzz tree (TRADINGAPP_BUILD_FUZZERS=ON there and nowhere else — see the
# root CMakeLists comment) and runs each one for a bounded time against its own seed
# corpus. This is a bug hunt, not a gate: exits 0 whenever every harness completes its
# run without libFuzzer reporting a crash, the same "measured, not yet enforced" stance
# this project already takes for Mull (tools/mutation_test.sh) and SonarCloud.
#
# libFuzzer is a clang compiler-rt runtime, not a separate install — any clang build
# already has it, so there is no ./setup.sh step (unlike Mull's prebuilt .deb). Linux/
# clang only: -fsanitize=fuzzer has no MSVC/Windows equivalent used here, matching the
# clazy/TSan/valgrind/Mull split already documented in CLAUDE.md.
#
# Usage: tools/fuzz.sh [seconds-per-target]
#   Defaults to a short 30 s smoke run per target — enough to catch a crash on trivial
#   malformed input without turning every CI run into a long fuzzing campaign. Pass a
#   larger budget (e.g. 3600) for an actual overnight/CI-scheduled fuzz run. A found
#   crash is kept under fuzz/crashes/<target>/ and reproduced by running the harness
#   binary directly on that one file.

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/common.sh
source "$ROOT/tools/common.sh"

EXIT_SKIPPED=3

suffix="$(llvm_suffix)" || { echo "fuzz.sh: no clang >= 18 found — skipped" >&2; exit $EXIT_SKIPPED; }

BUDGET="${1:-30}"
BUILD_DIR="${FUZZ_BUILD_DIR:-$ROOT/build-fuzz}"
QT_PREFIX="$(qt_prefix)"

TARGETS=(fuzz_tradescript fuzz_ollama_response fuzz_yahoo_chart)

echo "== configuring $BUILD_DIR (clang++$suffix, libFuzzer+ASan+UBSan) =="
CONFIG_ARGS=(-S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug
    -DTRADINGAPP_BUILD_FUZZERS=ON -DBUILD_TESTING=OFF)
[ -n "$QT_PREFIX" ] && CONFIG_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
CXX="clang++$suffix" CC="clang$suffix" cmake "${CONFIG_ARGS[@]}" \
    >/tmp/fuzz_configure.log 2>&1 ||
    { echo "configure failed — see /tmp/fuzz_configure.log" >&2; exit 1; }

echo "== building: ${TARGETS[*]} =="
cmake --build "$BUILD_DIR" --target "${TARGETS[@]}" -j"$(nproc)" \
    >/tmp/fuzz_build.log 2>&1 ||
    { echo "build failed — see /tmp/fuzz_build.log" >&2; exit 1; }

OUT_DIR="${FUZZ_OUT_DIR:-$ROOT/analysis-results}"
mkdir -p "$OUT_DIR"
REPORT="$OUT_DIR/fuzz-report.txt"
: > "$REPORT"

overall_status=0
for target in "${TARGETS[@]}"; do
    binary="$BUILD_DIR/fuzz/$target"
    # seed_dir is TRACKED (a handful of curated example inputs) and read-only to
    # libFuzzer; out_dir is where it WRITES every new corpus entry it discovers.
    # Passing seed_dir as the primary corpus argument would make libFuzzer add its
    # own minimized/interesting inputs directly into it — measured: a single 30 s
    # smoke run turned 3 tracked seed files into 53, all untracked noise. out_dir is
    # therefore the primary corpus and seed_dir is an EXTRA read-only input.
    seed_dir="$ROOT/fuzz/corpus/${target#fuzz_}"
    out_dir="${FUZZ_CORPUS_DIR:-$ROOT/analysis-results/fuzz-corpus}/${target#fuzz_}"
    crash_dir="$ROOT/fuzz/crashes/${target#fuzz_}"
    mkdir -p "$out_dir" "$crash_dir"
    {
        echo "== $target (${BUDGET}s, seeds: $seed_dir, corpus: $out_dir) =="
        if ! "$binary" -max_total_time="$BUDGET" -artifact_prefix="$crash_dir/" \
            "$out_dir" "$seed_dir" 2>&1; then
            echo "$target: libFuzzer reported a failure — artifact in $crash_dir"
            overall_status=1
        fi
        echo
    } | tee -a "$REPORT"
done

echo "wrote $REPORT"
exit $overall_status
