# Verification & validation

@page verification Verification: tests, coverage, sanitizers, static analysis
@tableofcontents

## Test suite

`tests/` holds 59 requirement-tagged Qt Test cases (see @ref test_spec):
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

    tools/publish_release.sh --dry-run  # what a release would attach, and whether the
                                        # evidence supports publishing at all
    tools/coverage.sh coco-components   # Coco again, but only the COMPONENT tests,
                                        # reported per test case: which functions of
                                        # each integrated component (domain +
                                        # services, as the app links them) those
                                        # tests actually reach. Coco calls this
                                        # function coverage; per-test attribution
                                        # comes from cmcsexeimport -t
    tools/coverage.sh        # auto: Squish Coco when installed AND licensed,
                             # otherwise both free modes below
    tools/coverage.sh gcov   # GCC --coverage → lcov/genhtml, line + branch
    tools/coverage.sh mcdc   # clang >= 18 -fcoverage-mcdc → llvm-cov, incl. MC/DC
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

The clang VERSION is resolved on the host, not hardcoded (`llvm_suffix` in
`tools/common.sh`): `-fcoverage-mcdc` needs ≥ 18, and clang++, llvm-profdata and
llvm-cov must come from one installation or the merged profile is rejected. A
machine whose distribution ships an older clang — a Raspberry Pi OS Bookworm, say
— gets gcov line/branch coverage and a `skipped` MC/DC step with the reason
named, instead of a failed stage (@ref platforms).

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
    tools/sanitize.sh tsan         # clang >= 18 ThreadSanitizer (data races)
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
- cppcheck + clang-tidy + the Clang Static Analyzer + g++ -fanalyzer + Axivion
  static analysis (unsound but effective bug-finders),
- MISRA C++ 2023 conformance monitoring via Axivion.

Adopting one of the sound tools is the documented path to an actual proof.

## Static analysis

    tools/static_analysis.sh   # cppcheck + clang-tidy + Clang Static Analyzer +
                               # g++ -fanalyzer + lizard + PMD CPD (+ clazy,
                               # codespell when installed)

Everything below runs over the **app and the test sources**. The gate is zero
findings; the one exception is the metrics ratchet, which is spelled out below.

- **the compiler** — the first analyzer: `-Wall -Wextra` plus the Qt-relevant
  extras (`-Woverloaded-virtual`, `-Wsuggest-override`, `-Wnon-virtual-dtor`,
  `-Wshadow` …), `/W4 /permissive-` on MSVC, all set in `CMakeLists.txt`.
  `build_all` configures with `-DTRADINGAPP_WARNINGS_AS_ERRORS=ON`, so a warning
  fails the build. `-Wpedantic` is deliberately not in the set: it reports the
  `Q_OBJECT;` semicolon — the traceability tooling's parse anchor — as an extra
  `;` (37 hits).
- **Axivion Suite** — MISRA C++ 2023 style checks, architecture verification;
  configuration under `axivion/`, results on the Axivion dashboard.
- **clang-tidy** — checks configured in `.clang-tidy`: the analyzer plus the
  bugprone/cert/concurrency/performance/portability/modernize/cppcoreguidelines/
  readability/misc families and the google-*/hicpp-* checks that are not aliases
  of those, with `WarningsAsErrors: '*'`. Every disabled check carries a written
  reason and the hit count that justifies it; `tests/.clang-tidy` inherits the
  lot and switches off only `readability-convert-member-functions-to-static`,
  which no Qt Test slot can satisfy (moc needs non-static members).
- **Clang Static Analyzer** — the same engine standalone
  (`tools/clang_analyzer.py`, provider `clang-analyzer`), because clang-tidy
  cannot pass `-analyzer-config`: off-by-default checkers
  (`optin.cplusplus.UninitializedObject`, `optin.cplusplus.VirtualCall`,
  `security.*`, `nullability.Nullable*`, `valist.*`) and a deeper search
  (400k exploration nodes, loop widening and unrolling). A TU the analyzer
  cannot finish is reported as a finding, never as silence.
- **cppcheck** — `--enable=all --check-level=exhaustive --inconclusive`, with
  `--checkers-report` as the record of which checkers ran; the four remaining
  suppressions are id-scoped and documented in
  `tools/cppcheck-suppressions.txt`.
- **clazy** — Qt-specific coding rules (levels 0–1: connect syntax, container
  detach, QString misuse …). Runs when installed (`apt install clazy`); it is
  a clang compiler plugin, so it consumes the same compile database.
- **g++ -fanalyzer** — GCC's symbolic-execution analyzer over every project
  TU; C++ support is upstream-experimental, so known false-positive patterns
  are filtered (see @ref tools).
- **lizard (code metrics)** — cyclomatic complexity, function length and
  parameter count per function; the full measurement lands in
  `analysis-results/lizard-metrics.csv`. Limits are CCN 15, NLOC 100, 5
  parameters, and the gate is a **ratchet**, not a threshold: the functions that
  exceed a limit today are recorded with their numbers in
  `tools/lizard_baseline.json`, and the run fails when a function appears that is
  not in that file, when a recorded number gets worse, or when an entry no
  longer violates anything (delete it — that is how the ratchet tightens).
  Every violation still reaches the dashboard, so the debt stays visible.
  Regenerate the file deliberately with
  `tools/lizard_metrics.py . analysis-results --update-baseline`.
- **PMD CPD (copy-paste detection)** — token-based clone detection at ≥ 100
  tokens. It closes a real gap: the Axivion configuration here is MISRA-only, so
  clones (issue type CL) had no gate at all. The five clone pairs it found on
  introduction were removed by extracting shared helpers, not baselined.
- **codespell** — typos in comments and docs (config in `.codespellrc`).

The external tools export their findings to `analysis-results/` as
plain reports and as a merged CSV that `axivion/external_import.py` maps to
the Axivion dashboard's external-findings format (see `axivion/README.md`).
