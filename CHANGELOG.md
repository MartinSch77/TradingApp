# Changelog

Application-level release history (`vX.Y.Z` tags). See `process/CHANGELOG.md`
for the separately-versioned process framework's own history
(`process-vX.Y.Z` tags) — the two are independent baselines
(`process/strategies/configuration-management-strategy.md`).

Filed to close GitHub Issue #16 (QA nonconformance: root CHANGELOG.md
absent) — `docs/roadmap.md` remains the forward-looking plan; this file is
the realized-history counterpart `process/strategies/project-management-
strategy.md`'s planning-artefact composition names.

## v1.2.0

Nine paper-bot corrections, every one measured on the running bot or verified in
the code before it was changed, plus the documentation that had drifted from it.
Real-money execution stays excluded from those nine (REQ-N-005); the separate
item at the end of this list wires it, deliberately, behind REQ-N-009's path.

- Real money (REQ-F-076, owner request 2026-09-30): the paper bot's SPX500 and
  NSDQ100 decisions can be mirrored to real eToro orders from the bot window's
  new "Real money" box — a REQ-N-005 double-pressed, time-bounded arming that
  names its grant (scope, 250 EUR per order, 250 EUR realised daily loss, 480
  min; `botLiveMaxPerOrderEur`/`botLiveDailyLossEur` in Config, 0 = nothing may
  be sent). Every order goes through REQ-N-009's validated-armed-recorded path
  (`EtoroOrderGateway`, the real seam, refusing a non-live client on its own;
  audit `bot-live-orders.jsonl`); the stake is min(cap, paper stake) at the paper
  leverage with the stop/target amounts scaled to it (`domain/LiveMirror`), one
  live position per instrument, the paper close closes the live position, and a
  realised daily loss at the cap trips the STICKY kill switch. Arming fails closed
  on the kill switch, a non-owned book and a non-live client; the REQ-F-031
  readiness verdict is shown and logged at arming but is NOT a precondition —
  the owner's decision, taken in REQ-F-031 itself (RISK-005 raised to 12). `EtoroClient` prices a market order on
  an instrument that is not on screen off that instrument's own rate (the old
  "only the instrument currently being traded" refusal is gone) and reports the
  opened position id as a signal. GUI only; the console binaries compose no
  gateway. TS-LIVE-001..004, TS-LBOT-001..005, TS-GATE-007, TS-CLI-042.

- Rollover: `paperRolloverNights` billed a weekend as 5 nights (Sat 3 + Sun 1 +
  Mon 1). eToro charges it ONCE, tripled, on the Friday night — Saturday and
  Sunday boundaries now count 0, so Fri→Mon is 3; `paperCostToHold` and the
  entry economics follow (TS-PAPER-002 extended).
- Entry gate: a Friday open on a non-24/7 instrument with a known positive fee
  table was closed as `WeekendCarry` on its FIRST mark, a pure spread round trip
  every Friday. `paperEntryVerdict` now refuses it `weekend-carry-ahead` right
  after the day gate, priced by the same arithmetic the exit uses, silent
  exactly when the exit rule is (TS-PAPER-044).
- Quote age: a per-tick quote that stopped updating still counted as live.
  `trading::quoteIsFresh` (one age rule, the open-trades table's own 120 s
  bound) decides `Sides::live` and `mark.live`; a stale quote still prices the
  candidate off the bulk mid but is refused `no-live-quote`. The unstamped
  bulk-mid and candle paths stay live and SAY so. New `tst_botsimrunner` target
  (TS-BOTSIM-001, TS-PAPER-045).
- Day-target harvest: no longer cuts a swing-strategy position to bank the
  composite bot's day, and `paperHarvestPick` judges the ledger against `now`
  — before the first close of a new date it read yesterday's realized and
  harvested for a target the day had not earned (TS-PT-031 extended).
- Experience log: a swing 2R partial never reached `recordExperience`, so the
  one example written at the final close carried the remainder's net alone (a
  trade that banked half at +2R and trailed out taught a LOSS). A partial now
  banks its net and the final close writes ONE example labelled with the whole
  trade's net (TS-PAPER-046 pins the proration identity).
- Ledger scoring and saving: `resolveAll` copied the sorted tail per row and
  scanned past the horizon's bound; it now walks a span and stops at the first
  row of any instrument beyond the limit, identical results (TS-LEDGER-007).
  The runner caches the ledger instead of re-reading the growing file every
  scan (TS-BOTSIM-002), and pure mark moves save `botsim.json` at most once a
  minute instead of every 5-second tick (~17 000 rewrites a day).
- One process per book: a `QLockFile` beside the store. The GUI and the console
  share one book by design and could both run the bot on it; the second runner
  now loads and shows the book read-only, refuses to arm naming the holder,
  and never marks, enters or saves (`ownsBook()`, TS-BOTSIM-003). The lock's
  age rule is off: QLockFile's default took a live holder's lock once its file
  was 30 s old, and a book is held for weeks (TS-BOTSIM-009).
- Session structure: the runner computed `openingRange` and the ledger's
  prior-move baseline from the eToro scan's HOURLY closes (a 30-hour "opening
  range"). Both now read the 1-minute series the window shows; the hourly
  closes remain the volatility source only. Ledger rows carry
  `strategyVersion` `composite-v2` from the switch so the two baselines are
  scored apart (TS-BOTSIM-004).
- Crypto: no crypto position ever opened — the eToro scan queues only resolved
  ids (crypto never resolves one) and `tryOpen` refused every id-less
  candidate. `MarketFeeds::fetchCryptoScreenerRows` supplies hourly Yahoo rows
  (`ScreenerRow::fromFallbackFeed`), `trading::mergeScreenerRow` is the ONE
  venue-beats-fallback rule for both front ends, and an id-less crypto
  candidate opens off its candle close (TS-FEED-016, TS-DEC-011, TS-BOTSIM-005).
  The console front ends scan the coins; the desktop GUI stays crypto-free at
  the source, a separate decision.
- clang-tidy newer than CI's 18 (a developer machine on the 24 snapshot reported
  1251 findings against a green CI): the wildcard check families switch on every
  check a new release adds. Measured on 23.1.2: five churn checks are now disabled
  in `.clang-tidy` with their counts (designated initializers 449, trailing commas
  329, unchecked `operator[]` 275, math parentheses 90, enum class 3) and the
  deterministic test seeds in `tests/.clang-tidy` (8); everything else was fixed
  in the code — const-correctness, internal linkage for file-local functions,
  `std::ranges` algorithms, by-value parameters, functional casts, `std::move`,
  redundant `()` on lambdas, two FinBERT fields unused without ONNX Runtime,
  `MainWindow::eventFilter` back to QObject's public visibility and the class's
  copy/move explicitly deleted. The ONNX Runtime block of `FinBertSentiment`,
  which only a machine with the runtime compiles, followed in a second pass
  (three `std::ranges` conversions). The same local run showed what CI's
  `static-analysis` job had been hiding behind `continue-on-error` since PR #32:
  six cppcheck findings (an unknown `Q_DISABLE_COPY_MOVE` macro in cppcheck 2.13's
  Qt library, now spelled out; three raw loops now `std::ranges` algorithms; a
  local `day` shadowing `LiveBotExecutor::day()`) and one PMD CPD clone in
  tst_botsimrunner (the one-symbol snapshot, now a helper) — all fixed, both
  tools at 0 again. `tools/clang_analyzer.py` no longer pins `clang++-18`: it
  takes the NEWEST `clang++-NN` on PATH (the `llvm_suffix` rule), because a pinned
  18 cannot parse a GCC 14 compile database's libstdc++ headers and reported
  every translation unit as `clang-analyzer-failed` (136 lines) on a developer
  machine whose clang 23 parses them fine — the count `publish_release.sh`
  refused on. The same stage now resolves its extra checkers against the
  compiler's own `-analyzer-checker-help` listing: clang 23 folded the two
  `valist.*` checkers into `security.VAList`, and naming a checker the compiler
  does not know is a hard error that again failed every translation unit.
- TS-CLI-041 (the paper-held delayed row repaired off its candle) raced the
  client's own first tick: the bulk rates request and the candle repair go out
  together, and when the candle reply lands first the repair knows no spread yet,
  so the ask reads 61100 for one tick until the row lends it its 10.0 — the
  slower ASan build hit that window and failed the release's sanitize stage. The
  test now pins the candle-first order with the mock's `holdUntil` and waits for
  the converged quote; the client itself is unchanged (it converges within a tick).
- Release build on the ubuntu-22.04 AppImage runner: GCC 11 rejects
  `[[maybe_unused]]` on a data member (`-Werror=attributes`), which the two FinBERT
  index fields had carried since the clang-tidy cleanup; the stub build's `load()`
  now references them instead, and the attribute is gone.
- The network-facing tests' 15 s spy-wait bound is one shared `tests/TestWait.h`
  and scales with `TRADINGAPP_TEST_WAIT_SCALE`, which the sanitize stage exports
  (6) for its valgrind pass — memcheck on a loaded machine ran TS-CLI-005 and
  TS-CLI-041 into the unscaled bound.
- Documentation: `docs/bot-decision-pipeline.md` §8 no longer claims the swing
  strategy is outside the live loop (it is wired behind `useSwingStrategy`,
  off by default, unvalidated live); CLAUDE.md names REQ-F-059..-075 for the
  reads and TS-PAPER-027 as the test that pins the confluence majority rule.

## v1.1.2

The first release since v1.0.6. `v1.1.0` and `v1.1.1` were tagged but never
published: v1.1.0's packaging died because Qt Graphs' CMake package needs
`qtquick3d` + `qtshadertools`, which no install step requested (fixed in
`f81d17d`), and v1.1.1's release gate then stopped on one cppcheck finding
that only an ONNX-Runtime-less build — the CI configuration — produces.
Everything below the first five bullets landed in those unpublished tags.

- Release gate: `FinBertSentiment`'s stub `scoreText` no longer trips
  cppcheck's `functionStatic` on builds without ONNX Runtime.
- Traceability: a `STATUS: superseded` requirement is traced THROUGH its
  successors (`tools/trace_report.py`, `tools/sdoc_to_md.py`) instead of
  being reported as untested; REQ-F-057 gained its dedicated test
  (TS-PM-006); the traceability-gate workflow, red on `main` since it was
  added, passes (the process model now names the human final approver).
- GitHub issue #14 closed: after REQ-F-034 (PR #21, REQ-F-049..058),
  REQ-F-035 is split too — REQ-F-059..075, nine independent reads, the
  agreement gate, five cross-cutting rules and two displays, every test
  re-tagged after reading it. The split showed two on-screen labelling
  clauses unmet, now fixed: the participation read calls itself a stand-in
  for breadth (REQ-F-064), and every display of the cap-weighted lead says
  its weights are "approx. static weights" (REQ-F-075).
- Issue #15 (Coco GUI coverage under Squish) re-scored as an accepted,
  monitored low risk — `process/risk-register.md` RISK-001 is the live
  record.
- CI actions: `actions/checkout` 7, `actions/setup-java` 6.0.0,
  `fsfe/reuse-action` 6 (Dependabot #25/#26/#27).
- Qt Graphs resolves in every CI and release job (qtquick3d/qtshadertools);
  the release gate's timeout is 90 minutes.
- ICA added as a second, independent clang-based analyzer beside Axivion
  (informational); Python unit tests with branch coverage for
  `tools/*.py` and `tools/ml/*.py`.
- Issue #20 (JsonHttp TSan race) fixed with `Qt::SingleShotConnection`;
  every `test-results/*.xml` consumer now reads the nested Squish results.
- Bot strategy redesign (7/N): `SwingPullbackStrategyV1` wired into
  `BotSimRunner` live — additive, off by default (`BotConfig::
  useSwingStrategy`); new `PaperTrader::paperStakeCeiling` extracted so the
  swing entry path shares the composite bot's own portfolio/margin/
  correlation budget rather than a second copy of it.
- REQ-F-004 (exposure-cap guard) and REQ-N-002 (pure domain layer,
  `tst_architecture.cpp`) closed the project's last two automated-test
  coverage gaps.
- Mull mutation-testing pilot backlog closed for `PositionMath.cpp`,
  `Money.cpp`, `ConfirmGate.cpp`.
- Added an ASPICE-mapped process framework (`process/`, `process-v0.1.0`):
  independent SUP.1 Quality Assurance distinct from engineering
  verification, 16 process specs, `tools/qa_report.py`, GitHub issue/PR
  template operationalization.
- `requirements/requirements.sdoc`'s schema extended with SOURCE/RATIONALE/
  PRIORITY/STATUS/ACCEPTANCE_CRITERIA/ISSUE fields.

## v1.0.6

Release-pipeline hardening: fixed analysis-stage failures found by a full
`build_all.sh` run, an optional CBMC proof for `priceDecimals`'s pure core,
two more libFuzzer targets (Ollama/Yahoo response parsers extracted to
domain), an AppImage reproducibility check, the OpenSSF Scorecard workflow
and README architecture diagram.

## v1.0.5

Release gates now cover the console front ends (analysis/test/sanitize
green); the bot defaults to trading SPX500/NSDQ100, with peripheral
instruments needing more conviction; `TradingAdvise`/`TradingPortfolioAdvise`
skip Android packaging like `TradingBot` already did.

## v1.0.4

Crowd-sentiment/AI feature branch merged; packaging fixes for what that work
changed (no Mimer in the AppImage, no console APK); the console detects it
is a POSIX-terminal program rather than failing on MSVC; decision-log
timestamps are UTC-stable across machine timezones.

## v1.0.3

GPL-3.0-or-later relicensing; REQ-F-035's nine independent confluence reads;
a README a first-time visitor can actually read; MC/DC coverage taken from
77.0% to ~88% across several rounds, each finding at least one real defect
along the way; Squish and Test Center wired for real; an early read on index
heavyweights.

## v1.0.2

Money counted exactly (`domain::Money`, REQ-N-008) rather than approximated
in floating point; the order path validated, armed, and recorded
end-to-end.

## v1.0.1

First tagged release.
