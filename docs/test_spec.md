# TradingApp — Test Specification

@page test_spec Test Specification
@tableofcontents

Requirement-based test specification. Every test case has a stable ID
(`TS-…`), links to the requirement(s) it verifies and the design element it
exercises, and maps 1:1 to a Qt Test function of the same name in
`tests/tst_*.cpp` (tagged there with `@tstid` / `@design` plus the StrictDoc
source marker `@relation(REQ-…, scope=function)` naming the verified
requirements).
Test results are read from the JUnit XML files the suite writes to
`test-results/` (see `tools/run_tests.sh`); the traceability matrix
(`tools/trace_report.py`) joins spec ↔ implementation ↔ result ↔ design ↔
requirement and reports gaps.

Levels: **U** = unit (pure domain), **I** = integration (multiple classes /
event loop / local mock HTTP server).

## Indicators (tests/tst_indicators.cpp, DES-DOM-IND, REQ-F-005)

| ID | L | Case |
|----|---|------|
| TS-IND-001 | U | `sma` equals the arithmetic mean of the last n values; 0 when the series is shorter than n. |
| TS-IND-002 | U | `rsi` returns −1 with insufficient data; 100 for a strictly rising series; below 50 for a strictly falling one. |
| TS-IND-003 | U | `stochasticK` locates the latest close inside the n-bar range (100 at the top, 0 at the bottom). |
| TS-IND-004 | U | `macdHistogram` is positive on a sustained uptrend and negative on a downtrend. |
| TS-IND-005 | U | `bollingerPercentB`: exactly 0.5 for a flat series (degenerate band), upper half for an alternating series ending on the high leg, near the upper band (> 0.85) when a ramp ends at its own maximum. |
| TS-IND-006 | U | `volatilityPct` is 0 for a constant series and positive for a noisy one. |
| TS-IND-007 | U | `roc` equals the percent change over n bars. |
| TS-IND-008 | U | `returnsOf`/`meanReturn` produce per-bar fractional returns and their trailing mean. |
| TS-IND-009 | U | `emaSeries` is seeded with the first value and pulls toward the latest values. |

## Forecasting (tests/tst_forecasting.cpp, DES-DOM-FC, REQ-F-006)

| ID | L | Case |
|----|---|------|
| TS-FC-001 | U | `linRegForecast` on a perfect line recovers the slope with R² ≈ 1. |
| TS-FC-002 | U | `linRegForecast` on a constant series gives slope 0. |
| TS-FC-003 | U | `knnForecast` reports k used and an agree fraction within [0, 1]. |
| TS-FC-004 | U | `hurstExponent` is higher for a persistent (trending) series than for an anti-persistent (alternating) one. |
| TS-FC-005 | U | `monteCarlo` is invalid on a too-short series and valid with barriers on a sufficient one. |
| TS-FC-006 | U | `monteCarlo` on a strong uptrend: pWinLong > pWinShort; win+lose probabilities per side stay ≤ 1; p5 ≤ p95. |
| TS-FC-007 | U | `sigmoid` maps 0 → 0.5, is bounded in (0, 1) and monotone. |
| TS-FC-008 | U | `monteCarlo` reports the measured mean final move of paths expiring between the barriers (sign follows the drift; zero without barriers). |

## Signal ensemble (tests/tst_signalensemble.cpp, DES-DOM-ENS, REQ-F-007)

| ID | L | Case |
|----|---|------|
| TS-ENS-001 | U | A sustained uptrend yields signal BUY (dir +1) with confidence > 0. |
| TS-ENS-002 | U | A sustained downtrend yields signal SELL (dir −1). |
| TS-ENS-003 | U | A too-short series yields an invalid ensemble. |
| TS-ENS-004 | U | `applyVixHaircut` never increases confidence and trims more for higher VIX. |

## Decision engine (tests/tst_decisionengine.cpp, DES-DOM-DEC, REQ-F-008/-009/-022)

| ID | L | Case |
|----|---|------|
| TS-DEC-001 | U | `crowdTilt`: contrarian positive at extreme fear (≤20), contrarian negative at extreme greed (≥80), mild momentum tilt in between, bounded in [−1, 1]. |
| TS-DEC-002 | U | `newsSentimentScore` scores positive/negative keyword headlines with the right sign and neutral otherwise. |
| TS-DEC-003 | U | `marketRegime`: risk-off for VIX ≥ 25, risk-on for VIX < 16; imminent high-impact event sets the event-risk flag. |
| TS-DEC-004 | U | `computeDecisionRows` renormalises weights over available sources, applies the crowd tilt when Fear & Greed is valid, and sorts by confidence descending. |
| TS-DEC-005 | U | `buildDecisionEvidence` names the crowd reading and the actionable candidates. |
| TS-DEC-006 | U | `intradayTilt` reads the session position (rising > 0.5, falling < −0.5, flat/short = 0) and a bullish Yahoo intraday series lifts the composite (REQ-F-022). |

## Trade planner (tests/tst_tradeplan.cpp, DES-DOM-PLAN, REQ-F-010/-011/-012)

| ID | L | Case |
|----|---|------|
| TS-PLAN-001 | U | `proposedSlFraction` scales with √horizon and is clamped to [0.001, 0.5]. |
| TS-PLAN-002 | U | `recommendLeverage` picks the largest allowed step with step ≤ budget/slFrac, respecting the instrument cap and defaulting steps to {1,2,5,10,20}. |
| TS-PLAN-003 | U | `buildTradePlan` returns STAY OUT with reason "no clear directional signal" when no side is actionable. |
| TS-PLAN-004 | U | Cost bill: openCost = closeCost = invest·lev·spread%/2; a Friday "now" marks the weekend crossing and bills the weekend night once instead of an ordinary night. |
| TS-PLAN-005 | U | A plan whose costs exceed the expected gross edge is verdicted STAY OUT and the reason names the cost gate ("costs eat the expected edge"). |
| TS-PLAN-006 | U | Risk factor rises with elevated VIX, imminent events and crowd extremes, is clamped to [1, 5], and the weekend crossing is noted. |
| TS-PLAN-007 | U | Verdict gates: a forced side with weak ensemble confidence is STAY OUT ("confidence too low"); with identical seeded Monte-Carlo draws, a cost bill that leaves the net edge below 0.25% of the stake flips an actionable BUY to STAY OUT ("too thin"). |
| TS-PLAN-008 | U | Break-even gate: a forced short against a confidently rising market keeps the ensemble confidence (gate a passes) but its Monte-Carlo win rate sits below the reward:risk break-even rate — verdict STAY OUT ("break-even"), with the measured conditional win rate itself below `breakeven`. |

## Position math (tests/tst_positionmath.cpp, DES-DOM-POS, REQ-F-003/-016)

| ID | L | Case |
|----|---|------|
| TS-POS-001 | U | `priceDecimals` gives more decimals for low-priced instruments than for indices. |
| TS-POS-002 | U | `accountValuePerPoint` uses amount×leverage/openRate (FX-free) and falls back to units when the notional is unknown. |
| TS-POS-003 | U | `slTpAmountText` converts a rate distance to the account/display amount and is empty when the leg is off; `slSignedAmountText` is negative for a losing stop and positive for a stop on the winning side. |
| TS-POS-004 | U | `closedSincePreviousIds` reports the position ids that vanished between two portfolio snapshots — in shown order, several at once, none on the first snapshot (no previous set) and never an empty id. This is what triggers the immediate closed-trades refresh (REQ-F-025). |

## Positions model (tests/tst_positionsmodel.cpp, DES-UI-POSMODEL, REQ-F-012/REQ-N-006) — headless ui-model unit tests

| ID | L | Case |
|----|---|------|
| TS-PM-001 | U | An unchanged position-id set refreshes in place (dataChanged, zero model resets — editors/marks survive by construction); a changed set resets the model. |
| TS-PM-002 | U | While an SL/TP cell editor is open (`beginCellEdit`), portfolio snapshots, FX updates and SL/TP echoes emit no dataChanged for that one cell (Qt would re-fill the open editor) while every other cell refreshes and the stored value keeps updating; `endCellEdit` emits the single held-back dataChanged. |

## Event insight (tests/tst_eventinsight.cpp, DES-DOM-EVT, REQ-F-020)

| ID | L | Case |
|----|---|------|
| TS-EVT-001 | U | `parseNum` extracts leading numbers from feed strings ("0.3%", "-0.2%", "215K"). |
| TS-EVT-002 | U | `guessImpact` returns a non-empty text and a direction in {−1, 0, +1} keyed to the event type. |
| TS-EVT-003 | U | `proposeActivity` (REQ-F-023): high-impact hot CPI → SELL after the print; medium-impact stronger PMI → BUY before the release; missing forecast/previous → STAY OUT with a reason. |

## Configuration (tests/tst_config.cpp, DES-SVC-CFG, REQ-F-017/-018, REQ-N-004) — integration

| ID | L | Case |
|----|---|------|
| TS-CFG-001 | I | With no config files and no env vars, defaults apply (demo, SPX500, no credentials ⇒ simulation mode label). |
| TS-CFG-002 | I | `config.json` (non-secret) and a sibling `apiKeyEtoro.json` (secrets) layer correctly: keys come only from the secrets file. |
| TS-CFG-003 | I | Environment variables override both files. |
| TS-CFG-004 | I | `isLive` requires credentials AND mode "real"; mode labels match. |

## Simulation engine (tests/tst_simulationengine.cpp, DES-SVC-SIM, REQ-F-017) — integration

| ID | L | Case |
|----|---|------|
| TS-SIM-001 | I | `prepare`+`emitSnapshot` publish history, price, cash and leverage options; `tick` moves the price. |
| TS-SIM-002 | I | `openPosition` reduces cash and publishes the position with SL/TP rates set from the amounts. |
| TS-SIM-003 | I | An adverse price path triggers the stop-loss auto-close, frees the margin and records a closed trade in the monthly summary. |

## JSON/HTTP plumbing (tests/tst_jsonhttp.cpp, DES-SVC-HTTP, REQ-N-003) — integration, local mock server

| ID | L | Case |
|----|---|------|
| TS-HTTP-001 | I | A 200 JSON reply is delivered parsed with ok=true. |
| TS-HTTP-002 | I | A 429 with Retry-After on an idempotent GET is retried and succeeds on the second attempt (one callback, ok=true). |
| TS-HTTP-003 | I | A POST failing with 500 is NOT retried; the callback reports ok=false with the status. |

## eToro client (tests/tst_etoroclient.cpp, DES-SVC-CLIENT, REQ-F-014/-017/-025, REQ-N-003) — integration, local mock server

| ID | L | Case |
|----|---|------|
| TS-CLI-001 | I | The trade-history pager walks multiple pages until an empty page and reports account totals in `monthlyPnlReady`. |
| TS-CLI-002 | I | `closedTradesReady` delivers the individual trades with open/close cost estimates equal to invest·lev·spread%/2 from the bulk-rates spread, discloses the spread % each estimate priced with (`spreadPctUsed`) and keeps the frozen-quote flag on when no freshness poll ran (`spreadStale`). |
| TS-CLI-003 | I | Without credentials, `start()` runs the simulation and publishes a display FX rate (`fxRateUpdated` > 0) — orders must not block on "waiting for the EUR/USD rate" (regression). |
| TS-CLI-004 | I | The open-trade set comes from the live `/portfolio` view, not from the cached `/pnl` snapshot: a position still listed by `/pnl` but absent from `/portfolio` (closed at eToro, or auto-closed by SL/TP) is dropped, while the surviving position keeps eToro's own P/L overlaid from `/pnl` (regression). |
| TS-CLI-005 | I | Closed trades are named from the id→symbol map when the walk COMPLETES, not while pages parse: with id resolution answering 200 ms late, the trades still come out listed under their symbols and the per-instrument summary contains every listed instrument, not just the force-mapped current one, with the open+close spread estimates rolled up per symbol (`estSpreadCosts`) (regression). |
| TS-CLI-006 | I | A `fetchClosedTrades` issued while a walk is paging is queued (latest lookback wins) and runs right after it, instead of being silently dropped — the details dialog's 13-week fetch must survive the startup 7-week walk (regression). |

## Coverage & gaps

UI-level requirements (REQ-F-001, -002, -004, -013, -015, -019, -021, -024,
REQ-N-001, -002, -005) are exercised manually / by the offscreen screenshot
QA aid and are reported as *gaps* in the traceability matrix until automated
GUI tests exist — the matrix makes this visible rather than hiding it.
REQ-F-003 and REQ-F-016 show as covered because their SL/TP and currency
conversion *math* is unit-tested (TS-POS-002/-003); their UI legs (the guarded
order flow, the EUR display itself) remain manual like the list above.

## Performance benchmarks (tests/tst_benchmarks.cpp, REQ-N-006)

| ID | L | Case |
|----|---|------|
| TS-PERF-001 | U | `monteCarlo` (1200 paths) benchmark over a deterministic 240-bar walk — QBENCHMARK wall-clock per iteration. |
| TS-PERF-002 | U | `buildTradePlan` benchmark (full plan incl. its Monte-Carlo) over the same walk. |
| TS-PERF-003 | U | `computeDecisionRows` benchmark over 25 instruments with intraday series. |
