#!/usr/bin/env bash
# CPU profiling for TradingApp (REQ-N-006): runs the release binary under the
# best available profiler and prints the hotspot report.
#
#   tools/profile.sh [binary] [seconds]
#     binary   default: build-release/TradingApp (./build_all.sh release)
#     seconds  default: 30 — the app is profiled offscreen for this long
#
# Profiler pick, in order:
#   perf        (linux-tools; flame-graph-ready: perf.data kept)
#   gperftools  (LD_PRELOAD libprofiler; report via google-pprof / pprof)
# Output lands in analysis-results/profile/.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build-release/TradingApp}"
SECS="${2:-30}"
OUT="$ROOT/analysis-results/profile"
mkdir -p "$OUT"

if [ ! -x "$BIN" ]; then
    echo "binary not found: $BIN  (build it with: ./build_all.sh release)" >&2
    exit 2
fi
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

if command -v perf >/dev/null 2>&1 && perf stat true >/dev/null 2>&1; then
    echo "== perf record ($SECS s, offscreen) =="
    perf record -g --call-graph dwarf -o "$OUT/perf.data" -- timeout "$SECS" "$BIN" || true
    perf report --stdio -i "$OUT/perf.data" 2>/dev/null | head -40 | tee "$OUT/perf-top.txt"
    echo "full data: $OUT/perf.data   (perf report -i …, or a flame graph)"
    exit 0
fi

LIBPROFILER="$(ldconfig -p 2>/dev/null | grep -om1 '/[^ ]*libprofiler\.so[^ ]*' | head -1)"
if [ -n "$LIBPROFILER" ]; then
    echo "== gperftools CPU profiler ($SECS s, offscreen) =="
    CPUPROFILE="$OUT/cpu.prof" LD_PRELOAD="$LIBPROFILER" timeout "$SECS" "$BIN" || true
    PPROF="$(command -v google-pprof || command -v pprof || true)"
    if [ -n "$PPROF" ]; then
        "$PPROF" --text "$BIN" "$OUT/cpu.prof" 2>/dev/null | head -30 | tee "$OUT/pprof-top.txt"
    fi
    echo "profile: $OUT/cpu.prof"
    exit 0
fi

echo "no profiler found — install one of:" >&2
echo "  sudo apt install linux-tools-common linux-tools-generic   # perf" >&2
echo "  sudo apt install google-perftools libgoogle-perftools-dev # gperftools" >&2
echo "The domain hot paths are also benchmarked deterministically by" >&2
echo "build/tests/tst_benchmarks (QBENCHMARK) — no profiler needed for those." >&2
exit 2
