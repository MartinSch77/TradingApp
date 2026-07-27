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

## Requirements-as-code (StrictDoc)

The requirements live in `requirements/requirements.sdoc` (StrictDoc, single
source of truth, versioned in git); `tools/make_requirements.sh` validates
and exports them to `docs/strictdoc/html/` — including the requirement ↔
source-code traceability built from the `@relation(REQ-…, scope=function)`
markers the test functions carry — and regenerates the `docs/requirements.md`
Doxygen page from the same file. **Doorstop** was evaluated hands-on for the
same role (versioned requirement items + links in git; the pilot with
REQ/TST documents, linking and validation worked); it was *not* adopted,
because it would duplicate the requirement set StrictDoc already owns —
one source of truth beats two. Its distinctive feature (fingerprint-based
suspect-link review on item changes) is the one reason to reconsider.

## Structural coverage — line, branch, MC/DC

    tools/coverage.sh        # auto: Squish Coco when installed AND licensed,
                             # otherwise both free modes below
    tools/coverage.sh gcov   # GCC --coverage → lcov/genhtml, line + branch
    tools/coverage.sh mcdc   # clang-18 -fcoverage-mcdc → llvm-cov, incl. MC/DC
    tools/coverage.sh coco   # Squish Coco csg++ build, incl. native MC/DC

Both reports land under `coverage/`. MC/DC (modified condition/decision
coverage) is measured with Clang 18's source-based coverage — each complex
decision's conditions must independently affect the outcome to count.
Baseline (2026-07-25): domain layer ≈ 80–97% line coverage, MC/DC 0–50% per
file — the MC/DC deficit pinpoints which condition combinations still need
targeted tests; UI files are not yet under automated test (tracked as open
gaps in the traceability report).

**Squish Coco** (Qt Group's coverage tool) measures MC/DC natively and its
CocoAI feature can propose test inputs that close specific uncovered
conditions. `tools/coverage.sh` auto-detects it at `/opt/SquishCoco` and uses
it as the coverage tool whenever the license is valid (`cocolic --check`); with
the Linux license currently expired that script falls back to the clang MC/DC
path above as the measuring tool of record. Note that clang-18 cannot
instrument decisions with more than 6 conditions for MC/DC, so the sources keep
every decision at ≤ 6 conditions (keyword groups are tested via
`hasAny()`/`std::any_of`).

### MC/DC on Windows — measured twice

The Windows machine has a **valid Coco licence**, so `tools\coverage.ps1` runs
MC/DC through *both* engines and reports them side by side (see @ref windows):

    tools\coverage.ps1              # auto: Coco + clang-cl MC/DC (+ OpenCppCoverage)
    tools\coverage.ps1 -Mode coco   # Squish Coco cscl/cslib/cslink, native MC/DC
    tools\coverage.ps1 -Mode mcdc   # clang-cl -fcoverage-mcdc → llvm-cov
    tools\coverage.ps1 -Mode msvc   # OpenCppCoverage, LINE coverage only

Two independent instrumentation techniques measuring the same criterion is
stronger evidence than either alone, and the numbers agree on the shape of the
deficit. Baseline (2026-07-27, domain + services scope):

| engine | MC/DC |
|---|---|
| Squish Coco (source instrumentation) | 637 / 1845 conditions ≈ 34.5% |
| clang-cl + llvm-cov (IR instrumentation) | 36 / 209 conditions ≈ 17.2% |

The two denominators differ because the tools count MC/DC conditions
differently (Coco counts every instrumented condition in the scope it
instruments; llvm-cov counts only decisions it could build a full MC/DC table
for). They are therefore *not* directly comparable as percentages — each is a
baseline against itself, and both point at the same files. Coco's report also
drills down per decision in `coveragebrowser`, which is what makes it the tool
of record where it is licensed.

## Performance (REQ-N-006)

    ./build_all.sh release   # optimized RelWithDebInfo build (frame pointers kept)
    tools/profile.sh         # perf / gperftools hotspots over the release binary
    build*/tests/tst_benchmarks   # deterministic QBENCHMARK numbers

Compute-heavy work (Monte-Carlo, trade-plan building) runs off the GUI thread
(QtConcurrent); the open-trades table refreshes allocation-free through
`PositionsModel` (in-place `dataChanged`). Measured 2026-07-26 (Debug →
release): monteCarlo 0.29 → 0.086 ms, buildTradePlan 1.59 → 0.73 ms,
computeDecisionRows over 25 instruments 4.5 → 0.22 ms per iteration.

## Dynamic runtime-error evidence (sanitizers)

    tools/sanitize.sh              # all three checkers in sequence
    tools/sanitize.sh asan-ubsan   # GCC ASan (incl. LeakSanitizer) + UBSan
    tools/sanitize.sh tsan         # clang-18 ThreadSanitizer (data races)
    tools/sanitize.sh valgrind     # memcheck: --leak-check=full
                                   # --show-leak-kinds=all --track-origins=yes
                                   # --error-exitcode=1

A clean ASan+UBSan run demonstrates absence of out-of-bounds access,
use-after-free, leaks and undefined behaviour **on the executed paths**; the
TSan run adds data races and lock-order inversions. Every mode normalizes its
findings into `analysis-results/sanitize-<mode>.txt`
(`tools/parse_sanitizer_log.py`), which the next `axivion_ci` run imports
onto the dashboard (providers `asan-ubsan`, `tsan`, `valgrind` — see
`axivion/external_import.py`); the raw logs sit next to them as
`sanitize-<mode>.raw.txt`. Valgrind "still reachable" blocks stay in the raw
log only — they are Qt/OpenSSL runtime allocations, not actionable findings.

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
