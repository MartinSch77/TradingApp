# TradingApp — project instructions

Qt 6 / C++23 desktop app trading eToro instruments via the official public
API; SIMULATION mode without keys. Quality toolchain is the point of this
repo: requirements-as-code, full traceability, four static analyzers + three
sanitizers on one Axivion dashboard.

## Entry points

```bash
./setup.sh                     # provision/update all tools (naked Debian/Ubuntu)
./build_all.sh                 # everything incl. Axivion; --skip axivion; app; release
./clean_all.sh [--deep]        # remove everything generated
tools/run_tests.sh build       # test suite with JUnit output
python3 tools/trace_report.py  # traceability matrix; fails on hard gaps
tools/static_analysis.sh build [--fix]   # cppcheck+clang-tidy+clazy+g++ -fanalyzer
tools/sanitize.sh [asan-ubsan|tsan|valgrind|all]
tools/profile.sh               # perf/gperftools over build-release/
```

Skills: `/verify` (all checks), `/axivion-dashboard` (run + REST verification),
`/add-requirement` (requirements-as-code workflow), `/perf-check` (benchmarks).

## Non-negotiables

- Requirements live ONLY in `requirements/requirements.sdoc` (StrictDoc);
  `docs/requirements.md` is generated (`tools/make_requirements.sh`).
- Every test carries `//! @tstid TS-… @design DES-…` plus
  `// @relation(REQ-…, scope=function)` (plain `//` — StrictDoc ignores `//!`).
  Test classes write `Q_OBJECT;` (semicolon = tree-sitter parse anchor).
- Keep every boolean decision ≤ 6 conditions (clang-18 MC/DC limit).
- Header-inline functions that grow logic: define out-of-line in one TU
  (comdat coverage records otherwise break llvm-cov).
- Layering is linker-enforced: domain (Qt Core only) ← services ← ui.
- Money-moving actions need the double-press gate; advisory features never
  trade (REQ-N-005). Secrets only in git-ignored `apiKeyEtoro.json`.
- Monte-Carlo/plan building stay off the GUI thread (QtConcurrent); the
  positions table stays model/view, allocation-free per tick (REQ-N-006).
- ONE Axivion run at a time (flock in `axivion/start_analysis.sh`); no
  clean/build while it runs. External findings import: `axivion/external_import.py`
  (Python layer — matchlist is not expressible in the JSON configs).
- Check `.clang-tidy` header comments before disabling checks; disable only
  with a written rationale.

## Gotchas that cost hours (details: docs/verification.md, docs/tools.md)

- TSan vs non-TSan Qt: `ignore_noninstrumented_modules=1` is load-bearing
  (otherwise false "unlock of unlocked mutex" + watchdog DEADLOCK).
- clang-tidy `--fix` breaks Qt: never let it remove the `private:` after
  `private slots:` nor rewrite guarded QEvent static_casts (both disabled).
- valgrind: QtTest watchdog TLS "possibly lost" is suppressed in
  tools/valgrind.supp; g++ -fanalyzer `<unknown>`-value reports are filtered
  (experimental C++ FPs).
