# Tool inventory

@page tools Tools: origin, version, role
@tableofcontents

Every tool used for building, testing, measuring and analysing this project,
with provenance and the version in use on the reference machine (WSL2 Ubuntu
24.04, captured 2026-07-25). Versions are also echoed by the scripts that
invoke the tools.

| Tool | Origin / vendor | Version | Role here |
|------|-----------------|---------|-----------|
| Qt (Widgets, Charts, Network, Test) | Qt Group, qt.io | 6.10.2 (gcc_64 kit at `~/Qt`) | Application framework; Qt Test drives the suite |
| CMake | Kitware, cmake.org | 4.2.x (`cmake --version`) | Build system, CTest test runner |
| GCC | GNU Project | Ubuntu 24.04 default (g++ 13) | Reference compiler; `--coverage` instrumentation |
| Clang / LLVM | LLVM Project, llvm.org (Ubuntu pkg) | 18.1.3 | MC/DC coverage (`-fcoverage-mcdc`), clang-tidy host |
| llvm-cov / llvm-profdata | LLVM Project | 18.1.3 (`llvm-cov-18`) | Coverage reporting incl. MC/DC column |
| lcov / genhtml | Linux Test Project, github.com/linux-test-project/lcov | 2.0-1 | Line/branch HTML report from gcov data |
| gcovr | gcovr project, gcovr.com | 8.6 | Alternative gcov reporting (CI-friendly XML) |
| clang-tidy | LLVM Project | 18.1.3 | C++ linting: bugprone/cert/performance checks |
| cppcheck | Cppcheck team, cppcheck.sourceforge.io | 2.13.0 | Static analysis: warnings, portability |
| g++ -fanalyzer | GNU/GCC | 13.3.0 | Symbolic-execution analyzer over every project TU (`tools/static_analysis.sh`); C++ support is upstream-experimental — findings triaged on the dashboard (provider `gcc-analyzer`) |
| clazy | KDE / clazy project, invent.kde.org/sdk/clazy | 1.11 (Ubuntu package; install: `apt install clazy`) | Qt-specific coding rules (connect syntax, detach, QString…) |
| Axivion Suite | Axivion GmbH / Qt Group, axivion.com | 7.12.3 (`~/bauhaus-suite`) | MISRA C++ 2023, architecture checks, dashboard; external-findings import |
| Squish Coco | Qt Group (froglogic), qt.io/product/quality-assurance/coco |  at `/opt/SquishCoco` — **license expired**; auto-detected and used by `tools/coverage.sh` once renewed | MC/DC coverage + CocoAI test-case suggestion (documented alternative) |
| AddressSanitizer / UBSan | LLVM/GCC runtime | as shipped with GCC 13 / Clang 18 | Dynamic out-of-bounds / UB detection (`tools/sanitize.sh`) |
| Valgrind (memcheck) | valgrind.org | 3.22.0 | Independent dynamic memory checking |
| StrictDoc | strictdoc.readthedocs.io (Apache-2.0) | 0.27.0 (pipx) | Requirements-as-code: `requirements/requirements.sdoc` → HTML + requirement↔source traceability (`tools/make_requirements.sh`) |
| Doorstop | doorstop.readthedocs.io (LGPL) | 3.2 (pipx) | Evaluated for versioned requirements in git — not adopted (StrictDoc owns the requirement set; see docs/verification.md) |
| Doxygen | doxygen.nl | 1.9.8 | API + specification documentation, HTML output |
| PlantUML | plantuml.com (GPL) | 1.2025.4 (`tools/third-party/plantuml.jar`, downloaded from the official GitHub release; re-fetch with `tools/fetch_plantuml.sh`) | Architecture/sequence diagrams inside Doxygen |
| Graphviz (dot) | graphviz.org | Ubuntu 24.04 package | Doxygen graphs, PlantUML layout backend |
| OpenJDK | openjdk.org (Ubuntu package) | 21 | Runs the PlantUML jar |
| Python | python.org (Ubuntu package) | 3.12 | `tools/trace_report.py`, `axivion/import_external.py` |

## Sound runtime-error provers (documented, not installed)

| Tool | Origin | Note |
|------|--------|------|
| Astrée | AbsInt GmbH, absint.com | Sound abstract interpretation; C and a C++ subset |
| Polyspace Code Prover | MathWorks | Sound proof of absence of runtime errors |
| TrustInSoft Analyzer | TrustInSoft | Sound C/C++ analysis |

See @ref verification for why these are the honest route to a *proof* of
absence of out-of-bounds/runtime errors, and what evidence this project
provides in the meantime.
