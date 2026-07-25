# Verification & validation

@page verification Verification: tests, coverage, sanitizers, static analysis
@tableofcontents

## Test suite

`tests/` holds 48 requirement-tagged Qt Test cases (see @ref test_spec):
unit tests over the pure domain layer and integration tests over the services
(config layering, simulation engine, HTTP retry policy, eToro history pager
against an in-process mock HTTP server — no network access in tests).

    cmake -S . -B build -DCMAKE_PREFIX_PATH=~/Qt/6.10.2/gcc_64
    cmake --build build && (cd build && ctest --output-on-failure)
    tools/run_tests.sh          # same suite, JUnit XML into test-results/

The JUnit results feed the traceability matrix
(<a href="traceability.html">traceability.html</a>, `tools/trace_report.py`),
which shows for every requirement the design element, the specified test, the
implementing test function and its latest verdict — and lists every gap.

## Structural coverage — line, branch, MC/DC

    tools/coverage.sh gcov   # GCC --coverage → lcov/genhtml, line + branch
    tools/coverage.sh mcdc   # clang-18 -fcoverage-mcdc → llvm-cov, incl. MC/DC

Both reports land under `coverage/`. MC/DC (modified condition/decision
coverage) is measured with Clang 18's source-based coverage — each complex
decision's conditions must independently affect the outcome to count.
Baseline (2026-07-25): domain layer ≈ 80–97% line coverage, MC/DC 0–50% per
file — the MC/DC deficit pinpoints which condition combinations still need
targeted tests; UI files are not yet under automated test (tracked as open
gaps in the traceability report).

**Squish Coco** (Qt Group's coverage tool, installed at `/opt/SquishCoco`)
measures MC/DC natively and its CocoAI feature can propose test inputs that
close specific uncovered conditions. Its license on this machine is expired
(`cocolic --check`); once renewed, build with the `coveragescanner` compiler
wrappers and import the `.csmes/.csexe` into the CoverageBrowser — until
then, the clang MC/DC path above is the measuring tool of record.

## Dynamic runtime-error evidence (sanitizers)

    tools/sanitize.sh asan-ubsan   # AddressSanitizer + UBSan over the suite
    tools/sanitize.sh valgrind     # independent memcheck pass

A clean ASan+UBSan run demonstrates absence of out-of-bounds access,
use-after-free and undefined behaviour **on the executed paths**.

### On *proving* the absence of runtime errors

The user-level goal "prove absence of out-of-bounds access and other runtime
errors" requires *sound* static analysis (abstract interpretation) covering
**all** paths. For full C++/Qt codebases the realistic options are commercial:
**AbsInt Astrée** (sound for a C/C++ subset), **MathWorks Polyspace Code
Prover**, **TrustInSoft Analyzer**. No free tool soundly proves runtime-error
absence for idiomatic Qt C++ (Frama-C EVA is C-only; CBMC does not scale to
Qt). What this project provides instead, and states as such:

- sanitizer + valgrind clean runs (dynamic evidence on tested paths),
- cppcheck + clang-tidy + Axivion static analysis (unsound but effective
  bug-finders),
- MISRA C++ 2023 conformance monitoring via Axivion.

Adopting one of the sound tools is the documented path to an actual proof.

## Static analysis

    tools/static_analysis.sh          # cppcheck + clang-tidy (+ clazy if installed)

- **Axivion Suite** — MISRA C++ 2023 style checks, architecture verification;
  configuration under `axivion/`, results on the Axivion dashboard.
- **clang-tidy** — checks configured in `.clang-tidy` (bugprone-*, cert-*,
  performance-*, readability subset); runs over the compile database.
- **cppcheck** — `--enable=warning,performance,portability`, suppressions in
  `tools/cppcheck-suppressions.txt`.
- **clazy** — Qt-specific coding rules (levels 0–1: connect syntax, container
  detach, QString misuse …). Runs when installed (`apt install clazy`); it is
  a clang compiler plugin, so it consumes the same compile database.

All three external tools export their findings to `analysis-results/` as
plain reports and as a merged CSV that `axivion/import_external.py` maps to
the Axivion dashboard's external-findings format (see `axivion/README.md`).
