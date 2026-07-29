# Windows toolchain

@page windows Windows quality pipeline (PowerShell)

@tableofcontents

Linux is the reference platform and drives the `*.sh` entry points. Windows has
a one-to-one PowerShell counterpart for every one of them, so the whole quality
pipeline — build, tests, traceability, docs, coverage incl. MC/DC, static
analysis, sanitizers, Axivion — runs natively without WSL.

| purpose | Linux | Windows |
|---|---|---|
| provision the toolchain | `./setup.sh` | `.\setup.ps1` |
| build everything | `./build_all.sh` | `.\build_all.ps1` |
| remove everything generated | `./clean_all.sh` | `.\clean_all.ps1` |
| test suite + JUnit | `tools/run_tests.sh` | `tools\run_tests.ps1` |
| traceability matrix | `python3 tools/trace_report.py` | same script |
| requirements export | `tools/make_requirements.sh` | `tools\make_requirements.ps1` |
| docs | `tools/make_docs.sh` | `tools\make_docs.ps1` |
| coverage | `tools/coverage.sh` | `tools\coverage.ps1` |
| static analysis | `tools/static_analysis.sh` | `tools\static_analysis.ps1` |
| sanitizers | `tools/sanitize.sh` | `tools\sanitize.ps1` |
| profiling | `tools/profile.sh` | `tools\profile.ps1` |
| supply chain | `tools/supply_chain.sh` | `tools\supply_chain.ps1` |
| SonarQube | `tools/sonar_scan.sh` | `tools\sonar_scan.ps1` |
| Coverity export → dashboard | `python3 tools/coverity_findings.py` | same script |
| Axivion | `axivion/start_analysis.sh` | `axivion\start_analysis.ps1` |
| PlantUML fetch | `tools/fetch_plantuml.sh` | `tools\fetch_plantuml.ps1` |
| IDE project | (Qt Creator opens CMakeLists.txt) | `tools\make_vs_solution.ps1` → `build-vs\TradingApp.sln` |
| standalone deployment | (not needed — the build tree runs in place) | `tools\deploy_app.ps1` (windeployqt bundle, Windows-only) |

On top of the shared `app` / `release` extra stages, `build_all.ps1` also
accepts the Windows-only extras `vs` (generate the solution) and `deploy`
(run `tools\deploy_app.ps1`) — both only when named.

The Python tools (`trace_report.py`, `sdoc_to_md.py`, `parse_sanitizer_log.py`,
`merge_findings.py`, `coverity_findings.py`) are shared verbatim — they are platform-neutral and both
platforms produce byte-identical generated artefacts (explicit `encoding="utf-8"`
and `newline="\n"` — except `merge_findings.py`, whose CSV writer uses
`newline=""` as the `csv` module requires — so regenerating on Windows does not
flip `docs/requirements.md` to CRLF).

## Missing tools: skipped, never fatal

Stage outcomes are tri-state on both platforms — `ok`, `skipped`, `FAILED`. A
stage script that exits **3** means "skipped": it needs a tool that is
license-bound or simply absent, so it could not run. `build_all.ps1` and
`build_all.sh` report that as `skipped`, print a one-line count at the end, and
**do not fail the pipeline**. Any other non-zero exit is a real failure.

That makes the repository usable without the commercial licences:

| tool | licence | behaviour when absent |
|---|---|---|
| Axivion Suite | commercial | `axivion` stage → `skipped` with a message naming what to install |
| Squish Coco | commercial | `coverage -Mode coco` → `skipped`; `-Mode auto` silently uses the other back ends |
| OpenCppCoverage | open source (installed by `setup.ps1`) | `-Mode msvc` → `skipped` |
| LLVM | open source (installed by `setup.ps1`) | `-Mode mcdc` → `skipped`; the UBSan stage reports `ok` (it degrades to a no-op and writes an empty findings log) |
| clazy / TSan / valgrind | n/a on Windows | reported as unavailable, log says why |

Everything **open source** that the pipeline needs is installed by
`.\setup.ps1` (winget + pip + aqt) or `./setup.sh` (apt + pipx + aqt) — nothing
has to be fetched by hand. `.\setup.ps1 status` lists the three groups
separately: installable, license-bound, and no-Windows-counterpart.

## Quick start

```powershell
.\setup.ps1                  # install/verify the toolchain (winget + pip + aqt)
.\setup.ps1 status           # read-only report of what is present
.\build_all.ps1 -Skip axivion
.\build_all.ps1 build test   # just the fast stages
```

`build_all.ps1` picks the Qt kit itself (newest kit that contains Qt6Charts,
MSVC preferred). Override with `$env:QT_PREFIX` or `-QtKit mingw_64`. It imports
the Visual Studio developer environment into the session automatically — no
"x64 Native Tools" prompt needed.

## Both compilers: MSVC and MinGW

```powershell
.\build_all.ps1 build test                   # MSVC (default)
.\build_all.ps1 build test -QtKit mingw_64   # MinGW
```

Both are verified against Qt 6.11.1: MSVC 19.44 and MinGW g++ 13.1.0 each build
the app and all 12 test binaries and pass 12/12.

The MinGW path needs two things the MSVC path does not, both handled by
`Initialize-KitToolchain` in `tools\common.ps1`:

* **The toolchain is not in the kit.** A MinGW Qt kit ships no compiler; the
  matching one lives in `<QtRoot>\Tools\mingw*_64` and is only on PATH if Qt
  Creator put it there. It is selected by the **version number in the directory
  name, not by the name** — as strings, `mingw810_64` (GCC 8.1) sorts above
  `mingw1310_64` (GCC 13.1), and GCC 8 cannot compile C++23. The symptom of
  getting that wrong is a try_compile failure deep inside `find_package(Qt6)`
  saying the compiler "does not support" `CXX23`.
* **Its `bin` must be on PATH even when the compiler is named explicitly.**
  `cc1plus` loads its DLLs from there; without it every compilation exits 1 with
  **no diagnostic at all**. The same directory supplies `libstdc++-6.dll`,
  `libgcc_s_seh-1.dll` and `libwinpthread-1.dll` at test runtime, which is why
  `run_tests.ps1` and `profile.ps1` initialise the kit toolchain too, not just
  `build_all.ps1`.

The compiler is passed explicitly (`-DCMAKE_CXX_COMPILER=…`) so a different
MinGW already on PATH cannot win, and switching kits in an existing build tree
is detected and the tree discarded (see below).

## Running the app straight from the build directory

A freshly built `build\TradingApp.exe` needs the Qt kit on PATH. To make it
self-contained instead:

```powershell
.\build_all.ps1 deploy            # or: tools\deploy_app.ps1
tools\deploy_app.ps1 -BuildDir build-release
tools\deploy_app.ps1 -IncludeTests   # also make the tst_*.exe standalone
```

This runs `windeployqt` and copies the Qt DLLs, the `platforms\qwindows*.dll`
plugin, the **Schannel TLS backend** (needed for HTTPS to the eToro API), image
formats, styles and the compiler runtime next to the executable. Verified by
launching the result with `PATH` reduced to `system32` — the app starts and
renders normally.

Two details worth knowing:

* **The kit is read from the build tree's `CMakeCache.txt`, not from PATH or
  kit auto-detection.** Deploying Qt 6.11.1 release DLLs next to a binary linked
  against 6.9.2 debug ones yields an executable that starts and then dies on the
  first Qt call, so the deployment has to follow the binary.
* **A MinGW build also needs `libstdc++-6.dll`, `libgcc_s_seh-1.dll` and
  `libwinpthread-1.dll`.** `windeployqt --compiler-runtime` only finds those when
  the toolchain happens to be on PATH, so they are copied explicitly.

A Debug deployment is large (~420 MB, dominated by the debug Qt DLLs and the
`opengl32sw.dll` software rasteriser). For something shippable use the release
build and the install/CPack route, which stages the same deploy script:

```powershell
cmake --install build-release --prefix dist
cd build-release; cpack            # ZIP; cpack -G NSIS for an installer
```

## Switching kits, generators, or WSL/Windows in one working tree

A CMake build tree records the absolute source and binary paths, the generator
and the compiler it was configured with, and refuses to be reused if any of
them changed. This repository invites all three collisions: the same checkout is
`C:\AxivionRepoCheck\TradingApp` from Windows and
`/mnt/c/AxivionRepoCheck/TradingApp` from WSL, both platforms default to
`build/`, and `-QtKit mingw_64` swaps the compiler.

`Reset-StaleCMakeCache` (PowerShell) and `reset_stale_cache` (shell) detect the
mismatch, discard the tree and say why, instead of surfacing

    CMake Error: The current CMakeCache.txt directory C:/…/build/CMakeCache.txt
    is different than the directory /mnt/c/…/build where CMakeCache.txt was created.

## Visual Studio and CMake presets

`CMakeLists.txt` is the single description of the build (DES-BLD-CMAKE), so the
solution is generated rather than committed — a hand-maintained `.sln` would be a
second, drifting description of the same targets. `.gitignore` covers `build-vs/`,
`*.sln` and `*.vcxproj*` accordingly.

```powershell
.\tools\make_vs_solution.ps1          # -> build-vs\TradingApp.sln
.\tools\make_vs_solution.ps1 -Open    # ... and open it
.\build_all.ps1 vs                    # same, as a named extra stage
```

`CMakeLists.txt` adds a few IDE-only properties (all guarded by
`if(CMAKE_CONFIGURATION_TYPES)`, so Ninja builds are unaffected):

* `VS_STARTUP_PROJECT` — F5 launches `TradingApp`, not `ALL_BUILD`. Override with
  `-StartupProject tst_tradeplan` to debug a test instead.
* `VS_DEBUGGER_ENVIRONMENT` — puts the Qt `bin` directory on PATH for the app and
  every test project, otherwise debugging starts with a
  "Qt6Cored.dll was not found" dialog.
* `USE_FOLDERS` + `AUTOGEN_TARGETS_FOLDER` / `FOLDER "Tests"` — keeps Solution
  Explorer readable despite the ~30 moc/uic helper targets.

[CMakePresets.json](../CMakePresets.json) is the `.sln`-free route and works on
both platforms: `windows-msvc-debug`, `windows-msvc-release`, `visual-studio`,
`linux-gcc-debug`, `linux-gcc-release`. Every preset takes the kit from
`$QT_PREFIX`, so nothing machine-specific is committed. Visual Studio picks the
presets up from **File → Open → Folder**; from a shell:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

## Tool mapping, and what genuinely does not exist here

Every substitution below is a deliberate choice, not a silent gap. The scripts
print the reason at the point where the Linux tool would have run.

| Linux | Windows | note |
|---|---|---|
| g++ / clang-18 | MSVC 19.4x (`cl`), clang-cl, MinGW g++ | MSVC is the default; the MinGW kit works via `-QtKit mingw_64` |
| cppcheck | cppcheck | same tool, same flags |
| clang-tidy | clang-tidy | same tool; newer upstream versions report extra checks (see below) |
| `g++ -fanalyzer` | **MSVC `/analyze`** | same role — a second, compiler-native symbolic-execution pass over every TU. `tools/msvc_analyze.py` normalizes the C6xxx diagnostics to the GCC-style line format, dashboard provider `msvc-analyze`. |
| Clang Static Analyzer | Clang Static Analyzer | same shared driver `tools/clang_analyzer.py`; it picks the clang matching the compile database's dialect (clang-cl for an MSVC database, clang++ for MinGW) and prints "skipped" when no clang driver is installed. Provider `clang-analyzer`. |
| lizard | lizard | same shared driver `tools/lizard_metrics.py`, same limits and the same `tools/lizard_baseline.json` ratchet. Installed by `.\setup.ps1` through pip. |
| PMD CPD | PMD CPD | same shared driver `tools/cpd_scan.py`. PMD is a Java tool: `.\setup.ps1` fetches the pinned dist into `tools\third-party\` (`tools\fetch_pmd.ps1`), `Get-PmdLauncher` in `tools\common.ps1` locates it. |
| clazy | *nothing* | no Windows build exists. Not a coverage gap: Axivion's `Qt-*` ruleset (~180 rules, including the clazy checks) runs on every `axivion_ci`. |
| gcov + lcov | **OpenCppCoverage** | GCC-only toolchain. OpenCppCoverage reads the PDBs and gives LINE coverage but no branch coverage; branch + MC/DC come from the two MC/DC back ends instead. |
| clang-18 `-fcoverage-mcdc` | clang-cl `-fcoverage-mcdc` + llvm-cov | works; see MC/DC below |
| Squish Coco | Squish Coco | works; see MC/DC below |
| ASan + UBSan (one GCC build) | ASan (MSVC) + UBSan (clang-cl), **two build trees** | MSVC cannot combine them; separate dashboard providers `asan` and `ubsan`. MSVC's ASan has **no LeakSanitizer** — leaks are not reported on Windows. |
| TSan | *nothing* | ThreadSanitizer has no Windows target in MSVC or upstream LLVM. Race evidence for REQ-N-006 comes from the Linux run. |
| valgrind memcheck | *nothing* | Linux/macOS only. ASan covers the same error classes except leaks; for leak checking use the Linux pipeline, or Application Verifier / Dr. Memory by hand. |
| perf / gperftools | VSDiagnostics, `wpr`/`wpa` | there is no `perf report --stdio` equivalent, so `profile.ps1` records a trace for a GUI analyzer instead of printing a text hotspot table. The deterministic `QBENCHMARK` suite (`tst_benchmarks`) is unaffected and is what `/perf-check` compares. |

## MC/DC on Windows — two independent measurements

`tools\coverage.ps1 -Mode auto` runs both, because they instrument at different
levels and cross-check each other:

* **Squish Coco** (`-Mode coco`) — source instrumentation, true MC/DC plus
  multiple-condition coverage, and the `.csmes` opens in `coveragebrowser` for
  per-decision drill-down. Report: `coverage\coco\index.html`, machine-readable
  `coverage\coco\summary.csv`.
* **clang-cl + llvm-cov** (`-Mode mcdc`) — IR instrumentation, MC/DC column in
  `coverage\mcdc\summary.txt` and in the per-file HTML.

Three things are load-bearing for the Coco path and were each found the hard way:

1. **`-DCMAKE_CXX_STANDARD=20` for the Coco tree only.** Coco's instrumenting
   front end (2025-11 build) does not parse C++23: with `/std:c++latest` it
   emits a wall of `syntax error, unexpected requires` against the Qt headers
   and then fails with `Could not insert instrumentation in file …`. `CMakeLists.txt`
   therefore only defaults `CMAKE_CXX_STANDARD` to 23 when it is not already
   defined. Every other build tree — including everything that ships or is
   analyzed — stays on C++23.
2. **Both the librarian and the linker must be wrapped**, not just the compiler:
   `-DCMAKE_CXX_COMPILER=cscl.exe -DCMAKE_AR=cslib.exe -DCMAKE_LINKER=cslink.exe`.
   This project links `trading_domain`/`trading_services` as static libraries;
   archiving them with plain `lib.exe` drops the per-TU coverage tables and every
   test binary then fails with `LNK2001: unresolved external __cs_tb_*`.
3. **Qt, MSVC STL and Windows SDK headers must be excluded from
   instrumentation.** Otherwise Coco instruments their inline/template code, each
   TU instantiates the templates differently, `cmmerge` reports
   `source file qmetatype.h is differently instrumented in the database` for
   every test binary, and `cmreport` then crashes with an access violation.

Also note that an instrumented binary writes its execution report
(`<exe>.csexe`) into the **current working directory**, not next to the
executable — `coverage.ps1` runs each test from the tests directory for that
reason, and `.gitignore` covers stray `*.csexe`/`*.csmes`.

## PowerShell gotchas that cost hours

* **`.ps1` files are stored with a UTF-8 BOM** (enforced via `.gitattributes`).
  Windows PowerShell 5.1 — the version shipped with Windows — decodes a BOM-less
  file as ANSI. The em dashes in these scripts then become U+201D, which
  PowerShell treats as a *string delimiter*, and every script fails to parse with
  `The string is missing the terminator`.
* **PowerShell 5.1 strips embedded double quotes** when it builds a native
  command line. Any `python -c "…"` probe silently returns nothing. Compute such
  values in PowerShell instead (see `Get-PythonScriptsDirs` in `tools\common.ps1`).
* **`ValueFromRemainingArguments` still binds positionally to later parameters**
  unless one parameter declares an explicit `Position`. Without
  `[Parameter(Position = 0, …)]` on `$Stages`, `build_all.ps1 build test trace`
  becomes `Stages=build, Skip=test, QtKit=trace`.
* **CMake strips backslashes out of `CMAKE_EXE_LINKER_FLAGS`.** A compiler-rt
  path put there arrives at the linker as `C:Program FilesMicrosoft…` and
  `lld-link` reports `could not open 'C:Program'`. The scripts put the directory
  on the linker's `LIB` search path and pass only the bare library name.
* **Two clang installations are normal** (the one bundled with Visual Studio and
  a standalone LLVM). Mixing `clang-cl` from one with `llvm-cov`/`llvm-profdata`
  from the other yields `unsupported instrumentation profile format version`;
  `Get-LlvmToolset` in `tools\common.ps1` pins all three to one installation.
* **A silent `winget install` of cppcheck, Graphviz or LLVM does not extend
  PATH.** `tools\common.ps1` adds the well-known install directories to the
  session PATH so the scripts work anyway; `setup.ps1` also persists them.
* **A stage function must not leak command output into its return value.**
  `$ok = Invoke-Native …` would otherwise be "true" for any command that printed
  something, including failing ones — hence `| ForEach-Object { Write-Host $_ }`
  in `Invoke-Native` and in every `Invoke-*Stage`. `| Out-Host` was deliberately
  rejected: it writes past every redirection, so `*> log.txt` and the CI
  artifact upload would capture an almost-empty log.
* **Quote `-D` properties passed to java.** Unquoted, PowerShell splits
  `-Djava.awt.headless=true` at the dot and java reports
  `Could not find or load main class .awt.headless=true`.
* **`Measure-Object` returns `$null` for an empty directory**, so reading `.Sum`
  off it under `Set-StrictMode` throws `PropertyNotFoundStrict`. `clean_all.ps1`
  sums file sizes by hand for that reason.

## Two build-configuration traps

* **`/fsanitize=address` must also be on the LINK line**, not only when
  compiling. It is what makes the linker pull in
  `clang_rt.asan_dynamic_runtime_thunk`, so that the exe's `operator new`/
  `operator delete` resolve against the ASan runtime rather than the plain CRT.
  Without it the process dies before `main()` with
  `The procedure entry point ??3@YAXPEAX_K@Z could not be located in <exe>`
  (`??3@YAXPEAX_K@Z` = `operator delete(void*, size_t)`), i.e. exit code
  `0xC0000139` / `STATUS_ENTRYPOINT_NOT_FOUND`.

* **THE one that actually bit: `$env:LIB` leaking from the `coverage` stage into
  the `sanitize` stage.** Two *incompatible* ASan implementations are installed
  side by side, and both ship a file called `clang_rt.asan_dynamic-x86_64.lib`:

  | implib the linker picked | what the exe then imports |
  | --- | --- |
  | MSVC (`VC\Tools\MSVC\<ver>\lib\x64`) | `__asan_delete`, `__asan_delete_size` |
  | LLVM (`…\lib\clang\<major>\lib\windows`) | mangled `??2@YAPEAX_K@Z`, `??3@YAXPEAX_K@Z` |

  The DLL that *loads* at startup is always MSVC's, and it does not export the
  mangled names — so a binary linked against LLVM's implib dies before `main()`
  with `0xC0000139` / `STATUS_ENTRYPOINT_NOT_FOUND`, i.e. the
  `??3@YAXPEAX_K@Z` dialog.

  How LLVM's directory got onto the ASan link path: `tools\coverage.ps1` calls
  `Add-ToLibPath` on LLVM's compiler-rt directory to find
  `clang_rt.profile-x86_64.lib` — and **that same directory also contains LLVM's
  `clang_rt.asan_dynamic-x86_64.lib`**. `build_all.ps1` runs every stage in ONE
  PowerShell process, with `coverage` *before* `sanitize`, so the mutation was
  still in `$env:LIB` when the ASan tree was linked.

  This is why the bug looked haunted: `tools\sanitize.ps1` on its own always
  passed (clean `$env:LIB`), while a full `build_all.ps1` produced 13 unstartable
  test binaries. Both sides are fixed — `coverage.ps1` restores `$env:LIB` when
  its stage ends, and `Invoke-Asan` additionally strips any `\lib\clang\`
  directory for the duration of its own configure+build, so it cannot be poisoned
  by whatever ran before it. Reproduced on demand and repaired: prepending LLVM's
  compiler-rt dir to `$env:LIB` turns the link into `??3@YAXPEAX_K@Z` + exit
  `0xC0000139`; with the guard, the same polluted environment yields
  `__asan_delete` + 13/13 passing.

* **The ASan runtime is also staged next to the binaries** rather than looked up
  on `PATH`. A program's own directory is searched first, so copying the runtime
  belonging to the very `cl.exe` that built the tree into `build-san\` and
  `build-san\tests\` removes the *other* startup failure — `0xC0000135` /
  `STATUS_DLL_NOT_FOUND` when no copy is on `PATH` at all, which is what happens
  outside a developer prompt. Verified by running all 13 test exes in a plain
  shell with no MSVC environment.

  Both startup failures are **modal dialogs**, so an interactive run *hangs*
  instead of failing and `ctest` reports nothing useful. When triaging, get the
  exit code rather than reading the dialog: `0xC0000135` = runtime missing,
  `0xC0000139` = runtime present but wrong ABI.

* **Not the cause, kept as hygiene: `/INCREMENTAL:NO` and `/RTC1`.** An earlier
  diagnosis blamed incremental linking, on an A/B that turned out to be
  confounded — the "before" binaries came from a poisoned-`$env:LIB` session and
  the "after" ones from a clean one, so the flag was never the variable under
  test. `sanitize.ps1` still sets `CMAKE_EXE_LINKER_FLAGS_DEBUG=/debug
  /INCREMENTAL:NO`, because MSVC advises non-incremental linking for ASan and
  because the *mechanism* found along the way is real and worth knowing:
  `CMAKE_EXE_LINKER_FLAGS` is emitted **before** `CMAKE_EXE_LINKER_FLAGS_DEBUG`
  (default `/debug /INCREMENTAL`), so an `/INCREMENTAL:NO` placed in the
  config-agnostic variable is silently undone by last-one-wins. That rule applies
  to any `CMAKE_<LANG>_FLAGS` / `CMAKE_EXE_LINKER_FLAGS` override in a Debug
  build.

* **CMake 4.0 moved `/RTC1` out of `CMAKE_CXX_FLAGS_DEBUG`** into its own
  abstraction (`CMAKE_MSVC_RUNTIME_CHECKS`, policy CMP0197). Overriding
  `CMAKE_CXX_FLAGS_DEBUG` silently stopped removing it, so ASan builds quietly
  went back to compiling *with* MSVC runtime checks. `sanitize.ps1` now clears
  `CMAKE_MSVC_RUNTIME_CHECKS` as well. This one is hygiene rather than a crash
  fix — MSVC 14.44 does accept `/RTC1` alongside `/fsanitize=address`, but the
  two instrument the same stack and `/RTC1` has no place in a sanitizer build.
* **CMP0156 warnings are silenced in `CMakeLists.txt`, not on the command line.**
  `cmake_minimum_required(4.2)` turns CMP0156 NEW; Qt forces it back to OLD for
  non-Apple platforms and warns once per `qt_add_library`/`qt_add_executable`
  call — around 20 screens of output. Passing
  `-DQT_FORCE_CMP0156_TO_VALUE=OLD` does **not** help: only the value `NEW`
  silences that check, and `NEW` would change how Qt links. The project sets
  `cmake_policy(SET CMP0156 OLD)` instead, matching what Qt imposes anyway, so
  the build is unchanged and the warning has nothing to report.

## Findings that differ from Linux

The Windows run is not expected to produce the same finding count, because the
analyzer versions differ. On the reference machine:

* **cppcheck 2.21** reports one `returnByReference` performance finding in
  `src/ui/TradeGauge.h` that the older Linux cppcheck does not.
* **clang-tidy 19/22** reports ~27 findings from checks that did not exist in
  clang-tidy 18 (`modernize-use-designated-initializers`,
  `readability-math-missing-parentheses`, `readability-use-std-min-max`,
  `modernize-use-ranges`).

These are real findings in existing code, surfaced by newer tools — they are
reported, not suppressed.

## Axivion on Windows

`axivion\start_analysis.ps1` mirrors the shell version, including the single-run
lock (an exclusive file handle instead of `flock`). It runs the same rule set —
MISRA C++ 2023, the Qt-Autosar rules and the architecture checks — against the
MSVC build.

### Configuration layering

`axivion/compiler_config.json` was generated by `gccsetup` on the Linux machine
and hardcodes `/usr/bin/gcc`, so it cannot describe a Windows build. Unlike GCC,
the Microsoft compiler needs no generated profile: the Suite has built-in support
for it. So Windows gets its own entry point,
`axivion/windows/axivion_config.json`, which reuses the shared layers by relative
path and adds two overrides:

| layer | purpose |
|---|---|
| `axivion/compiler_config_msvc.json` | activates `Project/MicrosoftToolchain` (`native_compiler: cl`), deactivates `GNUToolchain` |
| `axivion/ci_config_windows.json` | overrides `ir` to `build_axivion/TradingApp.exe` — the Windows linker launcher writes the IR under the linked output name, which carries the `.exe` suffix. Without this the analysis phase aborts with `Failed to load IR "build_axivion\TradingApp"`. |

Layer order matters: `_Layers` is evaluated **bottom to top**, so a layer listed
*above* another wins — which is why `ci_config_windows.json` sits above
`ci_config.json`.

`start_analysis.ps1` selects this directory automatically; override with
`-ConfigDir` or by setting `BAUHAUS_CONFIG`.

### Two traps on this machine

* **Several Suites can be installed at once, and the path does not tell you which
  is newer.** Here 7.12.1 sits in `Program Files` while 7.12.3 sits in
  `Program Files (x86)`. Taking the first path that matches gets you `axivion_ci`
  from one version chainloading the CMake launcher toolchain of the other, which
  loops until CMake errors out. `Get-AxivionSuite` in `tools\common.ps1` asks each
  install its version and takes the newest.
* **Qt 6.11.1 trips an internal error in Axivion's C++ front end.** IR generation
  dies with
  `Assertion failed: edg::is_at_least_one_error(), control_flow_expressions.cpp:4168`
  while processing `QtCore/qvariant.h:511`, and writes a `*.reproducer.zip` next
  to the object file. Qt 6.9.2 analyzes cleanly, so pin the kit for this stage:

  ```powershell
  $env:QT_PREFIX = 'C:\Qt\6.9.2\msvc2022_64'; .\axivion\start_analysis.ps1
  ```

  This is a Suite bug (it says so itself: report to `axivion.support@qt.io`), not
  a project problem — the same sources compile warning-free with MSVC and analyze
  fine on Linux.

### Shared import layer

`axivion/external_import.py` is shared and platform-aware: it uses `cat` on Linux
and `cmd /c type` on Windows (there is no `cat`, and `type` is a shell builtin
rather than an executable), it finds the project root by walking up to
`CMakeLists.txt` (so it also works from the nested `axivion/windows/` directory),
and it registers the Windows-only providers `msvc-analyze`, `asan` and `ubsan`
alongside the Linux ones. A provider whose log file is absent imports nothing, so
one configuration serves both platforms.
