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

Windows has a PowerShell counterpart for EVERY one of those (`setup.ps1`,
`build_all.ps1`, `clean_all.ps1`, `tools\*.ps1`, `axivion\start_analysis.ps1`);
the Python tools are shared verbatim. Both platforms are verified. Details and
the tool substitutions: `docs/windows.md`. When changing a `*.sh` script or a
shared Python tool, change its counterpart too — they are meant to stay in
lockstep.

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
- Stage exit code 3 = "skipped": the stage needs a license-bound or absent tool
  (Axivion Suite, Squish Coco, OpenCppCoverage, LLVM). Both build_all runners
  report `skipped` and stay green; any other non-zero code is a real failure.
  Every OPEN-SOURCE tool the pipeline needs must be installable by setup.sh /
  setup.ps1 — if you add a tool dependency, add it there too.
- No machine-specific absolute paths in committed scripts or Axivion configs.
  The Qt dir for the Frameworks-QtSupport rule comes from `$(AXIVION_QTDIR=)`,
  exported by both start_analysis scripts.
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

### Windows-specific (details: docs/windows.md)

- `.ps1` files MUST keep their UTF-8 BOM (`.gitattributes` enforces CRLF).
  PowerShell 5.1 reads a BOM-less file as ANSI; the em dashes then decode to
  U+201D, which it treats as a string delimiter, and nothing parses.
- PowerShell 5.1 strips embedded double quotes from native command lines —
  `python -c "…"` probes silently return nothing. Compute in PowerShell instead.
- `ValueFromRemainingArguments` needs an explicit `Position = 0`, or later
  parameters get bound positionally (`build_all.ps1 build test trace` →
  `Skip=test, QtKit=trace`).
- CMake strips backslashes from `CMAKE_EXE_LINKER_FLAGS`; put compiler-rt
  directories on `$env:LIB` and pass only the bare library name.
- `Get-LlvmToolset` pins clang-cl/llvm-cov/llvm-profdata to ONE LLVM install —
  VS-bundled and standalone LLVM both exist and mixing them breaks profdata.
- Squish Coco: its front end parses only up to C++20, so that build tree alone
  uses `-DCMAKE_CXX_STANDARD=20` (CMakeLists only defaults the standard when it
  is not already defined). `CMAKE_AR=cslib` and `CMAKE_LINKER=cslink` are both
  required (static libs), and Qt/STL/SDK headers must be excluded from
  instrumentation or `cmreport` crashes on the merged database.
- No clazy, no TSan, no valgrind on Windows — the scripts say so out loud
  rather than skipping quietly.
- MSVC ASan needs `/fsanitize=address` on the LINK line too, or the exe never
  exports operator new/delete and Windows refuses to start it (`entry point
  ??3@YAXPEAX_K@Z not located`). It only shows up outside a developer prompt.
- CMP0156: silence it with `cmake_policy(SET CMP0156 OLD)` in CMakeLists —
  `-DQT_FORCE_CMP0156_TO_VALUE=OLD` is a no-op, only `NEW` silences the check
  and `NEW` changes linking.
- MinGW: select the toolchain by the NUMBER in `mingw*_64`, not the name
  (`mingw810_64` string-sorts above `mingw1310_64`); its `bin` must be on PATH
  or cc1plus fails with exit 1 and no message.
- One working tree reached as `C:\…` and `/mnt/c/…` cannot share `build/`.
  `Reset-StaleCMakeCache` / `reset_stale_cache` discard a tree whose cached
  source dir, generator, Qt kit or compiler no longer matches.
- Axivion: the stage falls back to the newest Qt < 6.10 on its own; Suite
  7.12.3's front end asserts on Qt >= 6.10 `qvariant.h`.
