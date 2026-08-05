# TradingApp — project instructions

Qt 6 / C++23 desktop app trading eToro instruments via the official public
API; SIMULATION mode without keys. Quality toolchain is the point of this
repo: requirements-as-code, full traceability, seven static analyzers
(cppcheck, clang-tidy, Clang Static Analyzer, g++ -fanalyzer, clazy, Axivion,
Coverity Scan) + code metrics (lizard) + clone detection (PMD CPD) + three
sanitizers on one Axivion dashboard.

## Entry points

```bash
./setup.sh                     # provision/update all tools (naked Debian/Ubuntu)
./build_all.sh                 # everything incl. Axivion; --skip axivion; app; release
./clean_all.sh [--deep]        # remove everything generated
tools/run_tests.sh build       # test suite with JUnit output
python3 tools/trace_report.py  # traceability matrix; fails on hard gaps
tools/static_analysis.sh build [--fix]   # cppcheck+clang-tidy+CSA+clazy+
                               # g++ -fanalyzer+lizard+PMD CPD+codespell
tools/lizard_metrics.py . analysis-results --update-baseline  # re-ratchet metrics
tools/sanitize.sh [asan-ubsan|tsan|valgrind|all]
tools/make_report.py           # downloads/TradingApp-quality-report.pdf — the
                               # whole run in one colour PDF (build_all `report`
                               # stage; skips with exit 3 without reportlab). Reads
                               # the Axivion dashboard via AXIVION_DASHBOARD_URL,
                               # so run it AFTER the axivion stage to include it
tools/profile.sh               # perf/gperftools over build-release/
tools/mcp_env.sh --persist     # env the .mcp.json Axivion MCP servers need
tools/common.sh                # sourced, not run: host arch -> Qt kit dir
                               # (gcc_64 / gcc_arm64), aqt host+arch, AppImage
                               # arch, LLVM toolset. Run it to print what a
                               # machine resolves to
tools/package_appimage.sh      # downloads/TradingApp-<ver>-<arch>.AppImage
                               # (x86_64 or aarch64 — the host's)
tools/build_android.sh [--abi android_arm64_v8a] [--run]   # APK -> downloads/;
                               # --run boots an emulator and screenshots the app
tools/run_android.sh           # emulator only (needs /dev/kvm + the kvm group)
./setup.sh android             # SDK+NDK+system image+Qt kits (~6 GB, separate mode)
tools/package_portable.ps1     # downloads/TradingApp-<ver>-windows-x64.zip
```

Windows has a PowerShell counterpart for EVERY one of those (`setup.ps1`,
`build_all.ps1`, `clean_all.ps1`, `tools\*.ps1`, `axivion\start_analysis.ps1`);
the Python tools are shared verbatim. Both platforms are verified. Details and
the tool substitutions: `docs/windows.md`. When changing a `*.sh` script or a
shared Python tool, change its counterpart too — they are meant to stay in
lockstep.

Skills: `/verify` (all checks), `/axivion-dashboard` (run + REST verification),
`/ax-fixcode` (fix Axivion SVs + refactor the marked/selected code),
`/add-requirement` (requirements-as-code workflow), `/perf-check` (benchmarks).

## Non-negotiables

- Requirements live ONLY in `requirements/requirements.sdoc` (StrictDoc);
  `docs/requirements.md` is generated (`tools/make_requirements.sh`).
- Every test carries `//! @tstid TS-… @design DES-…` plus
  `// @relation(REQ-…, scope=function)` (plain `//` — StrictDoc ignores `//!`).
  Test classes write `Q_OBJECT;` (semicolon = tree-sitter parse anchor).
- Keep every boolean decision ≤ 6 conditions (clang-18 MC/DC limit).
- Analyzer configs are strict by construction: every disabled check in
  `.clang-tidy` / `tools/cppcheck-suppressions.txt` carries a written reason AND
  the measured hit count that justifies it. Do not silence a check without both.
  `tests/.clang-tidy` inherits the root config and turns off exactly one check.
- The compiler is an analyzer too: `-Wall -Wextra` + the Qt-relevant extras in
  CMakeLists, fatal under `-DTRADINGAPP_WARNINGS_AS_ERRORS=ON` (what build_all
  configures). GCC-only `-W…` spellings must be gated by compiler id — clang
  reads the same compile database. NEVER add `-Wpedantic`: it reports the
  required `Q_OBJECT;` anchor as an extra `;`.
- The lizard metrics gate is a ratchet, not a threshold: over-limit functions are
  recorded in `tools/lizard_baseline.json` with their numbers. New debt, a
  worsened number, or a stale entry all fail the stage. Regenerate deliberately.
- cppcheck's free MISRA addon is MISRA **C** 2012 and this code is C++23: measured
  527 findings of which 514 are the mismatch (404 `misra-config` on Q_OBJECT-style
  macros, 110 rule-12.3 hits on template argument lists). It is therefore
  informational only — `tools/misra_cppcheck.sh` — and MUST NOT be added to the
  gate; MISRA C++ 2023 here is Axivion's job. The other three addons were measured on the REAL compile database (a bare file
  list makes them bail out early and look clean — do not repeat that mistake):
  `misc-implicitlyVirtual` 16 (wants `virtual` on an `override`), `threadsafety`
  19 (getenv in `Config::load`, which runs before any thread exists), `findcasts`
  7 (a cast inventory). All informational, all in `tools/misra_cppcheck.sh`.
- PMD CPD (≥ 100 tokens) is the only clone gate — the Axivion configuration here
  is MISRA-only. Fix clones by extracting a helper; do not baseline them.
- Downloadable builds: `tools/package_appimage.sh` (linuxdeploy; needs
  `-DTRADINGAPP_SKIP_QT_DEPLOY=ON`, since Qt's own Linux deploy step aborts on
  RUNPATH length inside an AppDir) and `tools/package_portable.ps1` (windeployqt
  via the CMake install rules). Both write to git-ignored `downloads/`;
  `.github/workflows/release.yml` runs THESE scripts on a `v*` tag — never
  reimplement the packaging in YAML.
- Header-inline functions that grow logic: define out-of-line in one TU
  (comdat coverage records otherwise break llvm-cov).
- Layering is linker-enforced: domain (Qt Core only) ← services ← ui.
- Linux means x86-64 AND ARM64 (Raspberry Pi 4/5, 64-bit OS only). NEVER hardcode
  `gcc_64`, `x86_64` or `clang-18` in a script again — those three names come from
  `tools/common.sh` (`qt_kit_dir`, `host_arch`, `llvm_suffix`), which every Linux
  entry point sources. A host with no `~/Qt` kit for its architecture resolves an
  EMPTY prefix on purpose: the build then uses the distribution's Qt 6.
  ARM64-unsupportable stages say so and skip — Axivion Suite is x86-64-only.
  Evidence: the `build-linux-arm64` CI job runs `./build_all.sh build test trace`
  on `ubuntu-24.04-arm` with no arch arguments; details in docs/platforms.md.
- Money-moving actions need the double-press gate; advisory features never
  trade (REQ-N-005). Secrets only in git-ignored `apiKeyEtoro.json`.
- The bot simulation (REQ-F-029, `domain/PaperTrader` + `ui/BotSimPanel`) is
  SIMULATED money on LIVE prices, and must keep having NO route to an order
  endpoint — reads only (quotes/spreads/fees/decision rows, plus
  `EtoroClient::setExtraQuoteInstruments`, which registers quote interest, not an
  order). Its cost model charges half the LIVE spread per side plus per-night
  rollover with the tripled weekend night; never simplify those away — a
  simulation without costs measures nothing. Those costs also DECIDE exits: close
  when the remaining upside no longer covers rollover-to-horizon + exit spread, and
  before the tripled weekend charge unless the position has earned it (a credit
  never closes; unknown fees keep both rules silent). `TRADINGAPP_BOT_ARM=1` arms it at
  startup for unattended runs, and the armed flag + AI mode are PERSISTED (an
  experiment that stops silently on restart looks like a working bot finding
  nothing). How many trades it holds is governed by the PORTFOLIO RISK BUDGET
  (Σ loss-if-every-stop-hit ≤ `maxPortfolioRiskFraction` × equity), never by a
  trade count — don't reintroduce a queue limit. Every scan logs one summary line
  (candidates, opened, risk vs budget, refusals per `code`); those codes on
  `EntryVerdict`/`AiGate` are what make it countable, so keep them stable.
- The bot's DEFAULT decision source is the local model in LEAD mode
  (`BotConfig::aiMode`), bounded by every risk rule; a book saved earlier keeps the
  mode it was left in, because an upgrade must not change what a running experiment
  measures. Opening a position raises a NON-MODAL notice, at most one on screen
  (`MainWindow::onBotTradeOpened`) — a modal box would stop the marking/exit timers,
  and one scan can open a dozen trades.
- Prediction rests on AGREEMENT BETWEEN INDEPENDENT reads (REQ-F-035,
  `domain/IndexConfluence`): futures leadership, volatility DIRECTION (^VXN for
  Nasdaq, ^VIX otherwise), the US 10-year yield, heavyweight participation and the
  opening range. `MarketFeeds::fetchReferenceSeries` fetches the eleven tickers.
  Two invariants: an unmeasurable read is UNKNOWN and NEVER counts as agreement
  (a "4 of 5" built from absent feeds is a lie), and heavyweight participation is
  labelled a STAND-IN for breadth — real breadth needs per-constituent data this app
  does not fetch. The bot refuses below `minAgreeingReads` (3) MEASURED agreements
  (`no-confluence`), and the threshold is clamped to what is actually available.
- Session STRUCTURE is read before any oscillator (REQ-F-022, `openingRange` +
  `relativeStrength` in DecisionEngine): both come from the 1-minute series the app
  already fetches for every catalog instrument — including ES=F and NQ=F via
  SP.24-7 / NSDQ100.24-7 — so they cost no new feed. They go into the evidence prompt,
  and the bot refuses to open INTO a fresh opposite break (`against-range-break`).
  True market breadth (advance/decline, up-volume, constituents above VWAP) is NOT
  available here: it needs per-constituent data the app does not fetch, and the
  Nasdaq-vs-S&P read is the honest stand-in — don't let a comment claim otherwise.
- Some instruments are traded RELUCTANTLY (REQ-F-034, `reluctantSymbols`, default
  USDOLLAR): allowed only when the expected move per hour at the chosen leverage
  clears `reluctantMinHourlyMovePct` AND conviction clears
  `minConfidence × reluctantConfidenceFactor`. Refusal code `reluctant-symbol`.
  Measured: 3 USDOLLAR trades for −19.22 EUR on a dollar index that moves hundredths
  of a percent an hour.
- The record is decomposed BY EXIT RULE (`PaperPerformance::netByReason`) and the
  window shows it worst-first: on the first 18 real closes that view said
  `signal faded` −97.12 over 7 trades vs `banked before giving it back` +99.10 over 2,
  on a book whose GROSS was +119.35 against 148.77 of costs. Keep that view — a total
  hides which rule is the problem. The fade rule now also needs the loss to exceed
  `fadeMinLossOverCost` × the exit cost, because closing a barely-losing position pays
  the spread to save nothing.
- Every widget in src/ui carries a stable objectName, enforced by
  `tools/check_object_names.py` in the analysis stage (REQ-N-007) — the Squish object
  map addresses by name only. A GUI run cannot reach a real account:
  `TRADINGAPP_FORCE_SIMULATION` makes `Config::hasCredentials()` answer false at the
  ONE place every mode question reads (TS-CFG-007), and `squish/suite_gui/envvars`
  sets it for every run. Licence-bound stages (`gui`, `testcenter`, coco, axivion) exit
  3 and are listed as MISSING LICENCES in the quality PDF — never a gate.
- Churn, not strategy, is what lost money in the first measured hour (6 closes, median
  hold 5.2 min, gross +1.64 EUR against 19.38 EUR of costs). REQ-F-034 answers it and
  the numbers are load-bearing: `reentryCooldownMinutes` (45), `maxOpensPerHour` (6),
  `minHoldMinutes` (30, applies to AiExit/SignalFade/GiveBack but NEVER to stops,
  targets or the carry rules), `aiExitMinConfidence` (60 — a 1.5B model is not
  consistent between two calls). A blocked opinion is still REPORTED (`ai-too-soon`,
  and the window's AI column still shows "close"), because hiding it would make the
  bot look broken. `sessionPhaseFor` reads the instrument's OWN exchange clock for the session edges and
  the NEW YORK clock for US releases and the Fed (never a fixed offset: Europe and the
  US shift their clocks on different days, which is exactly what an offset gets wrong).
  `OpeningChaos` (first 15 min) and `PolicyWindow` (14:00-14:45 NY = the statement plus
  the press conference) are SAT OUT, not sized down; a scheduled release outranks the
  "rest of the opening hour", and `groupLeverageCap` bounds leverage per bucket
  with fx tightest (x5). One scale bug to never reintroduce: in lead mode
  `entryConfidence` is the MODEL's number while `confNow` is the COMPOSITE's — the
  fade rule must read `entryCompositeConf`, or every model-led trade closes on open.
- The bot MANAGES positions (REQ-F-032): stacking in one instrument is allowed only
  when the model names it again (`aiBacked`), never against an existing side
  (`opposite-open`), and under `maxPositionsPerSymbol` + `maxSymbolRiskFraction`
  (3%, the tightest of the three risk caps). The model is shown the OPEN book
  (`paperHoldEvidence`) and may close a position (`paperAiHold`: opposite side or an
  explicit CLOSE) — but SILENCE MUST NEVER CLOSE, because a 1.5B model routinely
  answers about two instruments out of twenty-six. Two dynamic exits complete it:
  `SignalFade` (conviction decayed below `signalFadeFraction` while the trade is not
  paying) and `GiveBack` (handed back `giveBackFraction` of the persisted `peakNet`).
  Entries price the whole round trip (`paperEntryEconomics`, refusal `cost-vs-edge`).
- The bot LEARNS from its record (REQ-F-033, `domain/BotNet`): every close appends a
  labelled example to `botsim-experience.jsonl` (label = NET after costs), and
  `trainBotNet` fits a one-hidden-layer network IN C++ — deliberately, because the
  target machine (a Pi left running for weeks) may have no Python;
  `tools/train_bot_net.py` is the optional desktop twin and must keep writing the
  IDENTICAL file (TS-NET-004 pins that contract, TS-NET-005 the in-app trainer).
  Three rules are load-bearing: the validation split is by TIME (a random split
  leaks the future and the AUC becomes fiction), inputs are matched BY NAME against
  the model's own feature list (`entryFeatureNames` is append-only), and an
  untrusted model (< `minSamples`, or AUC < `minAuc`) NEVER refuses a trade — it
  only annotates. Retrains itself every `kRetrainEvery` closes, off the GUI thread.
- Risk is aggregated by CORRELATION, not by count (REQ-F-031): `correlationGroup`
  buckets a symbol (equity-index / fx / metals / commodity, own bucket when
  unknown) from the catalog group plus documented exceptions (USDOLLAR is FX, not
  an index), `BookState::riskByGroup` sums loss-at-stop per bucket, and
  `maxGroupRiskFraction` (8%, deliberately below the 20% portfolio budget) refuses
  with `group-risk` naming the bucket. Do not "simplify" this back to one pool:
  without it the portfolio budget is satisfied by a dozen positions that share one
  outcome. The scan line prints where the risk sits (`by view: …`).
- The bot trades BOTH sides (REQ-F-031): shorts get mirrored stop/target geometry,
  identical sizing and the sell-side rollover, and the record attributes net to
  longs and shorts separately.
- The daily target (REQ-F-031, default 350 €) is a STOPPING rule, and the whole
  point is that it cannot become a chase: reaching it stops opening for the day,
  reaching the loss limit stops opening for the day, and size NEVER grows after a
  loss (stake is a fraction of current equity). Only BOOKED net counts towards
  either — open profit can still turn, which is why `paperHarvestPick` books the
  day by closing the SMALLEST open winner that already covers the target (the
  truncated upside is a known, documented cost; `harvestForDailyTarget` switches it
  off). `paperDayGate` runs FIRST in
  `paperEntryVerdict` (codes `day-target`/`day-loss`/`weekend`); positions already
  open keep being governed by their own stops/targets/carry rules.
- Real money stays gated on MEASURED evidence, not impression: `paperPerformance`
  computes the record (net, net/day, rolling few-day net, profit factor,
  expectancy, win rate, peak-to-trough drawdown, target hit rate, long/short
  split) and `paperLiveReadiness` reports readiness or EVERY unmet threshold. Live
  execution is not wired; wiring it needs a REQ-N-005 carve-out (the REQ-F-028
  arm-instead-of-double-press precedent), the gate passing, and per-order/daily
  caps — never a silent removal of the safeguard. The window states the verdict
  and its blockers at all times; do not let it claim more than the record shows.
- The bot's proposal source can be a LOCAL model (REQ-F-030,
  `services/OllamaAdvisor` + the pure `paperAiGate`): optional, no key,
  `./setup.sh ollama` installs runtime + model under `~/.local/ollama`,
  `ollamaModel`/`OLLAMA_MODEL` configures it, `TRADINGAPP_BOT_AI=off|confirm|lead`
  picks the mode. Three things are load-bearing and easy to "tidy" wrongly:
  (1) the response parse is DEFENSIVE because small models answer sloppily —
  measured on qwen2.5:1.5b: `"symbol":"SPX500 composite"`, `rationality` for
  `rationale`, `"high"` for a number, and — asked for a LIST — `{"picks":{"SPX500":
  {...}}}`, a symbol-KEYED map instead of an array; hence `matchProposalSymbol` (one
  unambiguous match or nothing), the word/`x3`/`0.62` normalisations and the shape
  dispatcher in `picksFrom` (array / keyed map / alternative key / single object).
  Every one of those shapes is pinned by TS-OLLAMA-007 — a mis-parse is a SILENT
  no-trade, which is the worst failure this feature can have. (2) A proposal is
  judged by AGE (< one scan cycle), NOT by whether a newer scan overtook it —
  discarding overtaken answers silently disables the feature, since a CPU model is
  regularly overtaken. (3) The model supplies DIRECTION only: it can never exceed
  the stake/exposure/leverage/ruin limits, and `paperLeverageWithAi` honours a
  model's caution but never its ambition.
- Monte-Carlo/plan building stay off the GUI thread (QtConcurrent); the
  positions table stays model/view, allocation-free per tick (REQ-N-006).
- ONE Axivion run at a time (flock in `axivion/start_analysis.sh`); no
  clean/build while it runs. External findings import: `axivion/external_import.py`
  (Python layer — matchlist is not expressible in the JSON configs).
- SonarCloud is INFORMATIONAL, never a gate: its default gate fails on hotspot
  categories only a human can rule on (deterministic PRNG for reproducible
  training, plain HTTP to a localhost model server, unpinned action versions). The
  README badge shows its issue COUNT, not `alert_status`. Do not wire it into
  build_all or CI as a pass/fail.
- Coverity Scan runs on its weekly cron or `gh workflow run coverity.yml` only.
  Do NOT add a push trigger: the free tier's weekly submission cap plus a
  shared analysis queue (~188 builds deep) make per-push builds pure waste.
- Stage exit code 3 = "skipped" (both build_all runners report it and stay
  green; any other non-zero code is a real failure). Exit 3 exists in
  `axivion/start_analysis.{sh,ps1}`, `tools/coverage.{sh,ps1}` (Coco /
  OpenCppCoverage / LLVM-mcdc) and `tools/make_docs.ps1` (doxygen). On Linux the
  MC/DC and TSan steps resolve the clang version (>= 18) via `llvm_suffix` and
  report `skipped` when the host has none — `coverage.sh auto` still exits 0
  because gcov measured something real; `coverage.sh mcdc` alone exits 3.
  Every OPEN-SOURCE tool the pipeline needs must be installable by setup.sh /
  setup.ps1 — if you add a tool dependency, add it there too.
- No machine-specific absolute paths in committed scripts or Axivion configs.
  The Qt dir for the Frameworks-QtSupport rule comes from `$(AXIVION_QTDIR=)`,
  exported by both start_analysis scripts.
- `.mcp.json` expands `${VAR}` / `${VAR:-default}` — NOT `$(VAR)`. `$(VAR)` is
  Axivion's own config syntax; Claude Code passes it through literally with no
  warning and the server dies with a bare "cannot find the path specified".
  Its three variables come from `tools/mcp_env.{sh,ps1}` (the venv interpreter
  is `bin/python` vs `Scripts\python.exe`, and JSON interpolation cannot branch
  on the platform), so the JSON stays byte-identical on both. Details:
  docs/tools.md. `MCP_TIMEOUT` in `.claude/settings.json` is load-bearing: both
  servers need 37-53 s to start cold, well past the 30 s default.
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
- MSVC ASan needs `/fsanitize=address` on the LINK line too, or `operator
  new`/`delete` never bind to the ASan runtime and Windows kills the process
  before `main()` (`entry point ??3@YAXPEAX_K@Z not located`, exit
  `0xC0000139`) — as a MODAL dialog, so the run hangs rather than fails.
- `$env:LIB` MUST NOT leak between build_all stages. MSVC and LLVM both ship a
  `clang_rt.asan_dynamic-x86_64.lib`; they are ABI-incompatible (MSVC exe imports
  `__asan_delete`, LLVM exe imports mangled `??3@YAXPEAX_K@Z`) while the DLL that
  loads is always MSVC's. `coverage.ps1` puts LLVM's compiler-rt dir on `LIB` for
  `clang_rt.profile-x86_64.lib`, that dir ALSO holds LLVM's ASan implib, and
  build_all runs `coverage` before `sanitize` in ONE process — which silently made
  all 13 ASan test exes unstartable (`0xC0000139`). Fixed on both sides: coverage
  restores `LIB`, and `Invoke-Asan` strips `\lib\clang\` dirs for its own build.
  Keep both. Symptom-to-cause: `0xC0000135` = ASan runtime missing (staged copy
  gone), `0xC0000139` = runtime present but linked against the wrong ASan implib.
- The ASan runtime is COPIED next to the build-san binaries on purpose (own
  directory beats PATH) so the suite runs outside a developer prompt too.
- Debug-config flags beat config-agnostic ones: `CMAKE_EXE_LINKER_FLAGS_DEBUG`
  (default `/debug /INCREMENTAL`) is emitted AFTER `CMAKE_EXE_LINKER_FLAGS`, so
  an `/INCREMENTAL:NO` put in the latter is silently undone. Override the `_DEBUG`
  variable. Likewise CMake 4.0 moved `/RTC1` into `CMAKE_MSVC_RUNTIME_CHECKS`
  (CMP0197), so overriding `CMAKE_CXX_FLAGS_DEBUG` no longer removes it. Both are
  hygiene, NOT the cause of the `??3@YAXPEAX_K@Z` failure — don't re-diagnose it
  as incremental linking.
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
