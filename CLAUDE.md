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
tools/publish_release.sh       # release: REFUSES unless the evidence is there
                               # (tests green, 7 analyzers at 0, ratchet clean,
                               # 0 hard gaps, PDF newer than the sources), then
                               # attaches binaries + docs + qualification bundle
tools/make_test_report.sh      # the FINAL report, reproducibly: tests -> Squish GUI ->
                               # GUI coverage (Coco, SEPARATE from the unit figure) ->
                               # Test Center upload -> the PDF, in that order. build_all's
                               # default run omits the licence-bound stages, so its PDF
                               # reports them as absent even where a licence exists
tools/axivion_report.sh        # downloads/TradingApp-axivion-report.pdf — the Axivion
                               # findings as their OWN PDF, via AXIVION'S delivered report
                               # module (bin/report_runner + example/reports/
                               # report_misra_pdf.py). Never reimplement that document.
                               # Run AFTER the axivion stage or it reports the PREVIOUS
                               # analysis — it prints the version so staleness is visible.
                               # NOTE: --noninteractive goes BEFORE the subcommand
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
build/TradingBot               # the CONSOLE front end for examining the bot (REQ-F-029):
                               # static ANSI TUI, LLM-lead by default, links the SAME
                               # BotSimRunner as the GUIs (split out of BotSimPanel).
                               # P/L+invested header, SPX500/NSDQ100 heavyweight bars,
                               # open/closed tables, keyboard-scrollable decision log
build/TradingCockpit           # the SECOND front end: Qt Quick + Qt Graphs, read-only,
                               # QCustomSeries candlesticks. Same domain/services/view-model
                               # as TradingApp; separate binary because Charts and Graphs
                               # cannot coexist (see below). TRADINGAPP_SHOT grabs it too
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
`/add-requirement` (requirements-as-code workflow), `/perf-check` (benchmarks),
`/clean-build-release` (clean_all -> full build_all -> commit, push, tag,
publish_release; refuses to publish on a red pipeline).

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
- PMD CPD (≥ 100 tokens) is the only clone GATE. Fix clones by extracting a helper;
  do not baseline them. But "the Axivion configuration here is MISRA-only" was WRONG
  and is corrected: `C++CloneDetection` is `_active` in `axivion/rule_config.json`,
  Axivion runs its own clone check at its own threshold, and it currently reports
  **2 clones** (`Metric.Violations.Clone`). Those are INFORMATIONAL — PMD CPD at ≥ 100
  tokens is what gates, and it is 0. `tools/make_report.py` used to print "Axivion's own
  clone check is off" while colouring the row as a hard gate, so a passing build showed
  red beside a note claiming the check was not running; both are fixed. The same config
  also enables 133 CWE rules, so Axivion here is MISRA C++ 2023 + CERT/CWE + architecture,
  not MISRA alone.
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
  never closes; unknown fees keep both rules silent). An ACTIVE keep from the model
  (`ExitContext::aiBacksHold`, config `aiMayOverrideCarry`, default on) waives BOTH carry
  closes so a conviction trade may ride overnight/over the weekend — but ONLY those two: the
  stop/target barrier is checked first and never waived, the rollover is still CHARGED every
  mark (hold through the rent, not escape it), and SILENCE never triggers it (the safe
  default is to close before an unearned weekend charge — the mirror of silence-never-closes). `TRADINGAPP_BOT_ARM=1` arms it at
  startup for unattended runs, and the armed flag + AI mode are PERSISTED (an
  experiment that stops silently on restart looks like a working bot finding
  nothing). How many trades it holds is governed by the PORTFOLIO RISK BUDGET
  (Σ loss-if-every-stop-hit ≤ `maxPortfolioRiskFraction` × equity), never by a
  trade count — don't reintroduce a queue limit. Every scan logs one summary line
  (candidates, opened, risk vs budget, refusals per `code`); those codes on
  `EntryVerdict`/`AiGate` are what make it countable, so keep them stable.
- Exposure is bounded TWICE and the smaller wins: `maxExposureFraction` (0.75 of
  CURRENT equity, which guarantees free margin at any account size) and
  `maxInvestedEur` (an ABSOLUTE 15000 EUR ceiling on Σ stakes; 0 = off). Both are
  needed because a fraction of current equity is not a fixed amount of money —
  0.30 × 50k is 15k only while equity is exactly 50k, and it drifts with the very
  P&L it is meant to bound. NOTE THE CONSEQUENCE, new in 1.0.3: at the default 50k
  equity and the 6% stake the EUR ceiling binds FIRST — 5 concurrent positions,
  where 0.75 × 50k allowed ~12 — so the binding constraint is no longer the risk
  budget that `maxPortfolioRiskFraction`'s own comment describes. That is
  deliberate and requested, but it means a refusal reading `invested-cap` is
  normal rather than a defect. It is reported as `invested-cap` and NEVER as
  `margin-cap`, because the two send a reader to change different numbers. Tests
  that are about the risk budget, the margin cap or the cash rule set
  `maxInvestedEur = 0` to isolate what they measure (TS-PAPER-008/012/014);
  TS-PAPER-031 owns the ceiling itself.
- The bot TRADES ONLY its FOCUS SET (`BotConfig::focusSymbols`, default SPX500 + NSDQ100):
  anything else is refused before every other check with code `not-focus`, and only focus
  instruments are shown to the model. Measured on the ledger this removes the two failure
  modes that dominated it — the model spending its one answer on a peripheral name
  (`ai-other-pick`, 201 of 673 refusals) and the peripheral names it lost -197 EUR on
  (OIL.24-7, USDOLLAR). Empty set = whole catalog (the tests that isolate the risk rules set
  it empty).
- LEAD mode FALLS BACK to the composite by default (`aiLeadFallbackToComposite`), and this is
  what makes the bot trade: 442 of 673 ledger refusals were the 1.5B model simply not
  answering (`ai-none`) while the composite held a direction. When the model gives no usable
  answer for a focus instrument — no proposal, unparsable, or it named the other focus name —
  the COMPOSITE leads (`paperAiGate`'s `leadFallback`). The model still leads WHEN IT SPEAKS;
  an explicit HOLD is NEVER overridden (a HOLD is an opinion); a neutral composite opens
  nothing (the fallback carries a direction, never invents one). Verified live: NSDQ100 went
  from `REFUSED no side · ai-other-pick` to `REFUSED long · weekend · strength 48` — the
  composite now leads and only the genuine weekend closure stops the trade.
- The bot's DEFAULT decision source is the local model in LEAD mode
  (`BotConfig::aiMode`), bounded by every risk rule; a book saved earlier keeps the
  mode it was left in, because an upgrade must not change what a running experiment
  measures. Opening a position raises a NON-MODAL notice, at most one on screen
  (`MainWindow::onBotTradeOpened`) — a modal box would stop the marking/exit timers,
  and one scan can open a dozen trades.
- Prediction rests on AGREEMENT BETWEEN INDEPENDENT reads (REQ-F-035,
  `domain/IndexConfluence`): NINE of them — futures leadership, the leading future's
  1/5/15-minute push (ONE read, because three horizons off one series are one piece of
  evidence in three hats; disagreeing horizons are neutral), volatility DIRECTION (^VXN
  for Nasdaq, ^VIX otherwise), the US 10-year, the CURVE (2YY=F else ^IRX against ^TNX —
  the read names which it got), heavyweight participation, how many heavyweights trade
  above their OWN session VWAP, whether the session's VOLUME is behind the up names or
  the down ones, and the opening range. `MarketFeeds::fetchReferenceSeries` fetches the
  nineteen tickers. The two volume reads need `VolumeSeries` — closes and volumes ALIGNED
  bar for bar, parsed together in `yahooBars`, because the two arrays skip different empty
  minutes and a VWAP across that shift is a number about nothing; a feed with no volume
  (the vol/yield indices) leaves them UNKNOWN, never zero. `termStructure` (^VIX9D vs
  ^VIX3M) is a REGIME damper and never a direction.
  Two invariants: an unmeasurable read is UNKNOWN and NEVER counts as agreement
  (a "4 of 5" built from absent feeds is a lie), and heavyweight participation is
  labelled a STAND-IN for breadth — real breadth needs per-constituent data this app
  does not fetch. Order flow (volume delta, CVD, bid/ask imbalance) is NOT available at
  all: it needs CME level-2, and eToro gives one bid/ask with no sizes.
  THE BOOKS ARE KEYED DIFFERENTLY AND A READ MUST NEVER SEARCH THE WRONG ONE — the
  references by Yahoo TICKER, the futures proxies by APP SYMBOL. `ReadInputs` +
  `readInputsFor` exist to make that unrepresentable: the futures-lead read once looked
  for `NSDQ100.24-7` in the ticker book, was therefore permanently UNKNOWN in the running
  app, and its unit test passed because the test filed the futures in the book the read
  was searching (TS-CONF-006 is the regression).
  The bot refuses below a MAJORITY of the measured reads, floored at `minAgreeingReads`
  (3) and clamped to what is available (`no-confluence`), 0 switching it off. The majority
  rule is load-bearing: an absolute 3 was a majority of five reads and a MINORITY of nine,
  so every read added silently weakened the gate (TS-PAPER-025 pins it).
- A probability is MEASURED, never asserted (REQ-F-037, `domain/PredictionLedger`).
  The 0..100 strength is EVIDENCE; P(up, 5/15/60/180 min) comes only from the record.
  EVERY evaluation is appended to `prediction-ledger.jsonl`, including the ones that
  STAYED OUT with their refusal code — a record of executed trades measures the gate in
  front of the signal, not the signal. Outcomes resolve by PAIRING the ledger's own later
  rows for the same instrument (so no second store can disagree), refusing a pairing too
  far past the horizon (an overnight gap is not a 5-minute outcome) and taking the
  EARLIEST qualifying row (the latest would silently lengthen every horizon). Below
  `kMinSamplesPerBucket` the answer is UNCALIBRATED with its sample count and NO number —
  the `paperLiveReadiness` discipline. Every score sits beside baselines on identical
  samples (always-long, prior 5-min move, VWAP side) plus a Brier score against 0.25, and
  an UNMEASURABLE baseline is named rather than scored 0% and counted as beaten.
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
- BOTH front ends can trade, and REQ-F-038's old "read-only by construction" clause is
  SUPERSEDED — that was a scoping decision recorded as a safety property. The safety
  property is the SAFEGUARD: `domain/ConfirmGate` (`confirmPress`) is the ONE REQ-N-005
  double-press rule, called by `MainWindow::handleQuickKey` AND `CockpitModel::press`.
  Never write a second copy — a gate with two implementations is the weaker of the two.
  The armed action names the WHOLE order ("BUY 500.00 at x5"), so editing amount/leverage/
  side disarms and a close confirmation is keyed by position id. `EtoroClient` stays the
  single order-capable object per process; the view-model only emits "a human authorised
  this" and never holds a broker. A view's claim about its own safety must be true of the
  build making it — the cockpit banner used to say "no order can be placed from here" and
  had to change when the ticket landed.
- A CONFIRMED-CLOSED position leaves the open-trades table IMMEDIATELY and is then HIDDEN
  from the portfolio poll for `kClosedSuppressMs` (30 s), because that endpoint lags its own
  truth — without the suppression the row deleted on the close reply reappears on the next
  poll, and a row that vanishes and returns reads as a close that FAILED. The hiding is
  bounded on purpose: still reported after the window means the close did NOT take, so the
  row comes back and is NAMED. Hiding it forever would tell someone they are flat while the
  risk is open. `trading::suppressClosedPositions` owns the rule; `positionClosed` carries
  the position id precisely so the window need not parse it out of a message string.
- QT CHARTS AND QT GRAPHS CANNOT SHARE A PROCESS. They declare seventeen classes with
  identical names in one namespace (QValueAxis, QAbstractAxis, QLineSeries, QBarSeries,
  QPieSeries, QXYSeries…), so linking both makes qmltyperegistrar ambiguous and EVERY
  Qt Graphs QML type stops resolving — "ValueAxis is not a type", the component is
  unavailable, and the cockpit renders as a blank white rectangle with no other symptom.
  Measured with a two-target probe: identical source, only the link line differs;
  instantiation order changes nothing. Hence TWO binaries over one `trading_cockpit`
  library: `TradingApp` (Widgets + Qt Charts, cockpit panel with the chart degraded to a
  stated note via a `Loader`) and `TradingCockpit` (Qt Quick + Qt Graphs, no Charts, the
  real QCustomSeries candlesticks). `find_package(Qt6 COMPONENTS Graphs)` is deliberately
  NOT followed by a link — the module loads as a QML plugin at run time. A static
  `qt_add_qml_module` also needs its plugin target named on the link line
  (`trading_cockpitplugin`); `qt_import_qml_plugins` does not help, it scans the target's
  OWN QML files and both executables have none.
- The candlestick chart encodes direction by FILL first (hollow up / solid down) and colour
  second, and `kMaxDrawnCandles` (120) is part of that guarantee, not a performance knob: a
  full 339-bar session leaves each body ~3 px, which a 1 px border fills completely, so
  every candle reads as solid and the fill channel carries nothing. Truncation is STATED
  ("last 120 of 344 one-minute bars"). `domain/Candles` drops a bar whole unless all four
  values are present, positive and consistent — the four Yahoo arrays have INDEPENDENT gaps,
  the same trap `yahooBars` exists for — and fits the axis to the WICKS, never the bodies.
- Monte-Carlo/plan building stay off the GUI thread (QtConcurrent); the
  positions table stays model/view, allocation-free per tick (REQ-N-006).
- ONE Axivion run at a time (flock in `axivion/start_analysis.sh`); no
  clean/build while it runs. External findings import: `axivion/external_import.py`
  (Python layer — matchlist is not expressible in the JSON configs).
- The Axivion stage takes ~30 min, and it is 30 rather than 94 because **CWE-464 and
  CWE-789 are `_active: false`** in `axivion/rule_config.json` with the measurements in
  their `".#"` comments. Do not re-enable them casually: parallelism is already maxed
  (12 workers via `axivion_ci --jobs`), and CWE-464 alone was a 41-minute CRITICAL PATH
  — no worker count beats the slowest single rule. Those two were 71% of rule time while
  every MisraC++2023 rule COMBINED takes 209 s, and removing them left the finding count
  essentially unchanged. The four rules re-enabled on request on 2026-07-27
  (CWE-20/200/502/79 + StaticSemanticAnalysis) stay ON — this is narrower than that
  experiment. Comment keys inside rule entries must be `".#"`; a bare `"#"` fails the
  Suite's config validator.
- `tools/gates_to_junit.py` and `tools/publish_release.sh` must judge artefact staleness
  against the SAME yardstick — `git ls-files 'src/*' 'tests/*' 'CMakeLists.txt'
  'tests/CMakeLists.txt'`, i.e. SOURCES ONLY. A bare `git ls-files` includes tracked-but-
  GENERATED files, and `make_test_report.sh` regenerates `docs/requirements.md` from the
  .sdoc *after* the analyzers run — which made the chain declare eight analyzer artefacts
  stale, invalidating evidence it had just collected. Two tools disagreeing about whether
  the same evidence is stale is worse than either rule alone.
- Artefacts are not all findings-only. `check_object_names.py` prints a human-readable
  SUCCESS sentence into `analysis-results/object-names.txt`, so counting non-empty lines
  reported "1 finding" for a CLEAN check — a permanent false red. `static_analysis.sh`
  judges it by exit code and the `has no objectName` marker; `gates_to_junit.py`'s
  `FINDING_MARKERS` mirrors that. Any new artefact carrying prose needs an entry there.
- SonarCloud is INFORMATIONAL, never a gate: its default gate fails on hotspot
  categories only a human can rule on (deterministic PRNG for reproducible
  training, plain HTTP to a localhost model server, unpinned action versions). The
  README badge shows its issue COUNT, not `alert_status`. Do not wire it into
  build_all or CI as a pass/fail.
- Coverity Scan runs on its weekly cron or `gh workflow run coverity.yml` only.
  Do NOT add a push trigger: the free tier's weekly submission cap plus a
  shared analysis queue (~188 builds deep) make per-push builds pure waste.
- Publishing goes through `tools/publish_release.{sh,ps1}` and NOWHERE else: it
  re-checks the evidence (tests, the seven analyzers at zero, the metrics ratchet,
  0 hard gaps) and that every artefact is NEWER than the newest tracked source,
  then attaches the binaries for THAT version plus the docs and qualification
  bundles. Artefacts named for another version are skipped and named — the release
  workflow rebuilds all four platforms on the tag. The qualification bundle is the
  one CI cannot produce alone: its PDF carries the Axivion result.
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
- ANY Rust component (the Qt Bridges risk-engine experiment) is checked by BOTH
  Axivion and Rust's own checker — neither alone. Verified 2026-08-07 against Suite
  7.12.3 and its documentation:
  * Axivion DOES support Rust (C, C++, CUDA C++, C#, Rust). This install has the
    frontend (`lib/scripts/_rust2rfg.abi3.so`, `example/projectconfig/rust`) and the
    machine has cargo/rustc/rustup. Enable `AxivionRustFrontend` under
    `BuildSystemIntegration` with `manifest_path` pointing at the crate's `Cargo.toml`.
    Rust analysis is CARGO-driven, not IR-based — `cafeCC` is not used — so it needs a
    buildable `cargo build` and network access for dependencies.
  * `RustClippyIntegration` (also a `BuildSystemIntegration` action, same
    `manifest_path`) imports Clippy diagnostics onto the same dashboard, so Rust and
    C++ findings sit together. That is the "Rust internal checker" half; the gate is
    `cargo clippy -- -D warnings`.
  * THE REASON THIS MATTERS MORE THAN USUAL: the Rust RFG MERGES with the C/C++ RFG,
    and `Rust-CheckExternSignatures` then detects FFI signature mismatches between Rust
    `extern` declarations and their C/C++ implementations — parameter count, parameter
    types, return type. That is exactly the failure mode of a Qt/Rust bridge, and the
    class of bug that otherwise surfaces as a corrupted stack at runtime.
  * Do NOT oversell it: MISRA for Rust is only MisraC2012Directive-4.2 and 4.3 (both
    about assembly), so this is not a MISRA story. The Axivion setup wizard does not
    support Rust — the config is written by hand.
  * When the crate lands, `setup.sh`/`setup.ps1` must install the Rust toolchain too
    (rustup + `rustup component add rust-src`, which the Rust frontend needs for std
    sources), because every open-source tool the pipeline needs is installable by setup.
  * Configure this only once a real `Cargo.toml` exists: `AxivionRustFrontend` pointed
    at a missing manifest just fails the stage.

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
