# V-model coverage — everything as code

@page vmodel V-model coverage (everything as code)
@tableofcontents

Every leg of the V-model lives as a versioned, machine-checked artefact in
this repository — no Word documents, no manual gates. The left side
(specification) and the right side (verification) are joined by generated,
bidirectional traceability; a broken link fails the build.

| V-model phase | As-code artefact | Tool / format | Verification counterpart | Gate |
|---|---|---|---|---|
| Requirements (SWE.1) | `requirements/requirements.sdoc` — single source of truth, REQ-F/N ids | StrictDoc (`tools/make_requirements.sh`; `docs/requirements.md` generated) | Acceptance: every REQ traced to a test or flagged as open gap | `tools/trace_report.py` fails on hard gaps |
| Architecture & design (SWE.2/3) | `docs/architecture.md`, `docs/design.md` — DES ids with `satisfies` REQ links | Markdown + PlantUML (rendered by Doxygen) | Layering enforced by the linker (domain ← services ← ui); Axivion architecture checks | build + `axivion_ci` |
| Implementation (SWE.3) | `src/` — C++23, layered static libraries | CMake ≥ 4.2, Qt 6 | Static analysis: Axivion MISRA C++ 2023 + cppcheck, clang-tidy, clazy, g++ -fanalyzer — all on ONE dashboard | `tools/static_analysis.sh` (exit 1 on findings) |
| Unit tests (SWE.4) | `tests/tst_*.cpp` — tagged `@tstid`/`@design` + `@relation(REQ-…, scope=function)` | Qt Test, JUnit XML | Spec ↔ implementation ↔ result join | `tools/run_tests.sh` + trace matrix |
| Integration tests (SWE.5) | same suite: config layering, simulation, HTTP retry, eToro client vs in-process mock server | Qt Test (no network) | REQ-level traces | same |
| Verification evidence (SWE.6) | coverage (line/branch + MC/DC), sanitizers (ASan/UBSan, TSan, valgrind), benchmarks | `tools/coverage.sh`, `tools/sanitize.sh`, `tests/tst_benchmarks` | findings imported to the Axivion dashboard (providers per tool) | `./build_all.sh` stages |
| Traceability (SUP.10-style) | generated matrix `docs/traceability.html` + StrictDoc REQ↔source view | `tools/trace_report.py`, StrictDoc source markers | bidirectional REQ ↔ DES ↔ TS ↔ result ↔ code | hard gap ⇒ exit 1 |

Supporting everything-as-code pieces: the environment itself (`setup.sh`
provisions a naked Linux and updates all tools), the build pipeline
(`build_all.sh`, stages selectable/skippable), the analysis configuration
(`axivion/*.json` + the `external_import.py` Python layer feeding all
third-party findings into the dashboard), CI (`.github/workflows/ci.yml`)
and this documentation (Doxygen + StrictDoc, generated from the same
repository state).

Honesty notes: UI-level requirements are open coverage gaps (listed in the
matrix, not hidden); sanitizer/coverage runs are evidence on executed paths,
not proof (see @ref verification); the ~100k-finding MISRA backlog on the
dashboard is the known baseline of the strict full ruleset, tracked and
triaged there.
