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

## Trade script (tests/tst_tradescript.cpp, DES-DOM-SCRIPT, REQ-F-028)

| Test id | Kind | Verifies |
| --- | --- | --- |
| TS-SCRIPT-001 | U | A fully-specified line parses into every field (side, trigger, window, SIGNALS, amount, SL/TP, TRAILING, LEV). |
| TS-SCRIPT-002 | U | Comments, blank lines, date-only FROM and defaults for every optional field; line numbers are 1-based over the raw file. |
| TS-SCRIPT-003 | U | All-or-nothing loading: one bad line rejects the whole file with its line number, reason and content. |
| TS-SCRIPT-004 | U | Per-field validation messages (missing AMOUNT, non-positive numbers, empty symbol, missing @, LEV < 1, unknown/duplicate fields, FROM after TO, bad dates, flag with a value). |
| TS-SCRIPT-005 | U | Leverage snaps to the next lower offered value; above all → highest offered; below all → lowest offered; unknown offering → unchanged. |
| TS-SCRIPT-006 | U | The time window gates resting (before FROM no, at FROM/TO yes, after TO expired and final; no window = immediately, never expires). |
| TS-SCRIPT-007 | U | SIGNALS entries place only when ensemble AND AI favour the side; neutral/disagreeing/unconfigured AI all wait; unflagged entries ignore the sources. |

## Position math (tests/tst_positionmath.cpp, DES-DOM-POS, REQ-F-003/-016)

| ID | L | Case |
|----|---|------|
| TS-POS-001 | U | `priceDecimals` gives more decimals for low-priced instruments than for indices. |
| TS-POS-002 | U | `accountValuePerPoint` uses amount×leverage/openRate (FX-free) and falls back to units when the notional is unknown. |
| TS-POS-003 | U | `slTpAmountText` converts a rate distance to the account/display amount and is empty when the leg is off; `slSignedAmountText` is negative for a losing stop and positive for a stop on the winning side. |
| TS-POS-004 | U | `closedSincePreviousIds` reports the position ids that vanished between two portfolio snapshots — in shown order, several at once, none on the first snapshot (no previous set) and never an empty id. This is what triggers the immediate closed-trades refresh (REQ-F-025). |
| TS-POS-005 | U | `positionPnl` reproduces eToro's own unrealised-P/L identity to the cent on figures captured from a real account (NSDQ100.24-7 1.545335 units from 27979.10 at bid 28075.99 → 149.73; GOLD.24-7 → 80.58): units × (close rate − open rate) × conversion rate, marked at the bid for a long and the ask for a short, with no fee or spread term. A quote-currency instrument (HKD, conversion 0.1275) converts the move instead of reporting the raw 650, a position without units falls back to the account-currency value per point, and an unusable quote yields 0 rather than a wrong number. |

| TS-POS-006 | U | `Quote` (DES-DOM-MODEL) decides where a position is marked, so each rule is pinned: a long closes at the bid and a short at the ask, a one-sided row falls back to the side it has, an unknown or crossed spread is 0 (never negative), a missing conversion rate falls back to 1.0 rather than scaling the P/L to zero, and `ageMs` is the PRICE's age — −1 while unstamped, under `kQuoteStaleMs` at 90 s, over it at the .24-7 feed's measured 11 min. Also `InstrumentFees::isValid` per leg (a negative leg is a credit). |

## Positions model (tests/tst_positionsmodel.cpp, DES-UI-POSMODEL, REQ-F-012/REQ-N-006) — headless ui-model unit tests

| ID | L | Case |
|----|---|------|
| TS-PM-001 | U | An unchanged position-id set refreshes in place (dataChanged, zero model resets — editors/marks survive by construction); a changed set resets the model. |
| TS-PM-002 | U | While an SL/TP cell editor is open (`beginCellEdit`), portfolio snapshots, FX updates and SL/TP echoes emit no dataChanged for that one cell (Qt would re-fill the open editor) while every other cell refreshes and the stored value keeps updating; `endCellEdit` emits the single held-back dataChanged. |
| TS-PM-003 | U | `repriceOpenPnl` marks EVERY row from its own instrument's quote, not only the rows of the instrument on screen: a trade on id 686 quoted at 28075.99 reads €149.73 (eToro's own figure) instead of the €84.98 a stale snapshot claimed. A quote eToro stamped 11 minutes ago — the measured lag of its .24-7 feed — does not mark the row at all: it keeps the snapshot figure with the trailing "*" not-live marker in grey, as does a row with no quote at all (field regression: the P/L column read ~€90 below eToro's own screen). |

| TS-PM-005 | U | The panel totals sum the columns AS SHOWN: invested adds the per-row Amount figures with their rounding (4000 + 1001 at 0.5 → 2000 + 501 = €2501, not €2500.50), and the P/L total sums the P/L column — snapshot figures while rows have no quote, live marks once they do, mixed totals flagged not-live until every row is marked. |
| TS-PM-004 | U | The SL/TP cells state what the leg is worth, so their tooltip names the instrument rate that triggers it: "triggers when EURUSD trades at 1.1300", its distance from the open rate (0.0073 below 1.1373) and — once a quote exists — from the rate the trade closes at now (0.0100 below the bid 1.1400), plus the cell's own amount. A trailing stop says so; a leg that is off reads "No take-profit on this trade" instead of a trigger at zero. |

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

## Simulation engine (tests/tst_simulationengine.cpp, DES-SVC-SIM, REQ-F-017/-027) — integration

| ID | L | Case |
|----|---|------|
| TS-SIM-001 | I | `prepare`+`emitSnapshot` publish history, price, cash and leverage options; `tick` moves the price. |
| TS-SIM-002 | I | `openPosition` reduces cash and publishes the position with SL/TP rates set from the amounts. |
| TS-SIM-003 | I | An adverse price path triggers the stop-loss auto-close, frees the margin and records a closed trade in the monthly summary. |
| TS-SIM-004 | I | A limit order rests without booking a position or margin, a second one can be cancelled individually, and the remaining one turns into a position (opened at or beyond its trigger, carrying its stop-loss) once the walk touches the trigger rate. |
| TS-SIM-005 | I | Adjusting a resting order changes only trigger/SL/TP (size, leverage and side carry over) and renumbers it, mirroring the real cancel-and-re-place; an unknown order id changes nothing and reports a failure. |

## JSON/HTTP plumbing (tests/tst_jsonhttp.cpp, DES-SVC-HTTP, REQ-N-003) — integration, local mock server

| ID | L | Case |
|----|---|------|
| TS-HTTP-001 | I | A 200 JSON reply is delivered parsed with ok=true. |
| TS-HTTP-002 | I | A 429 with Retry-After on an idempotent GET is retried and succeeds on the second attempt (one callback, ok=true). |
| TS-HTTP-003 | I | A POST failing with 500 is NOT retried; the callback reports ok=false with the status. |

## eToro client (tests/tst_etoroclient.cpp, DES-SVC-CLIENT, REQ-F-014/-015/-017/-025/-027, REQ-N-003) — integration, local mock server

| ID | L | Case |
|----|---|------|
| TS-CLI-001 | I | The trade-history pager walks multiple pages until an empty page and reports account totals in `monthlyPnlReady`. |
| TS-CLI-002 | I | `closedTradesReady` delivers the individual trades with open/close cost estimates equal to invest·lev·spread%/2 from the bulk-rates spread, discloses the spread % each estimate priced with (`spreadPctUsed`) and keeps the frozen-quote flag on when no freshness poll ran (`spreadStale`). |
| TS-CLI-003 | I | Without credentials, `start()` runs the simulation and publishes a display FX rate (`fxRateUpdated` > 0) — orders must not block on "waiting for the EUR/USD rate" (regression). |
| TS-CLI-004 | I | The open-trade set comes from the live `/portfolio` view, not from the cached `/pnl` snapshot: a position still listed by `/pnl` but absent from `/portfolio` (closed at eToro, or auto-closed by SL/TP) is dropped, while the surviving position keeps eToro's own P/L overlaid from `/pnl` (regression). |
| TS-CLI-005 | I | Closed trades are named from the id→symbol map when the walk COMPLETES, not while pages parse: with the mock holding the id resolution until after the history pages were parsed and the walk's last request until after the resolution landed — an order stated through MockHttpServer::holdUntil rather than timed — the trades still come out listed under their symbols and the per-instrument summary contains every listed instrument, not just the force-mapped current one, with the open+close spread estimates rolled up per symbol (`estSpreadCosts`) (regression). |
| TS-CLI-006 | I | A `fetchClosedTrades` issued while a walk is paging is queued (latest lookback wins) and runs right after it, instead of being silently dropped — the details dialog's 13-week fetch must survive the startup 7-week walk (regression). |
| TS-CLI-007 | I | Market-open follows the quote timestamp ADVANCING between polls, not its absolute age: with every quote stamped ~6 min behind real time (eToro's public feed lags, which the old 300 s age gate read as "frozen" and used to lock BUY/SELL on every instrument mid-session), an instrument whose stamp moves counts as open, one whose stamp is frozen counts as closed from the second poll on, and one stamped two days back is closed already on the first — where no baseline exists yet, the delay-absorbing age fallback decides (regression). |
| TS-CLI-008 | I | A limit order leaves the app as eToro's OWN resting order — the POST body carries `orderType: "mit"` with the `triggerRate`, and its SL/TP rates are measured from that trigger rate (1000 at x5 entered at 4000 = 1.25 units → SL 3920 / TP 4160), not from the current 5000/5001 quote a market order would price off; the order is then listed as resting under the broker's id, picks up the broker's status wording while it waits, and is dropped with a "triggered" report once the lookup reports it Filled. |
| TS-CLI-009 | I | A SHORT limit order goes out as `transaction: sellShort` with the stop ABOVE and the target BELOW the trigger rate (500 at x2 at 5200 → SL 5720 / TP 4160 — an inverted pair is what a broker rejects), and cancelling it issues a DELETE to `/v2/trading/execution{segment}/orders/{orderId}` that removes it from the pending list. |
| TS-CLI-010 | I | An order eToro accepted (200 + orderId) but then REJECTED is dropped from the pending list by the prompt post-placement status check and reported as an error quoting eToro's own `status.errorMessage` and `errorCode` — not as a bland "no longer resting" note discovered a poll cycle later (field regression). |
| TS-CLI-011 | I | Adjusting a resting order's trigger/SL/TP sends the DELETE **before** the replacement POST, ends with exactly one order (new id, new trigger, SL/TP re-measured from it: 1000 at x5 at 4100 → SL 3936 / TP 4428) and carries size, leverage and side over. |
| TS-CLI-012 | I | The resting-order list refreshes on its own 4 s cycle, not on the price poll: with the price poll set to 50 s and the order left WaitingForMarket, at least three status lookups land (the prompt post-placement check plus two cycle ticks) and the third cannot arrive inside 4 s — proving a spaced cadence rather than a spin. |
| TS-CLI-014 | I | Adjusting an order works whatever is on screen: with the app trading instrument 27 and the order resting on 38, the replacement POST carries `instrumentId` 38 (not 27) and the order's own side, size and leverage, with SL/TP re-measured from the new trigger (500 at x2 at 210 → SL 220.5 / TP 189) — field regression: the edit used to be refused with "Select SPX500 first". |
| TS-CLI-015 | I | A rates row eToro publishes behind real time never decides a P/L: with instrument 27's row stamped 11 minutes ago at bid 5020 and its 1-minute candle live at 5100, the open trade is marked from the candle (10 units × 100 = 1000, quote flagged `fromCandle` with the row's 2.0 spread kept for the ask) and the delayed row's 200 is never emitted — eToro's own /pnl figure (150, computed from the same delayed rate) is not trusted either. Field regression: the column read ~€90 below eToro's own screen on a fast-moving index. |
| TS-CLI-013 | I | Resting orders come from the broker's portfolio breakdown (`clientPortfolio.orders[]`), not only from what the session submitted: a payload with two orders the app never placed yields both — the listed one with its SL/TP rates converted back to amounts (4900/5200 around a 5000 trigger on 10 units → 1000 / 2000), the one on an unlisted instrument still visible as "#999" with its sentinel zero SL/TP left at zero (field regression: the panel was empty while two orders were open). The per-row current rate also resolves for an instrument that is NOT on screen (bulk-snapshot mid 202 for id 38) and stays 0 for an unknown one. |

## Market feeds (tests/tst_marketfeeds.cpp, DES-SVC-FEEDS, REQ-F-009/-019/-020/-022) — integration, local mock server

| ID | L | Case |
|----|---|------|
| TS-FEED-001 | I | VIX level and its change vs the multi-month baseline: null and non-positive closes are dropped before averaging (25 against a 20 average reads +25%). |
| TS-FEED-002 | I | Current-instrument TradingView rating: the scan POST carries exactly the instrument's ticker on the 1h column, and the score is published with its rating word. |
| TS-FEED-003 | I | A rating reply for an instrument that changed under us is dropped: with SPX500's scan reply held back and the instrument switched to EURUSD, only the EURUSD reading is published — one emission, not two. |
| TS-FEED-004 | I | Bulk instrument ratings fan out ticker→symbols: one deduplicated scan request (three timeframe columns) for the shared SP:SPX ticker, the rating lands on both SPX500 and SP.24-7, an unmapped instrument (RUBBER) is omitted and a null timeframe stays NaN. |
| TS-FEED-005 | I | News headlines per symbol: untitled items are skipped, the list is capped at five, provider and published epoch are parsed, and the query filter carries the ticker with its ':' unencoded. |
| TS-FEED-006 | I | Fear & Greed score range validation: an out-of-range score (120) publishes nothing; a valid 72.5/"greed" reading is emitted (crowd sentiment, REQ-F-009). |
| TS-FEED-007 | I | Independent web reference quote for the current instrument: Yahoo chart meta price plus the `regularMarketTime` exchange timestamp are emitted as `webQuoteUpdated`, from a byte-identical 1-minute chart query of the percent-encoded ticker (REQ-F-019). |
| TS-FEED-008 | I | Intraday 1-minute close series: null minutes are skipped, a genuine 0.0 close survives (positiveOnly off), and instruments without a Yahoo ticker issue no request (REQ-F-022). |
| TS-FEED-009 | I | A failing feed logs ONE throttled line per 10-minute window — repeated failing fetches within the window add no further lines. |

## Economic calendar (tests/tst_economiccalendar.cpp, DES-SVC-CAL, REQ-F-020) — integration, local mock server

| ID | L | Case |
|----|---|------|
| TS-CAL-001 | I | Fetch + parse: importance missing/0/1 maps to Low/Medium/High, forecast/previous arrive as string or number, events are time-sorted ascending, and events with an unparsable date or beyond the trading-day window are dropped. |
| TS-CAL-002 | I | Region scoping via the InstrumentCatalog: EURUSD queries `countries=EU,US`, switching to HKG50 re-fetches `HK,CN` immediately, the same regions do not re-fetch, an unknown instrument falls back to `US`, and nothing is fetched before `start()`. |
| TS-CAL-003 | I | A failed fetch emits one "Economic calendar fetch failed" error log line and publishes no event list. |

## AI advisor (tests/tst_aiadvisor.cpp, DES-SVC-AI, REQ-F-008/REQ-N-005) — integration, local mock server

| ID | L | Case |
|----|---|------|
| TS-AI-001 | I | Without an API key `isConfigured()` is false and `requestDecision` reports `decisionReady(ok=false, "No anthropicApiKey configured.")` synchronously — no HTTP request leaves the process. |
| TS-AI-002 | I | Request shape on the wire: exactly ONE POST to `/v1/messages` carrying the model, `max_tokens`, the evidence prompt, the guaranteed-parseable `json_schema` output format, and the `x-api-key`/`anthropic-version` headers — the advisor's complete traffic is this advisory call; nothing order-like ever leaves it (REQ-N-005). |
| TS-AI-003 | I | A Messages reply with the decision JSON in the first text block parses to ok=true with the action normalised to upper case. |
| TS-AI-004 | I | An HTTP 500 yields ok=false with "Claude request failed (HTTP 500…)" and is not retried (POSTs are never auto-retried). |
| TS-AI-005 | I | A 200 reply whose text block is not JSON yields ok=false with "Claude returned an unparsable response." |

## Coverage & gaps

UI-level requirements (REQ-F-001, -002, -004, -013, -021, -024, -026,
REQ-N-001, -002) are exercised manually / by the offscreen screenshot
QA aid and are reported as *gaps* in the traceability matrix until automated
GUI tests exist — the matrix makes this visible rather than hiding it.
REQ-F-015's inference is covered at the service level by TS-CLI-007; only its
BUY/SELL lock in the trade panel remains a manual check.
REQ-F-019's quote acquisition (price + exchange timestamp) is covered at the
service level by TS-FEED-007; the panel that shows the delta versus the eToro
rate remains a manual check.
REQ-N-005 is partially covered at the service level by TS-AI-002 (the
advisor's only wire traffic is the advisory Messages call — nothing
order-like); the double-press gate on the money-moving buttons remains a
manual check.
REQ-F-027 is covered end-to-end at the service level (TS-CLI-008…-014 against the
real API shape, TS-SIM-004/-005 for the simulated broker); its panel legs — the two
rate fields, the independent double-press gate on the limit buttons, and the
resting-order table with its live "Now" column, its click routing (Instr. cell
switches instrument, value cells open the editor), its adjust/cancel buttons and
its editor dialog — remain manual like the list above.
REQ-F-003 and REQ-F-016 show as covered because their SL/TP and currency
conversion *math* is unit-tested (TS-POS-002/-003); their UI legs (the guarded
order flow, the EUR display itself) remain manual like the list above.

## Performance benchmarks (tests/tst_benchmarks.cpp, REQ-N-006)

| ID | L | Case |
|----|---|------|
| TS-PERF-001 | U | `monteCarlo` (1200 paths) benchmark over a deterministic 240-bar walk — QBENCHMARK wall-clock per iteration. |
| TS-PERF-002 | U | `buildTradePlan` benchmark (full plan incl. its Monte-Carlo) over the same walk. |
| TS-PERF-003 | U | `computeDecisionRows` benchmark over 25 instruments with intraday series. |
