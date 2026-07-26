---
name: verify
description: Run the full TradingApp quality pipeline (build, tests, traceability, docs, coverage, static analysis, sanitizers — everything except the slow Axivion stage) and report a pass/fail summary with the first actionable error per failed stage. Use when the user asks to "verify everything", "run all checks", or before committing/pushing.
---

# Verify the whole project (without Axivion)

1. Run `./build_all.sh --skip axivion` (from the repo root; takes ~15–30 min —
   run it in the background and monitor).
2. While it runs, do not modify repository files (the sanitize/coverage stages
   rebuild from the sources mid-run).
3. When it finishes, read the stage summary at the end of the output. For every
   FAILED stage, extract the first real error:
   - `build`: first compiler error.
   - `test`: failing test function names from the Totals lines.
   - `trace`: hard-gap lines from `tools/trace_report.py` (a hard gap means a
     test references an unknown REQ/TS id, or spec and implementation diverge —
     usually a missing `@relation`/`@tstid` tag or a requirement not yet added
     to `requirements/requirements.sdoc`).
   - `analysis`: per-tool finding counts (cppcheck / clang-tidy / clazy /
     g++ -fanalyzer) from `analysis-results/*.txt`. Fix findings for real, or
     — only for check families that fight the codebase idioms — disable the
     check in `.clang-tidy` WITH a written rationale.
   - `sanitize`: grep the `analysis-results/sanitize-*.raw.txt` logs for
     SUMMARY lines.
   - `coverage`: usually an lcov/llvm-cov tooling error, not a code problem.
4. Report the summary table plus, for each failure, file:line and the fix you
   applied or propose. All green = say so explicitly, with test/requirement
   counts from the trace line.

Rules: never weaken a check silently; keep every new boolean decision at ≤ 6
conditions (clang-18 MC/DC limit); new tests carry the tag block
(`//! @tstid TS-… @design DES-…` + `// @relation(REQ-…, scope=function)`).
