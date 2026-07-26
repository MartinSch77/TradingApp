---
name: perf-check
description: Measure TradingApp performance — deterministic domain benchmarks (Debug vs release), optional CPU profiling — and judge whether a change regressed the hot paths. Use for "is it faster/slower", performance regressions, or before/after comparisons of optimisation work.
---

# Performance measurement (TradingApp)

1. **Benchmarks** (deterministic, no profiler needed):
   ```bash
   ./build_all.sh build release        # both trees
   QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmarks | grep -E "RESULT|msecs"
   QT_QPA_PLATFORM=offscreen ./build-release/tests/tst_benchmarks | grep -E "RESULT|msecs"
   ```
   Hot paths covered: `monteCarlo` (TS-PERF-001), `buildTradePlan`
   (TS-PERF-002), `computeDecisionRows` over 25 instruments (TS-PERF-003).
   Baseline 2026-07-26 (Debug → release): 0.29 → 0.086 ms, 1.59 → 0.73 ms,
   4.5 → 0.22 ms. Compare against these; >2× regression = investigate.
2. **Profiling** (hotspot drill-down): `tools/profile.sh [binary] [seconds]` —
   auto-detects perf or gperftools; if neither is installed it prints the two
   apt install lines (sudo needed, ask the user).
3. **Architecture invariants to preserve** (REQ-N-006): Monte-Carlo and
   buildTradePlan run off the GUI thread (QtConcurrent watchers in
   MainWindow); the open-trades table refreshes allocation-free through
   `PositionsModel` (dataChanged, no per-tick item churn). Do not reintroduce
   synchronous MC calls or QTableWidgetItem churn on the poll path.
4. Report a before/after table with the three benchmark numbers and a verdict.
