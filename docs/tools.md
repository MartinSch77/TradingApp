# Tool inventory

@page tools Tools: origin, version, role
@tableofcontents

Every tool used for building, testing, measuring and analysing this project,
with provenance and the version in use on the reference machine (WSL2 Ubuntu
24.04, captured 2026-07-25). Versions are also echoed by the scripts that
invoke the tools.

The table below is the **Linux** inventory. The Windows pipeline uses the same
tools where they exist there, substitutes a documented equivalent where they do
not (MSVC `/analyze` for `g++ -fanalyzer`, OpenCppCoverage for gcov/lcov), and
names the three that have no Windows counterpart at all (clazy, ThreadSanitizer,
valgrind). The Windows inventory and the reasoning behind each substitution are
in @ref windows.

| Tool | Origin / vendor | Version | Role here |
|------|-----------------|---------|-----------|
| Qt (Widgets, Charts, Network, Test) | Qt Group, qt.io | 6.11.1 at `~/Qt`, kit per host architecture: `gcc_64` on x86-64, `gcc_arm64` on ARM64 (aqt host `linux_arm64`) — resolved by `tools/common.sh`, see @ref platforms; Windows CI uses 6.10.3 — aqtinstall cannot fetch Windows 6.11.x metadata, see @ref windows | Application framework; Qt Test drives the suite |
| CMake | Kitware, cmake.org | 4.2.x (`cmake --version`) | Build system, CTest test runner |
| GCC | GNU Project | Ubuntu 24.04 default (g++ 13) | Reference compiler; `--coverage` instrumentation |
| Clang / LLVM | LLVM Project, llvm.org (Ubuntu pkg) | 18.1.3 | MC/DC coverage (`-fcoverage-mcdc`), clang-tidy host |
| llvm-cov / llvm-profdata | LLVM Project | 18.1.3 (`llvm-cov-18`) | Coverage reporting incl. MC/DC column |
| lcov / genhtml | Linux Test Project, github.com/linux-test-project/lcov | 2.0-1 | Line/branch HTML report from gcov data |
| gcovr | gcovr project, gcovr.com | 8.6 | Alternative gcov reporting (CI-friendly XML) |
| clang-tidy | LLVM Project | 18.1.3 | C++ linting over app AND test sources: analyzer + bugprone/cert/concurrency/performance/portability/modernize/cppcoreguidelines/readability/misc families plus selected google-*/hicpp-* checks; `WarningsAsErrors: '*'`. Every exclusion in `.clang-tidy` carries a written reason and a measured hit count |
| Clang Static Analyzer | LLVM Project | 18.1.3 (`clang++-18 --analyze`) | Path-sensitive analysis as a stage of its own (`tools/clang_analyzer.py`, provider `clang-analyzer`): the off-by-default checkers (optin.cplusplus.\*, security.\*, nullability.Nullable\*, valist.\*) and a deeper search than clang-tidy can be configured for (400k nodes, loop widening/unrolling). `alpha.*` stays off — measured 112 findings, 99 of them the same Qt-container false positive. Z3 path refutation is probed, not assumed (Ubuntu's LLVM 18 is built without it) |
| cppcheck | Cppcheck team, cppcheck.sourceforge.io | 2.13.0 | Static analysis at `--enable=all --check-level=exhaustive --inconclusive` over app and test sources; `--checkers-report` records which of its ~590 checkers ran. Suppressions (4, all id-scoped) are documented in `tools/cppcheck-suppressions.txt` |
| g++ -fanalyzer | GNU/GCC | 13.3.0 | Symbolic-execution analyzer over every project TU (`tools/static_analysis.sh`); C++ support is upstream-experimental — findings triaged on the dashboard (provider `gcc-analyzer`). Memory scales hard with TU size (unbounded: ~13 GB peak on MainWindow.cpp — full per-core parallelism OOM-killed the 16 GB CI runner, step exit 143), so TUs > 50 KB run sequentially after the parallel pool with the exploded graph bounded (call summaries + depth params, ~4.4 GB peak, identical finding set); analyzer crashes surface as `gcc-analyzer-failed` findings instead of vanishing |
| clazy | KDE / clazy project, invent.kde.org/sdk/clazy | 1.11 (Ubuntu package; install: `apt install clazy`) | Qt-specific coding rules (connect syntax, detach, QString…) |
| Axivion Suite | Axivion GmbH / Qt Group, axivion.com | 7.12.3 (`~/bauhaus-suite`) | MISRA C++ 2023, architecture checks, dashboard; external-findings import |
| Squish Coco | Qt Group (froglogic), qt.io/product/quality-assurance/coco | installed and **licensed** at `/opt/SquishCoco`; auto-detected by `tools/coverage.sh` | MC/DC coverage for the unit/integration suites (`coco`), the Squish GUI suite's own coverage reported SEPARATELY (`coco-gui`), per-test call coverage of the integrated components (`coco-components`), CocoAI test-case suggestions (`coco-ai`) |
| lizard | terryyin/lizard (MIT) | 1.23.0 (pipx) | Code metrics per function — cyclomatic complexity, NLOC, parameter count — over `src/` and `tests/` (`tools/lizard_metrics.py`, provider `lizard`). Full measurement in `analysis-results/lizard-metrics.csv`; the gate is a **ratchet** against `tools/lizard_baseline.json` (limits CCN 15 / NLOC 100 / params 5): new or worsened debt fails, and an entry that no longer violates must be deleted |
| PMD CPD | PMD project (BSD-style) | 7.19.0 (`tools/third-party/pmd-bin-7.19.0/`, fetched by `tools/fetch_pmd.sh`) | Token-based copy-paste detection over `src/` and `tests/` at ≥ 100 tokens (`tools/cpd_scan.py`, provider `pmd-cpd`). This is the project's only clone gate — the Axivion configuration here runs MISRA C++ 2023 only, without clone detection |
| GCC / clang warning set | GNU / LLVM | with the compiler | `-Wall -Wextra` plus `-Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wsuggest-override -Wimplicit-fallthrough -Wmisleading-indentation -Wformat=2 -Wdouble-promotion -Wcast-qual` (GCC-only spellings gated), `/W4 /permissive-` on MSVC — set in `CMakeLists.txt`. `-DTRADINGAPP_WARNINGS_AS_ERRORS=ON` (what `build_all` uses) makes them fatal. `-Wpedantic` is deliberately absent: it reports the required `Q_OBJECT;` anchor as an extra `;` |
| qmllint | Qt (GPL-3.0/commercial) | ships with the Qt kit | QML static analysis over `src/quick/qml` — unqualified access in a delegate, properties that do not exist on the target type. Given the generated `qmldir` with `-i`, so `TradingApp.Cockpit` types and the `Theme` singleton resolve (provider `qmllint` on the dashboard) |
| codespell | codespell-project (GPL-2.0) | pipx | Typos in comments/docs (`.codespellrc`; provider `codespell` on the dashboard) |
| SonarQube / SonarCloud | Sonar (LGPL server / free cloud tier) | conditional | `tools/sonar_scan.sh` runs only against a reachable server; issues → dashboard provider `sonarqube`; CI via SONAR_TOKEN |

### Why the architecture check is Axivion's job, not SonarCloud's

SonarQube Cloud has an architecture feature ("architecture as code": an
`architecture.json`/`.yaml` declaring perspectives, groups and constraints, verified during
analysis). It cannot be used here, for two independent reasons, both checked against
Sonar's own documentation (2026-08-06):

1. **C++ is not supported.** Architecture analysis covers C#, Java, JavaScript, Python and
   TypeScript. Architecture-as-code specifically is Java-only.
2. **It is deprecated.** The docs mark cycle detection and architecture as code as
   deprecated, pending removal in January 2026 — a date already past.

Writing an `architecture.json` for this repository would therefore produce a file no
analyzer reads, which is worse than no file: it looks like a verified constraint and is
not one.

The intended architecture is instead declared where it can actually be checked for C++,
and it is checked on every run:

| Layer of defence | Where | What it catches |
|---|---|---|
| the linker | `CMakeLists.txt` — domain links Qt Core only, services may not link ui | an illegal dependency fails the BUILD, before any analyzer runs |
| Axivion architecture check | `axivion/architecture.py` (`Architecture-ScriptedArchitecture`) | divergences (a dependency the model forbids) **and** absences (a declared edge the code no longer has), as AV findings on the dashboard |

`axivion/architecture.py` is the intended architecture as code: four components (Domain,
Services, UI, Main), the strictly downward edges between them, and a transitive directory
mapping so adding a class never requires touching the model. An edge is declared only
where the dependency is both intended *and* present — a declared-but-unused edge is
reported as an Absence, which is why "Main may use Domain directly" is deliberately not
modelled while `main.cpp` composes services and ui only.
| Coverity Scan | Black Duck (formerly Synopsys), scan.coverity.com | cloud only — project [`TradingApp`](https://scan.coverity.com/projects/TradingApp/builds) (the Scan APIs match the name case-sensitively) | Fifth static analyser, free tier for public repositories, used **only** as the cloud service (no local `cov-analyze`, no Coverity Connect): `.github/workflows/coverity.yml` builds under `cov-build` and uploads, analysis runs server-side. Defects exported from the web UI → `tools/coverity_findings.py` → dashboard provider `coverity`. Self-activating on COVERITY_SCAN_TOKEN + COVERITY_SCAN_EMAIL. Runs on the weekly cron or on manual dispatch only — no push/PR trigger: the free tier caps weekly submissions and queues accepted builds behind everyone else's (188 deep when measured), so a build per push spends quota to displace itself |
| CodeQL | GitHub | CI | Security scanning, free for public repositories (.github/workflows/codeql.yml) |
| Syft / Grype / Trivy | Anchore / Aqua (Apache-2.0) | ~/.local/bin via setup.sh | SBOM (SPDX+CycloneDX), vulnerability scan, repo/misconfig/secret scan (`tools/supply_chain.sh` + CI) |
| Sphinx + MyST | sphinx-doc.org (BSD) | pipx | Developer handbook over docs/*.md → docs/sphinx-html (tools/make_docs.sh) |
| Graphviz | graphviz.org (EPL) | apt | Doxygen dependency/class graphs (HAVE_DOT) |
| AddressSanitizer / UBSan | LLVM/GCC runtime | as shipped with GCC 13 / Clang 18 | Dynamic out-of-bounds / UB detection (`tools/sanitize.sh`) |
| Valgrind (memcheck) | valgrind.org | 3.22.0 | Independent dynamic memory checking |
| StrictDoc | strictdoc.readthedocs.io (Apache-2.0) | 0.27.0 (pipx) | Requirements-as-code: `requirements/requirements.sdoc` → HTML + requirement↔source traceability (`tools/make_requirements.sh`) |
| Doorstop | doorstop.readthedocs.io (LGPL) | 3.2 (pipx) | Evaluated for versioned requirements in git — not adopted (StrictDoc owns the requirement set; see docs/verification.md) |
| Doxygen | doxygen.nl | 1.9.8 | API + specification documentation, HTML output |
| Squish for Qt (optional) | Squish (Qt Group) | licence-bound; `./setup.sh squish` (or `.\setup.ps1 squish`) reports what this machine has and prints the remaining steps | The GUI suite in [`squish/`](../squish/). Every run is FORCED into simulation — `TRADINGAPP_FORCE_SIMULATION` makes `Config::hasCredentials()` answer false, so a GUI test cannot reach a real account whatever credentials exist (REQ-N-007, TS-CFG-007). `tools/squish_run.{sh,ps1}` writes JUnit XML next to the unit suite's and exits 3 when unlicensed |
| Squish Test Center (optional) | Squish Test Center (Qt Group) | licence-bound; `TESTCENTER_URL` + `TESTCENTER_TOKEN` | Central store for EVERY test result — the 21 Qt Test suites and the Squish GUI suite — uploaded by `tools/testcenter_upload.{sh,ps1}` and tagged with the short git sha so a run maps to a commit. `--dry-run` lists what it would send; exits 3 when not configured |
| cppcheck MISRA addon (informational) | cppcheck's own `addons/misra.py` (GPL, ships with cppcheck) | nothing — it is already installed with cppcheck; the MISRA rule TEXTS are copyrighted and must come from your own copy of the standard (`tools/misra_cppcheck.sh build /path/rules.txt`) | Free, and genuinely MISRA — but MISRA **C** 2012, while this codebase is C++23. Measured over the whole project (cppcheck 2.13): 527 findings, of which **404** are `misra-config` (the addon cannot parse Q_OBJECT / Q_DECLARE_METATYPE at all) and **110** are rule 12.3 flagging C++ TEMPLATE ARGUMENT LISTS as the comma operator — 514 of 527 are the language mismatch, not the code. It is therefore run on demand by [`tools/misra_cppcheck.sh`](../tools/misra_cppcheck.sh) and never gates a build. MISRA **C++ 2023** for this project is enforced by Axivion, whose configuration here is MISRA-only. The other three free addons were measured too and are also informational: `misc-implicitlyVirtual` (16) wants `virtual` repeated on a function that already says `override` — the opposite of modern C++; `threadsafety-unsafe-call` (19) flags `getenv` inside `Config::load`, which runs once at start-up before any thread exists; `findcasts-cast` (7) is an inventory of casts at "information" severity, not a defect list. It did find one thing worth fixing: a local named `exit`, which every tool reads as a call to `::exit` — now `exitRate` |
| `tools/train_bot_net.py` (optional) | stdlib Python 3 | none — no numpy, no torch; ships with the repo | The DESKTOP counterpart of the bot's own trainer (@ref requirements REQ-F-033). The app trains its outcome model itself, in C++, because the machines it runs on unattended may have no Python; this script exists for experimenting on a workstation (different hidden sizes, epochs, a copied log) and writes the byte-compatible model file. TS-NET-004 runs it and reads its output back, so the two halves cannot drift apart |
| Ollama (optional) | Ollama (MIT) | `v0.32.5` runtime + one model, installed by `./setup.sh ollama` into `~/.local/ollama` (~1.4 GB + the model); nothing else in the pipeline needs it | Serves the LOCAL large language model the bot simulation can take its trading proposal from (@ref requirements REQ-F-030, `src/services/OllamaAdvisor.*`). No key, no cloud: the daemon runs as a user process on `localhost:11434`. The tests mock its HTTP API, so the suite never needs it installed |
| linuxdeploy + linuxdeploy-plugin-qt | linuxdeploy project (MIT) | pinned dated release + SHA256 **per architecture** (x86_64, aarch64) in `tools/fetch_linuxdeploy.sh`, unpacked into `tools/third-party/` | Bundle the Qt runtime into the downloadable Linux **AppImage** (`tools/package_appimage.sh`) — the host's architecture decides which pair is fetched, so a Raspberry Pi packages an `aarch64` AppImage with the same script. The Windows counterpart is windeployqt, which ships with Qt and is driven by the CMake install rules (`tools/package_portable.ps1`) |
| PlantUML | plantuml.com (GPL) | 1.2026.0 (`tools/third-party/plantuml.jar`, downloaded from the official GitHub release; re-fetch with `tools/fetch_plantuml.sh`) | Architecture/sequence diagrams inside Doxygen |
| Graphviz (dot) | graphviz.org | Ubuntu 24.04 package | Doxygen graphs, PlantUML layout backend |
| OpenJDK | openjdk.org (Ubuntu package) | 21 | Runs the PlantUML jar |
| Python | python.org (Ubuntu package) | 3.12 | `tools/trace_report.py`, `tools/sdoc_to_md.py`, `tools/parse_sanitizer_log.py`, `tools/merge_findings.py`, `tools/msvc_analyze.py`, `tools/coverity_findings.py`, `tools/clang_analyzer.py`, `tools/lizard_metrics.py`, `tools/cpd_scan.py`, `packaging/make_icon.py`, `axivion/external_import.py` |

## Coverity Scan: where its evidence lives

Coverity analyses SERVER-SIDE, so its defect list is on scan.coverity.com rather than
in this repository. The one artefact the run itself produces is `cov-int/build-log.txt`
— what `cov-build` actually captured — and it answers the question a failed or empty
submission raises: did it see the compiler at all, and how many translation units did
it emit? The workflow uploads it on every run (artifact `coverity-build-log`, kept 90
days) and

```bash
tools/fetch_coverity_log.sh          # newest run
tools/fetch_coverity_log.sh --list   # pick another
```

places it in `analysis-results/coverity-build-log.txt`, next to the other analyzers'
output — which also puts it in the qualification bundle rather than behind a download
button in a web UI. Without `gh`, without authentication or without a run to fetch
from, it prints why and exits 3 (skipped), like every other optional stage.

Two things measured in that log on 2026-08-06, both worth knowing before anyone
re-diagnoses them:

* **91 compilation units** were captured. `cov-build` exits 0 even when it captures
  NOTHING, so the workflow asserts on this count rather than on the exit code — a
  silent empty submission still spends a weekly quota slot.
* **69 "recoverable errors" warnings**, two per TU, are all one cause: Coverity's
  front end fails on `std::__format::__float128_t` inside libstdc++'s `<format>` — a
  SYSTEM header, not this project's code. "Recoverable" is literal: each TU still
  emits, so the submission is complete. There is nothing to fix in `src/`.

## What must be installed — and what happens when it is not

Rather than a prose list that goes stale, the repository answers this by running:

```bash
tools/check_prerequisites.sh              # everything, grouped by pipeline stage
tools/check_prerequisites.sh --release    # only what publishing a release needs
tools\check_prerequisites.ps1 -Release    # the Windows counterpart
```

Each entry names the stage it belongs to, so a missing tool translates directly into
"this stage will report skipped" instead of a surprise halfway through a release. The
exit code is 1 only when something **required** is missing; licence-bound tools never
fail it.

The division is deliberate and is the rule the whole pipeline follows:

| Kind | Examples | If absent |
|------|----------|-----------|
| **Required** | CMake, a C++23 compiler, Qt 6, Python 3, cppcheck, clang-tidy, git, `gh`, reportlab | the build or the release genuinely cannot proceed — `setup.sh` / `setup.ps1` install every one of them |
| **Open-source, optional** | clazy, valgrind, lcov/gcovr, llvm-cov, PMD (Java), codespell, lizard, Ollama, linuxdeploy, Android SDK/NDK | the stage says so and **skips** (exit code 3); the pipeline stays green and the quality PDF records that this evidence was not measured here |
| **Licence-bound** | Squish, Squish Coco, Axivion Suite, Qt Test Center | same skip, and additionally listed as a **MISSING LICENCE** in the quality PDF — so a reader can tell "measured and clean" from "not measured on this machine" |

Two consequences worth stating plainly, because both have surprised people:

* **A release does not need the licensed tools.** It needs the tests green, the seven
  analyzers at zero, the metrics ratchet clean, zero hard traceability gaps and a PDF
  newer than the sources. `tools/publish_release.sh` checks exactly that and refuses
  otherwise — a missing Coco licence is not one of the reasons it can refuse.
* **No single machine produces all four platforms.** The Windows ZIP, the ARM64
  AppImage and the signed Android APK are built by `.github/workflows/release.yml` on
  a `v*` tag, one runner each. The publisher attaches what exists and names what is
  missing rather than quietly shipping three platforms as four.

For the optional runtime feature the app itself can use — the local model the trading
bot takes its picks from — `./setup.sh ollama` installs the runtime and the model
under `~/.local/ollama`; on Windows it is `winget install Ollama.Ollama` followed by
`ollama pull qwen2.5:1.5b`. Without it the bot simply reports the model as not
configured and keeps trading its own composite.

## Windows-only tools

Versions captured on the Windows reference machine (Windows 11, 2026-07-27).
Provisioned by `.\setup.ps1`; see @ref windows for how each one is wired in.

| Tool | Origin / vendor | Version | Role here |
|------|-----------------|---------|-----------|
| MSVC (`cl`) | Microsoft, Visual Studio 2022 | 19.44.35228 | Default Windows compiler; builds the app and all 12 test executables warning-free |
| MSVC `/analyze` | Microsoft | with 19.44 | Compiler-native symbolic-execution analyzer, the Windows counterpart of `g++ -fanalyzer`; normalized by `tools/msvc_analyze.py` (dashboard provider `msvc-analyze`) |
| MSVC AddressSanitizer | Microsoft | with 19.44 | `/fsanitize=address`. Note: **no LeakSanitizer** on Windows (dashboard provider `asan`) |
| clang-cl / llvm-cov / llvm-profdata | LLVM Project (winget `LLVM.LLVM`) | 22.1.8 | MC/DC coverage (`-fcoverage-mcdc`) and the UBSan build (provider `ubsan`) |
| Squish Coco | Qt Group (froglogic) | `C:\Program Files\squishcoco`, **licensed** (Full Commercial) | Source-instrumented statement/decision/condition **and true MC/DC** coverage; wrappers `cscl`/`cslib`/`cslink`; parses up to C++20 |
| cppcheck | Cppcheck team (winget `Cppcheck.Cppcheck`) | 2.21.0 | Same role as on Linux; the newer version reports one finding the Linux 2.13 does not |
| lizard / PMD CPD / Clang Static Analyzer | see the Linux rows | same versions | The same three shared Python drivers run from `tools\static_analysis.ps1`; the analyzer picks the clang matching the compile database's dialect (clang-cl for an MSVC database) and reports "skipped" when no clang driver is installed |
| clang-tidy | LLVM Project | 19.1.5 / 22.1.8 | Same role; newer checks report ~27 additional findings vs clang-tidy 18 |
| OpenCppCoverage | OpenCppCoverage project (MIT) | optional | PDB-based **line** coverage for MSVC builds — the gcov/lcov substitute (no branch coverage) |
| Doxygen / Graphviz | doxygen.nl / graphviz.org (winget) | 1.17.0 / 15.1.0 | Same role as on Linux |
| VSDiagnostics / `wpr`+`wpa` | Microsoft (Visual Studio / Windows Performance Toolkit) | as installed | CPU profiling; records a trace for a GUI analyzer instead of `perf report --stdio` |
| Axivion Suite | Axivion GmbH / Qt Group | 7.12.x (`C:\Program Files\Bauhaus`) | Same role; uses the built-in `Project/MicrosoftToolchain` profile via `axivion/compiler_config_msvc.json` |
| winget | Microsoft | shipped with Windows 11 | Package manager `.\setup.ps1` provisions through |

## Axivion MCP servers (Claude Code)

`.mcp.json` wires the two MCP servers that ship with the Axivion Suite —
`axdocumentation` (rule documentation) and `axdashboard` (findings, versions,
dashboard queries; what the `/axivion-dashboard` and `/ax-fixcode` skills drive).
Both are license-bound, so they are configured but not installable by setup.

The JSON must stay free of machine-specific absolute paths, and Claude Code's
`${VAR}` interpolation cannot branch on the platform, so the platform difference
lives in one script pair instead:

| | Linux | Windows |
|---|---|---|
| resolver | `tools/mcp_env.sh --persist` | `.\tools\mcp_env.ps1 -Persist` |
| written to | a guarded block in `~/.profile` | the **User** environment scope |
| Suite root | `~/bauhaus-suite` (same search as `axivion/start_analysis.sh`) | newest install found by `Get-AxivionSuite` |
| MCP venv | `mcps/axivion-mcps/.venv/bin/python` | `mcps\axivion-mcps\.venv\Scripts\python.exe` |

`setup.sh install` / `.\setup.ps1 install` run the resolver, and the `status`
report carries an `ax MCP` line. Exit 3 = no Suite installed, which is not an
error. The three variables it exports:

| Variable | Consumed by |
|---|---|
| `AXIVION_SUITE_DIR` | `bin/rfgscript` + the two server scripts; passed on as `BAUHAUS_INSTDIR` |
| `AXIVION_MCP_PYTHON` | the `axdocumentation` command, and `BAUHAUS_PYTHON` for `axdashboard` |
| `AXIVION_DATABASES_DIR` | `axdashboard` database mode |

Three things about this configuration are load-bearing:

- **`${VAR}`, never `$(VAR)`.** `$(VAR)` is Axivion's *own* config syntax
  (`axivion/ci_config.json` uses it, with defaults: `$(AXIVION_DASHBOARD_URL=…)`)
  and Claude Code does not recognise it — it passes the literal string through
  **without a warning**, and the server dies with a bare "cannot find the path
  specified" from the shell. `${VAR}` and `${VAR:-default}` are the supported
  forms; a missing `${VAR}` is reported as `Missing environment variables`.
- **`BAUHAUS_PYTHON` for `axdashboard`.** That server is started by
  `bin/rfgscript`, whose interpreter has the `axivion`/`bauhaus` modules but not
  the MCP venv's `mcp` package. `rfgscript` honours `BAUHAUS_PYTHON` and picks up
  the venv's site-packages, which is what makes both import sets available in one
  process. `axdocumentation` needs no such trick — it runs on the venv
  interpreter directly.
- **`MCP_TIMEOUT` in `.claude/settings.json`.** Cold start measured on the
  Windows reference machine: 53 s for `axdocumentation` (it parses the rule
  documentation) and 37 s for `axdashboard` (it processes the `axivion/` config
  layers and starts a local dashboard). Warm: 30 s / 9 s. Claude Code's default
  handshake timeout is 30 s, so both servers fail — with a *timeout*, which looks
  nothing like a configuration error. The project settings raise it to 180 s.

## Mutation testing pilot: does a passing suite actually notice a bug?

`tools/mutation_test.sh` runs [Mull](https://mull.readthedocs.io/) — an LLVM
IR-level mutation-testing tool — over a small, curated pilot set of domain test
programs (`tst_confirmgate`, `tst_positionmath`, `tst_money`, `tst_pathoutcome`
by default). It answers a question code coverage cannot: not "did the test run
this line" but "would the test have FAILED if this line were subtly wrong."

Install with `./setup.sh mull` (Linux/clang only — mirrors the clazy/TSan/
valgrind split already in this project; there is no Windows counterpart). It
unpacks Mull's own `.deb` release asset into `~/.local/mull`, matched to the
host's LLVM major version (`tools/common.sh`'s `llvm_suffix`) — no root needed,
since the runtime dependencies are already satisfied by the clang toolchain
`setup.sh` installs. Run the pilot with `tools/mutation_test.sh` (exits 3 if
Mull or clang >= 18 is missing); it configures a separate `build-mull/` tree
(clean_all.sh removes it), builds the pilot targets, and writes
`analysis-results/mutation-pilot.txt`.

**Scoping is load-bearing.** Every test binary here links the whole
`trading_domain` static library, so an unscoped run mutates code the test under
pilot never even calls — measured once on `tst_pathoutcome`: 435 mutants across
the entire library and a meaningless 4% score, almost none of them actually in
`PathOutcome.cpp`. The script writes a per-target `mull.yml` with an
`includePaths` regex scoped to the ONE source file each test is meant to pilot,
before every run, and removes it afterwards.

This is **deliberately informational, not a gate** — a pilot establishing the
capability and a first baseline, the same "measured, not yet enforced" stance
this project already takes with SonarCloud/Coverity, until a real threshold
policy exists. `src/domain/PathOutcome.cpp` reached 100% on this pilot
(`tests/tst_pathoutcome.cpp`'s `TS-PATH-007`..`011`, added specifically to kill
the mutants the first pilot run surfaced); `ConfirmGate.cpp`, `PositionMath.cpp`
and `Money.cpp` still carry a small number of documented survivors left for a
follow-up pass.

## Fuzzing: libFuzzer over hand-rolled text parsers

`tools/fuzz.sh [seconds-per-target]` builds the harnesses under `fuzz/` into a
separate `build-fuzz/` tree (`-DTRADINGAPP_BUILD_FUZZERS=ON` there and nowhere
else — the option is off by default, since `-fsanitize=fuzzer,address,undefined`
changes code generation project-wide if applied anywhere else) and runs each one
for a bounded time (default 30 s smoke run; pass a larger budget for a real
campaign) against its own seed corpus. libFuzzer is a clang compiler-rt runtime,
not a separate install — any clang build already has it, so there is no
`./setup.sh` step, unlike Mull's prebuilt `.deb`. Linux/clang only, same split as
clazy/TSan/valgrind/Mull; exits 3 when no clang >= 18 is present.

**First target (tooling backlog item 3): `fuzz/tradescript_fuzzer.cpp`**, over
`trading::parseTradeScript` (`src/domain/TradeScript.cpp`, REQ-F-028) — the one
parser in this codebase that reads untrusted text straight off disk, hand-split
on `;` and coerced through `QDateTime::fromString`/`QString::toDouble`/`toInt`.
The harness also re-asserts (via `__builtin_trap()`, sanitizer-visible) the two
invariants the rest of the domain trusts a parsed entry to already satisfy —
`amount > 0`, `leverage >= 1` — so a parser bug that lets either slip through
turns into a crash HERE rather than a silent bad trade downstream.

**Seed corpus vs discovered corpus — do not conflate them.** `fuzz/corpus/
<target>/` is a handful of curated, TRACKED example inputs (the shipped
`docs/tradescript-example.txt`, a minimal line, an all-fields line). Passing
that directory to libFuzzer as ITS corpus would be wrong: libFuzzer writes every
new minimized/interesting input it discovers directly into whatever directory it
is given — measured once, a single 30 s run turned 3 tracked seed files into 53,
all untracked noise. `tools/fuzz.sh` therefore passes an ignored output
directory (`analysis-results/fuzz-corpus/<target>/`, or `$FUZZ_CORPUS_DIR`) as
the PRIMARY corpus argument and the tracked seed directory as an extra read-only
input — the seeds stay exactly as committed after any number of runs.

Deliberately informational, not a gate, the same "measured, not yet enforced"
stance as Mull and SonarCloud: a found crash lands under `fuzz/crashes/<target>/
` (git-ignored) and is reproduced by running the harness binary directly on that
one file, but nothing here fails a build yet.

## Sound runtime-error provers (documented, not installed)

| Tool | Origin | Note |
|------|--------|------|
| Astrée | AbsInt GmbH, absint.com | Sound abstract interpretation; C and a C++ subset |
| Polyspace Code Prover | MathWorks | Sound proof of absence of runtime errors |
| TrustInSoft Analyzer | TrustInSoft | Sound C/C++ analysis |

See @ref verification for why these are the honest route to a *proof* of
absence of out-of-bounds/runtime errors, and what evidence this project
provides in the meantime.

## SonarCloud is informational, not a gate (moved from the README)

The badges above show SonarCloud's own measures, not its quality gate. That is
deliberate: Sonar's default gate fails on *hotspot* categories that need a human
"safe" verdict on their dashboard — a pseudorandom generator used for reproducible
model training, plain HTTP to a model server on `localhost`, unpinned action
versions — and none of those can be answered by a build. This project's gates are
the ones in `build_all.sh`: the test suite, requirements traceability, the metrics
ratchet, eight analyzers at zero findings, clone detection, the sanitizers and
Axivion's MISRA C++ 2023. SonarCloud runs alongside them as a second opinion, and
its findings are read rather than obeyed.

Two notes on the badges themselves. There is no "issues" badge any more: Sonar
retired `metric=violations`, and the badge endpoint answers a JSON error for it
rather than an image — which is why that badge rendered as a broken-image icon
until it was replaced by the two measures above. And what the dashboard currently
counts is worth stating plainly rather than hiding behind a green picture: **0
bugs**, 891 code smells and 113 findings Sonar files as vulnerabilities, of which
the large majority (74) are GitHub Actions hardening rules about workflow
permissions and unpinned action versions, 24 are taint warnings in the Python
tooling and 3 are the reproducible-RNG rule in the simulation feed. None is a
finding about the trading path; they are on the backlog as hygiene, not as
blockers.
