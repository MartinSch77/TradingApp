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
| TS-DEC-007 | U | The evidence prompt — the actual interface to both advisors — carries every source it claims to: the VIX regime in all three wordings, the crowd read, the multi-timeframe rating (NaN timeframes left out of the consensus, an all-NaN rating contributing nothing), the news score and the headline text, the leverage cap and both directions; an empty candidate list says HOLD instead of presenting nothing as a choice; and the rating wording table is pinned bucket by bucket. |
| TS-DEC-008 | U | The session's own structure: the opening range takes its high/low from the first 30 bars, reports inside / broken up / broken down with the excursion measured against the range's OWN width, and answers "no read" for too little session, a flat opening, an empty series or a zero window; relative strength is one session return minus the other with a missing series yielding no read rather than a one-sided claim; and both reads reach the model — the prompt states the range and the Nasdaq-versus-S&P leadership. |
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
| TS-SCRIPT-008 | I | The shipped `examples/trade_script_reference.txt` is parsed by the real parser: it loads all-or-nothing, exercises every keyword the format has (SIGNALS, TRAILING, FROM, TO, both sides, a non-offered leverage), and every line is UNREACHABLE by construction — buys far below and sells far above any price their instrument trades at, so loading or even arming the example cannot cost money. |
| TS-SCRIPT-007 | U | SIGNALS entries place only when ensemble AND AI favour the side; neutral/disagreeing/unconfigured AI all wait; unflagged entries ignore the sources. |

## Paper-trading bot (tests/tst_papertrader.cpp, DES-DOM-PAPER, REQ-F-029)

The simulation is only worth reading if it cannot flatter itself, so these check
the cost model and the accounting identity against hand-computed figures rather
than against the implementation.

| Test id | Kind | Verifies |
| --- | --- | --- |
| TS-PAPER-001 | U | Cost/P-L model: half the live spread on the notional per side (15 000 × 0.02% / 2 = 1.50), an unknown spread charges nothing, units = stake × leverage / rate, and the FX-free P/L identity (+1% on a 15 000 notional = ±150 by side). |
| TS-PAPER-002 | U | Rollover: one night per date boundary crossed, a Friday night charged three times (Tue→Sat = 6), invalid/reversed ranges charge nothing; the per-unit fee scales by units, nights and EUR/USD, and a negative table entry stays a CREDIT. |
| TS-PAPER-003 | U | Entry geometry: both sides fill at the quote's MID (the half-spread is a separate charge, so an ask fill would bill it twice), stop/target on the correct sides with reward:risk exactly 1.5, leverage inside the hard cap, and "cannot size" for a neutral call, a one-sided or missing quote, or too little history. |
| TS-PAPER-004 | U | The entry gate refuses — each with its own stated reason AND its own countable code (the scan summary counts them) — a closed market, a stale quote, no call, sub-floor confidence, an unknown spread, an instrument already held, the trade limit, the ruin guard and the exposure cap; and takes the good candidate at 6% of equity. |
| TS-PAPER-005 | U | Exits: stop and target on both sides, a gap past both legs read as the STOP (never the better outcome), signal flip only with conviction, neutral never closes, and the max-hold cut-off. |
| TS-PAPER-006 | U | Book accounting reconciles to the cent: the opening spread leaves cash at once, a 1% mark shows the right open P/L, two rollover nights bill exactly, a round trip at the entry rate loses exactly the two spreads plus rollover, and the cash-based and P/L-based equity agree. |
| TS-PAPER-007 | U | Persistence round trip: open and closed trades, cash, realised P/L and the close reason survive save→load; a restored book issues non-colliding ids; an unknown schema is refused instead of half-loaded. |
| TS-PAPER-008 | U | Stake sizing compounds with equity (6% of 50 000 / 80 000 / 20 000), is clipped by the exposure cap, refuses a stake below the minimum, and never goes under the floor on a small account. |
| TS-PAPER-010 | U | Proposal-symbol resolution: exact and case-insensitive matches, an exact match preferred over a substring one (`GOLD` vs `GoldMiners`), a chatty `"SPX500 composite"` still resolving, and ambiguous / unknown / empty answers resolving to nothing. |
| TS-PAPER-011 | U | The AI gate per mode: OFF passes the composite through; CONFIRM allows only the model's pick while the composite agrees and refuses a disagreement, a neutral composite and any other instrument; LEAD trades the model's side even against the composite. A HOLD, an unusable answer and an unresolvable instrument refuse in both AI modes with their own reasons. Leverage honours a model's caution (asked 3 → 3) but never its ambition (asked 20, sized 10 → 10), and only in LEAD. |
| TS-PAPER-012 | U | The RISK BUDGET, not a trade count, limits how many trades run: risk per euro of stake = leverage × stop distance, an early stake is conviction-sized, later ones are clipped to the room left and finally refused with the code of the limit that ACTUALLY bound — `risk-budget`, `margin-cap` or `cash`, each checked separately (the live log once mislabelled a margin refusal as a risk one), and no limit reported when the target stake fits; a trade's own contribution is notional × stop distance; the concurrency bound is above what one-position-per-instrument can reach. |
| TS-PAPER-013 | U | The carry exits: remaining upside (450 EUR on a 3%-away target) vs the cost to hold — a cheap fee stays open, a dear one closes with `carry beats the edge`, a CREDIT never closes and unknown fees keep the rule silent; the Friday-night lookahead is right (Friday yes, Wednesday no, invalid no), a flat position closes before the tripled weekend charge while one already ahead of it rides through, a weekend credit does not close, and a touched stop still outranks the economics. |
| TS-PAPER-014 | U | The account never commits more than it holds: opening trade after trade until the economics refuse, cash stays ≥ 0 at every step (it went negative once, at a 100% margin cap plus the from-cash opening costs), margin stays within its cap measured on the decision-time equity, free margin is left over, and it really does take many trades before stopping — the count is not the limit. |
| TS-PAPER-015 | U | The day rules: a made day (booked net ≥ target) and a lost day (≤ −limit) both stop opening, one cent short of either does not, a new date starts open whatever yesterday did, Saturday/Sunday are refused unless weekday-only is switched off, zero disables a rule, the entry gate reports `day-target` / `day-loss`, and the stake after a losing day is SMALLER — never escalated to chase the number. |
| TS-PAPER-016 | U | The measured record over three days (+200 / −150 / +400 short): trades, days, net, net/day, expectancy, profit factor 4.0, win rate, drawdown 150 EUR (0.3%), one day at target, the rolling window, the long/short split and costs; an empty record measures nothing; and the live gate refuses that record naming BOTH missing conditions, passes a record meeting every threshold, and blocks with exactly one reason per individually-failed threshold. |
| TS-PAPER-024 | U | When it trades matters as much as what: the session-phase classifier reads the instrument's OWN exchange clock — the first quarter hour after an open as its own phase, the readable rest of that hour, the central-bank window at 20:00/20:30 Berlin, the 14:30 and 16:00 macro slots (which take precedence over the opening hour), the power hour and the closing half hour, the 17:00–17:30 German close, the quiet American lunch, weekends and an invalid instant; the first quarter hour and the central-bank window are SAT OUT (switchably) rather than traded smaller; a loud window demands more conviction and is traded smaller by exactly the configured factor, and refuses a thin candidate as `volatile-window` while the same candidate passes at a quiet hour; the per-instrument cooldown refuses a re-entry with the minutes named and lets it through once cooled (and off when set to 0); the book-wide pace limit refuses as `pace-limit` at its bound, and an entry INTO a fresh opposite opening-range break is refused as `against-range-break` while the same trade WITH the break is taken (and the rule is switchable). |
| TS-PAPER-029 | U | Some instruments have to earn the attempt: a quiet dollar index is refused as `reluctant-symbol` naming the hourly move that refused it, the SAME quiet series on an instrument not on the list is taken (the rule is about the instrument, not about quiet markets), a lively dollar index WITH doubled conviction is traded, a lively one with ordinary conviction is still refused, and emptying the list removes the reluctance. |
| TS-PAPER-028 | U | The record says which RULE made or lost the money: net and count per exit reason, summing exactly to the total, with a rule that never fired absent rather than zero (0.00 would read as "measured and neutral"); and the fade rule must be worth acting on — a position down less than its round trip is left alone, the same fade acts once the loss exceeds it. |
| TS-PAPER-027 | U | Every phase word, every day-gate word and both non-European clock families are reachable: Hong Kong is judged on the Hong Kong clock (its first quarter hour, its opening hour, the run into its 16:00 close, and no session in its small hours — whatever the user's own clock says), the 08:00 Berlin release slot classifies, the confluence refusal names the numbers it was given, a threshold above what could be measured is clamped so a thinly-covered instrument stays tradable, and an uncatalogued symbol keeps its own bucket and the careful leverage ceiling. |
| TS-PAPER-026 | U | The clock, the pace and the confluence all gate one entry: a loud window raises the conviction bar and divides the size by exactly the configured factor, a thin candidate is refused as `volatile-window` there but taken at a quiet hour, the first quarter hour and the central-bank window are SAT OUT (and switchably so), the per-instrument cooldown and the book-wide pace limit refuse with their own codes, an entry INTO a fresh opposite range break is refused as `against-range-break`, and too few agreeing independent reads refuse as `no-confluence`. |
| TS-PAPER-025 | U | Churn is what loses the money: a model reversal inside the minimum holding time is SHOWN but not acted on (`ai-too-soon`), the same answer closes the trade once the time has passed, a hesitant reversal never closes anything however old the trade, the dynamic exits wait out the same clock while a STOP still closes instantly, a model-led trade is judged against the COMPOSITE's conviction at entry rather than the model's own scale, and forex is levered more carefully than an index by its bucket ceiling — with every cap applied BEFORE the ladder fold, so the leverage is always a step the instrument really offers (an x8 ceiling on Gold.24-7, which sells 1/2/5/20, folds to x5 instead of inventing x8). |
| TS-PAPER-020 | U | Adding to a position: allowed with nothing open, refused as `already-holding` without a model pick, taken with one, refused as `opposite-open` against the other side, refused as `symbol-count` at the per-instrument limit and as `symbol-risk` at its risk cap (which is tighter than the bucket cap), and the book aggregates count/side/risk per instrument itself. |
| TS-PAPER-021 | U | The hold review and its per-trade flag: silence and an unrelated pick keep the position AND report NO OPINION ("—") rather than a recommendation, an explicit HOLD or an agreeing pick keeps it and reports "hold" (`ai-keep`); the opposite side closes it as `ai-reversed` (carrying the model's reason) and an explicit CLOSE as `ai-close`; a short is the mirror image; with the model off nothing it "said" counts; and the prompt section names the held instrument, its side, its result and the CLOSE contract. |
| TS-PAPER-022 | U | Costs decide entries and dynamics decide exits: the round trip is both half-spreads, a spread wide enough to swallow the target refuses with `cost-vs-edge` (and switching the rule off takes the same candidate), a faded signal on a losing position closes it while conviction or profit keeps it, a winner that has handed back most of its peak is banked while a trivial peak is not, and the peak survives a save/load. |
| TS-PAPER-023 | U | The day ledger survives a restart — date, booked net, opened and closed counts all round-trip — and a book saved before the ledger existed still loads, starting a fresh day with its record intact. |
| TS-PAPER-019 | U | Banking the day: nothing open or no sufficient winner books nothing, one sufficient winner is picked, the SMALLEST sufficient winner wins over bigger ones (least upside given up), already-booked net counts towards the gap, a made day and a lost day both leave the rule silent (the day gate and the loss limit govern), the switch and a zero target disable it, and the reason has its own word so a cut winner is visible in the record. |
| TS-PAPER-018 | U | Correlated positions share one risk bucket: the index symbols, both FX names (including the dollar index, listed under Indices but an FX bet), the metal and the other commodities map as documented and an unknown symbol gets its own bucket; the per-bucket cap is below the portfolio budget and therefore binds; a full equity-index bucket refuses the next index trade with `group-risk` naming the bucket while the portfolio budget still has room, clips a partly-spent bucket to the room left, and leaves other buckets untouched; and the book itself aggregates per bucket from its open positions, summing to the portfolio risk. |
| TS-PAPER-017 | U | Shorts are first-class: a SELL candidate clears the same gates, is sized identically (same stake, leverage, fill and risk per euro), has mirrored stop/target geometry, and earns when the price falls. |
| TS-PAPER-009 | U | Every close reason has a word for the table, and the trade read-outs (notional, units, effective rate before the first mark, gross/costs/net, holding hours) are consistent. |

## Independent reads and their agreement (tests/tst_indexconfluence.cpp, DES-DOM-CONFLUENCE, REQ-F-035)

| ID | Type | What it pins |
|----|------|--------------|
| TS-CONF-001 | U | The reference list is what it claims to cover: both volatility indices, the 10-year yield and exactly eight heavyweights, eleven tickers in all — the list IS the documentation of what the participation read measures. |
| TS-CONF-002 | U | Each read reports what it measured or that it could not: technology leading supports a long and carries its number, volatility is read by direction with the Nasdaq judged by ^VXN and everything else by ^VIX, a rising yield argues against a long, participation is support / opposition / measured-neutral for a split field, structure comes from the opening range — and with no reference series every read reports UNKNOWN rather than defaulting to bullish (including a heavyweight field too thin to read). |
| TS-CONF-003 | U | Unknown never counts as agreement: a full bullish set scores four or more met with zero unknown, the same reads scored for the other side flip met and against exactly, nothing measurable scores five unknowns and zero met, and a zero direction is not a score at all. |

## The learned outcome model (tests/tst_botnet.cpp, DES-DOM-BOTNET, REQ-F-033)

| ID | Type | What it pins |
|----|------|--------------|
| TS-NET-001 | U | A model is read or rejected, never half-used: no file, a shape that does not line up, and a foreign feature list each yield a stated reason and `ok=false` (with a score of 0 that the gate can never mistake for an opinion), while a well-formed one reads back with its measured record. |
| TS-NET-002 | U | The score is the arithmetic it claims to be: sigmoid(tanh(x)) for a one-unit model, 0.5 at the training mean, monotone in the feature it was given, inside [0, 1] at extremes, and neutral for inputs the caller does not have. |
| TS-NET-003 | U | An unproven model never refuses a trade: off is not consulted, an absent model allows with a reason, too few samples or a coin-flip AUC score-but-allow, advise annotates only, and only a trusted model in gate mode refuses — with the `net-score` code — while still passing a setup it likes; the window's summary names each state. |
| TS-NET-005 | U | The app trains itself with no second runtime: 300 separable examples produce a model with a held-out AUC above 0.9 that learns the separation and, having earned both trust thresholds, refuses a bad setup; two runs over one record agree exactly; the model round-trips through the file format; a record that is too small or one-sided yields NO weights and a stated reason; and one experience line becomes one example — or nothing, never a half-read one. |
| TS-NET-004 | I | The Python trainer and this build agree on the model they exchange: `tools/train_bot_net.py` really runs over an app-shaped experience log, its output loads here with the same column order, it separates the two kinds of trade it was shown, and a record with nothing to learn from exits 3 ("skipped") without writing a model. |

## Shared data types (tests/tst_models.cpp, DES-DOM-MODELS, REQ-F-026)

| ID | Type | What it pins |
|----|------|--------------|
| TS-MODEL-001 | U | A resting order's equality reacts to EVERY field a broker poll can bring back changed (id, instrument, symbol, side, trigger, amount, leverage, SL, TP, trailing, status, timestamp) — a stale order shown as current is the failure this prevents. |
| TS-MODEL-002 | U | Every shared type survives a QVariant round trip — including a resting order inside a QList, field for field, i.e. is a registered metatype: without that, a queued signal carrying it fails at runtime in a slot that simply never fires. |

## Local-LLM advisor (tests/tst_ollamaadvisor.cpp, DES-SVC-OLLAMA, REQ-F-030)

Against an in-process mock of Ollama's HTTP API — no test needs a running daemon.

| Test id | Kind | Verifies |
| --- | --- | --- |
| TS-OLLAMA-001 | I | No model configured: an immediate error result and the availability probe's "no model configured" diagnosis, with NOTHING sent over the wire. |
| TS-OLLAMA-002 | I | The request shape: `/api/generate` carrying the configured model, the caller's evidence verbatim, `stream:false`, `format:"json"`, a JSON-only system prompt and a temperature ≤ 0.3 — and the proposal parsed back out of `response`. |
| TS-OLLAMA-003 | I | The availability probe diagnoses all three states: ready (model served, list returned), an implicit tag (`llama3.2` matches `llama3.2:latest`), and up-but-not-pulled — the last naming the `ollama pull` to run. |
| TS-OLLAMA-004 | I | The defensive parse of what small models really answer: JSON inside prose and ```json fences, lower-case `sell`, `"high"` → 75, `"x3"` → 3, `0.62` → 62%, `"SELL (short)"` → SELL, an unknown action → HOLD, and prose with no JSON → a reported failure. |
| TS-OLLAMA-007 | I | The model may name MANY instruments: a ranked `{"picks":[…]}` list comes back in order; a bare array, an alternative key (`trades`), a SYMBOL-KEYED map (the shape qwen2.5:1.5b really answers with, captured verbatim) and a single pick object are all accepted; an empty list is reported as "nothing worth trading" rather than as a failure or a HOLD; a runaway answer is bounded at 10 picks; and the `rationality`-for-`rationale` key a real model produced still reads. |
| TS-OLLAMA-005 | I | Transport failures are reported, not swallowed: an HTTP 500 from the daemon and a port with nothing listening both yield ok=false with a message, and the advisor is usable again afterwards. |
| TS-OLLAMA-006 | I | One request at a time (a second call while one is in flight is refused, the first still completes, exactly one request reaches the wire), and a scheme-less `127.0.0.1:11434/` host is normalised. |

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
| TS-EVT-004 | U | Every event family the explainer claims to cover answers with its own plain-language text (rates, inflation, unemployment, jobs, GDP, retail sales, PMI/ISM, confidence) and an unknown release says so while naming the instrument; and the side each family implies follows the same forecast-versus-previous comparison — hotter inflation bearish, cooling inflation bullish, rising unemployment bearish, stronger sales bullish, an unchanged forecast no side, an uncovered family a swing warning rather than an invented direction. |
| TS-EVT-003 | U | `proposeActivity` (REQ-F-023): high-impact hot CPI → SELL after the print; medium-impact stronger PMI → BUY before the release; missing forecast/previous → STAY OUT with a reason. |

## Configuration (tests/tst_config.cpp, DES-SVC-CFG, REQ-F-017/-018, REQ-N-004) — integration

| ID | L | Case |
|----|---|------|
| TS-CFG-001 | I | With no config files and no env vars, defaults apply (demo, SPX500, no credentials ⇒ simulation mode label). |
| TS-CFG-002 | I | `config.json` (non-secret) and a sibling `apiKeyEtoro.json` (secrets) layer correctly: keys come only from the secrets file. |
| TS-CFG-003 | I | Environment variables override both files. |
| TS-CFG-005 | U | The bot's daily rules are configuration, not code: the documented 350/350 defaults hold with no files, `botDailyTarget`/`botDailyLossLimit` in `config.json` replace them, `TRADINGAPP_BOT_TARGET=0` switches a rule off (0 is a real value here), and a negative override is refused so a typo cannot widen what may be lost. |
| TS-CFG-006 | U | Numeric settings and an explicit config path: leverage and poll interval come from the file, a sane env value replaces them, and an unsafe one (poll under 500 ms, leverage under 1) or an unparsable one is refused so the file value stands; `$ETORO_CONFIG` names the config file itself with the secrets file looked up BESIDE it; and a malformed file is skipped rather than fatal. |
| TS-CFG-007 | U | Forced simulation cannot be talked out of it: with real keys and `mode: real` on disk the app is LIVE, and with `TRADINGAPP_FORCE_SIMULATION` set the very same files yield no credentials, no live mode and a label that says WHY — while the keys are still readable, simply unusable. `0`, `false` and an empty value mean not-forced (a switch that turns on by accident is as bad as one that cannot turn on); anything else means on. |
| TS-CFG-004 | I | `isLive` requires credentials AND mode "real"; mode labels match. |

## Simulation engine (tests/tst_simulationengine.cpp, DES-SVC-SIM, REQ-F-017/-027) — integration

| ID | L | Case |
|----|---|------|
| TS-SIM-001 | I | `prepare`+`emitSnapshot` publish history, price, cash and leverage options; `tick` moves the price. |
| TS-SIM-002 | I | `openPosition` reduces cash and publishes the position with SL/TP rates set from the amounts. |
| TS-SIM-003 | I | An adverse price path triggers the stop-loss auto-close, frees the margin and records a closed trade in the monthly summary. |
| TS-SIM-006 | I | The simulated broker's remaining money paths: a take-profit closes in profit and books cash the other way from a stop-out; a trailing stop follows the price in the trade's favour and never once moves against it over 400 ticks; adjusting a live position rewrites both barriers and the trail distance; an unknown position id is answered ("not found") rather than ignored on both modify and close; and an order larger than the account is refused with its numbers instead of silently sized down. |
| TS-SIM-007 | I | The SIMULATED screener answers for every instrument without any credentials: one row per symbol with a leverage cap off the instrument's own ladder and a positive price series long enough for the indicators, progress reported with the right total, `screenerFinished` exactly once — and an empty universe still finishes rather than leaving the window waiting. |
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
| TS-CLI-016 | I | A rejected order names the FIELD the broker objected to: eToro answers a bad order with ValidationProblemDetails — a generic title plus the real reasons under `errors: {field: [msg]}` — and the reported message carries those field names, not just "one or more validation errors occurred". |
| TS-CLI-017 | I | Leverage comes from the account's own CFD configuration: the eligibility answer's `leverageConfigs` are filtered to the CFD settlement type (a realStock entry is not an offer), sorted, de-duplicated, and a nonsense 0 dropped. |
| TS-CLI-018 | I | Both money-moving position operations report their outcome: a close and an SL/TP change succeed through their real endpoints, and a refused close carries the BROKER's reason ("position already closed") rather than Qt's transport string — which is always present on an HTTP error and used to hide the real one. |
| TS-CLI-019 | I | The screener walks every resolved instrument: one bulk eligibility call supplies the per-instrument leverage CAP, one candle request per instrument fills its closes, progress is reported as it goes, `screenerFinished` fires exactly once per scan, and a rescan includes the ids that resolved while the previous scan was running. |
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
