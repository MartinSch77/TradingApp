# The bot's decision pipeline — every source, every algorithm, how they combine

This document answers one question precisely: for one instrument, on one scan, what
evidence does the bot actually look at, how is each piece of evidence computed, and in
what order do they combine into "BUY / SELL / stay out, at this size and leverage"?

It is a companion to `CLAUDE.md` (which states the *rules* and their measured rationale)
and `docs/design.md` (which lists each module's design element). This document instead
follows the data as it flows through ONE scan, in call order, so the whole pipeline can be
read top to bottom rather than reconstructed from scattered bullets. Every claim below is
sourced against the code (file:line); where a source exists but is **not** currently wired
into a live decision, that is stated explicitly rather than left to be discovered.

## The shape of the pipeline, in one paragraph

Every scan cycle, `BotSimRunner` builds a ranked list of instruments from `DecisionEngine`'s
composite (itself blended from a technical ensemble, a web rating, a keyword-based news
score, a crowd tilt and a regime term). For each candidate it separately computes the
nine-read confluence and the combined `LeadSignal`, asks the local LLM (if enabled) and
resolves its answer against the composite, sizes an entry (`buildEntrySignal`), runs it
through the entry gate (`paperEntryVerdict` — day rules, tradability, pace/session,
confluence majority, stacking, ruin guard, sizing/risk budget, cost-vs-edge) and, if it
survives, a last-chance ML veto (`BotNet`, via `paperNetGate`). Only then does
`PaperBook::open` actually create a position.

## 1. The technical ensemble (`SignalEnsemble.h/.cpp`)

`computeEnsemble(series, vixValid, vixChangePct)` (`SignalEnsemble.cpp:128`) needs at least
31 closes. It is a **vote count, not a weighted sum**: ten independent voters
(`kVoters`, `SignalEnsemble.cpp:44-103`), each a pure function of the closes that returns
`+1`/`-1`/`0`, or abstains (`nullopt`) when it cannot form an opinion:

| Voter | Indicator | Source |
|---|---|---|
| Trend | SMA(10) vs SMA(30) | `Indicators::sma` |
| Momentum | RSI(14) | `Indicators::rsi` |
| MACD | MACD histogram (12/26/9) | `Indicators::macdHistogram` |
| Bollinger | %B(20) | `Indicators::bollingerPercentB` |
| Rate of change | ROC(10) | `Indicators::roc` |
| Regression | OLS trend over 30 bars | `Forecasting::linRegForecast` |
| Pattern | k-NN analog forecast (10-bar, 5 neighbours) | `Forecasting::knnForecast` |
| Stochastic | %K(14) | `Indicators::stochasticK` |
| Long trend | SMA(50) | `Indicators::sma` |
| Volatility regime | raw `vixValid`/`vixChangePct` | caller-supplied |

`score` = sum of cast votes; `votes` = how many voters had an opinion;
`confidence = |score| / votes × 100`; `signal` = "BUY" (score ≥ 2), "SELL" (score ≤ -2),
else "NEUTRAL" (`SignalEnsemble.cpp:139-163`). `applyVixHaircut` (`:167`) — ×0.8 at
VIX ≥ 25, ×0.6 at VIX ≥ 35 — is applied by the *signals panel* display path, **not** inside
`computeEnsemble` itself and **not** by `DecisionEngine` (see below, which has its own,
separate VIX-based regime term).

## 2. The composite (`DecisionEngine::computeDecisionRows`, `DecisionEngine.cpp:137`)

This is the number the rest of the pipeline calls "the composite" — a **second, independent
blend**, not a wrapper around the ensemble's own `confidence`. Per instrument:

```
techSigned = ensemble.signalDir × (ensemble.confidence / 100)
composite  = clamp( Σ(weight × source) / Σ(weight of sources actually available), -1, 1 )
```

Weighted terms (`DecisionEngine.cpp:190-216`), each included only when measurable
(unmeasured sources drop out of both the numerator and denominator — a thin source never
drags the composite toward zero, it is simply absent from the average):

| Source | Weight | What it is |
|---|---|---|
| Technical ensemble | 0.35 | `techSigned` above |
| TradingView web rating | 0.25 | consensus rating (`ratingBySymbol`) |
| News sentiment | 0.15 | `DecisionEngine::newsSentimentScore` — a hand-rolled positive/negative keyword scorer over headlines, `DecisionEngine.cpp:22`. **This is not FinBERT** (see §4). |
| Crowd tilt | 0.10 | CNN Fear & Greed reading |
| Intraday tilt | 0.10 | Yahoo intraday reference-quote tilt |
| Regime | 0.15 | VIX-based tilt + an `eventRisk` flag — always present |

`dir` is the composite's sign with a ±0.02 deadband; `confidence = |composite| × 100`, then
×0.85 when `eventRisk` is set (`DecisionEngine.cpp:219-221`). This `dir`/`confidence` pair is
what the rest of the pipeline calls `compositeDir`/`compositeConfidence`.

## 3. The nine-read confluence (`IndexConfluence.h/.cpp`)

`indexReads(symbol, in)` (`IndexConfluence.cpp:515`) computes all nine `Read`s:

| Read | Function | Logic |
|---|---|---|
| Futures lead | `futuresLeadRead` (`:124`) | Nasdaq-future vs S&P-future session relative strength |
| Futures momentum | `futuresMomentumRead` (`:255`) | leading future's 1/5/15-min returns — a direction only when all three agree in sign |
| Volatility | `volatilityRead` (`:143`) | session change of ^VXN (Nasdaq) / ^VIX (else); falling = bullish |
| Yields | `yieldRead` (`:162`) | session change of the US 10-year (^TNX); rising = bearish for growth |
| Curve | `curveRead` (`:214`) | front (2YY=F, else ^IRX) minus ^TNX change, only when the divergence exceeds 0.5pp |
| Participation | `participationRead` (`:182`) | share of the index's ten heavyweights up (needs ≥ half readable) |
| Above VWAP | `aboveVwapRead` (`:294`) | share of heavyweights above their own session VWAP |
| Up/down volume | `upDownVolumeRead` (`:330`) | volume-weighted: heavyweight $-volume behind up names vs down names |
| Structure | `structureRead` (`:369`) | the instrument's own opening-range break direction |

`confluenceFor(reads, dir)` (`:622`) tallies `met` / `against` / `unknown` over all nine,
fixed order: unmeasured → `unknown`; measured-and-neutral → neither; agrees with `dir` →
`met`; else → `against`. **Unknown never counts as agreement, in either direction** — the
invariant `TS-INV-007` pins.

## 4. The combined indication (`LeadSignal::leadSignal`, `LeadSignal.cpp:257`)

Takes the nine reads, the `HeavyweightPulse`, session phase, VIX, event risk, term
structure, **and the composite's own `dir`/`confidence` from §2** (it does not re-derive
anything from the raw ensemble). Three honesty rules, verified in the code, not just the
comment:

1. **Unmeasurable contributes nothing.** `tallyFor` (`:201`) only adds a read to
   `agreeing`/`against` when `known`; an unknown read is counted separately
   (`out.unknowns`) and never enters the tally (`:207-217`).
2. **Strength is capped by the measurable share.**
   `strength = clamp(share × coverage × phase × regime × 100, 0, 100)` (`:291`), where
   `coverage = measurableWeight / totalWeight` (`:280-281`) — literally multiplies the
   agreement fraction by how much of the evidence was actually available.
3. **Leverage is an upper bound.** `leverageFor(grade)` (`:225`) returns 10/5/2/1 by grade;
   every caller only ever *intersects* `suggestedLeverage` downstream (§7) — it can shrink
   what a candidate is allowed, never raise it.

Grade thresholds scale with how much was measured, not a fixed count: **Strong** needs
`strength ≥ 55` *and* `measured ≥ 7`; **Fair** needs `strength ≥ 35` *and* `measured ≥ 4`
(`:297-305`).

## 5. Crowd/social sentiment and FinBERT — informational only, NOT wired into a decision

`crowdScore` (`CrowdScore.cpp:61`) combines four families by a **weighted, renormalized
sum**: retail 0.35 (contrarian-negated), options 0.30, institutional 0.20, social 0.15
(`:22-31`); the sum is renormalized over the families actually *measured* — same "absent
never drags the score toward zero" rule as §2. `FinBertSentiment::scoreHeadlines`
(`FinBertSentiment.h:43`) feeds the social family via `CrowdCollector.cpp:173`.

**Neither reaches the bot.** `grep`ing `BotSimRunner.cpp`/`PaperTrader.cpp` for
`crowdScore`/`CrowdScore` returns zero hits. Both are consumed only by
`CrowdDashboardWindow`, `MainWindow`'s display and the manual `TradingAdvise` CLI tool —
display/advisory surfaces entirely separate from the bot's scan loop. Do not confuse this
with §2's "news sentiment" weight, which **is** wired but is a hand-rolled keyword scorer,
not FinBERT.

## 6. The local LLM (Ollama) — `OllamaAdvisor` + `paperAiGate`

`OllamaAdvisor::requestDecision(evidencePrompt)` (`OllamaAdvisor.h:63`) is asked once per
scan (`BotSimRunner::onDecisions`, `:546`), only when `aiMode != Off` (`:574`), with a
prompt built from `buildDecisionEvidence` + the open-book context + the crowd evidence
block (`:580-581`). The reply lands asynchronously in `onProposals` (`:1005`), which
normalizes each pick against the tradable catalog into `m_proposals` and — unless the whole
batch has aged past `kProposalMaxAgeMs` — re-evaluates every pending candidate
(`considerEntriesForScan` → `tryOpen`).

`paperAiGate(symbol, compositeDir, proposals, aiMode, source)` (`PaperTrader.cpp:957`) is
where the model's opinion and the composite are actually combined:

- **Off** (the default since 2026-08-12): `allow = compositeDir != 0`, `dir = compositeDir`
  — the model is not consulted for the decision at all (`:977-984`).
- **No usable answer** (no proposal, unparsable, stale, wrong symbol): falls back — in
  **Lead** mode with `leadFallback` set and a non-neutral composite, the composite's
  direction is used; otherwise refused with a named code
  (`ai-not-configured`/`ai-no-answer`/`ai-stale`/`ai-unparsed`/`ai-unknown-symbol`/`ai-other-pick`).
- **An explicit HOLD** (`proposal.dir == 0`) refuses as `ai-hold` — **never** falls back; a
  HOLD is an opinion, not silence.
- **Lead**: the model's direction is used directly.
- **Confirm**: the model is a veto only — refuses on a neutral composite
  (`composite-neutral`) or a disagreement (`ai-disagree`); otherwise allows with the
  composite's own direction.

Leverage: `paperLeverageWithAi(sized, asked, aiMode)` (`:1029`) only lets the model's asked
leverage *reduce* the sized value (`min`), and only in Lead mode — never raise it.

`BotConfig::aiMode` defaults to `Off` (`PaperTrader.h:396`); read once per scan at
`BotSimRunner.cpp:574` (gates whether the model is asked at all) and again inside
`paperAiGate` itself.

## 7. BotNet ML score — last-chance veto only, never an opener

`BotNet::score(inputs)` (`BotNet.h:64`) scores an entry against eleven named features
(`entryFeatureNames`, `PaperTrader.cpp:1631`: confidence, volPct, stopPct, targetPct,
spreadPct, edgeOverCost, leverage, dir, hourUtc, dayOfWeek, aiBacked). Gating mode comes
from `TRADINGAPP_BOT_NET` (`Off`/`Advise`/`Gate`, `BotNet.h:116-120`); only in `Gate` mode,
and only once trained past `minSamples`(200) and `valAuc`(0.55), can a low score
(`< 0.5`) refuse a trade (`net-score`). Called in `tryOpen` (`BotSimRunner.cpp:1360`) as the
**very last check**, strictly after `paperEntryVerdict` has already approved — it can veto
an otherwise-accepted trade, but it can never open one on its own, and an untrained/untrusted
model only annotates (never gates), matching the same "an unmeasurable/untrusted signal
never manufactures a refusal out of nothing" discipline as §§3-5.

## 8. `SwingPullbackStrategyV1` — standalone / backtest-only, NOT in the live loop

The 2026-08-12 strategy redesign's `ITradingStrategy`/`SwingPullbackStrategyV1`/
`swingExitDecision`/`StrategyBacktest` modules are real, tested code (`docs/design.md`,
`DES-DOM-SWING`/`DES-DOM-BACKTEST`) — but `grep`ing `BotSimRunner.cpp` for
`SwingPullback`/`ITradingStrategy` returns **no matches**. The only non-test caller is
`StrategyBacktest.cpp`. This is stated in the class's own header comment
(`SwingPullbackStrategy.h:90`: *"the caller (BotSimRunner, **once wired**)..."*) and repeated
here because it is the kind of fact that is easy to assume is live once code exists and
compiles. Wiring it into the scan loop — alongside a decision about whether it *replaces*
or *runs beside* the composite/lead path for its own focus symbol — is future work, not
part of what any of items 1-7 above changed.

## 9. The final entry gate (`paperEntryVerdict`, `PaperTrader.cpp:1423`)

Checked in this exact order; the first refusal wins and names itself with a stable `code`:

1. **Day gate** (`paperDayGate`) — daily profit target / loss limit already hit today.
2. **Tradability** — market open → quote live → a signal (`dir != 0`) exists at all
   (`market-closed` / `no-live-quote` / `no-signal`).
3. **Pace/session** (`paceVerdict`) — sit-out session phase (opening chaos, policy window) →
   the **confidence floor** (scaled by the phase's own window factor) → re-entry cooldown →
   opens-per-hour pace limit → trading into a fresh opposite range break → the **LeadSignal
   veto** (§4's indication disagreeing at Strong-grade strength refuses as `lead-against`) →
   the **confluence majority** (§3: a majority of *measured* reads must agree, refuses as
   `no-confluence`).
4. **Stacking** — adding to an existing position needs the model to have named it again.
5. **Trade limit** — `openCount ≥ maxOpenTrades`.
6. **Ruin guard** — equity below the floor fraction of starting equity.
7. **No history** — `EntrySignal` itself invalid (`buildEntrySignal` had too little data).
8. **Spread unknown** — live spread not yet known.
9. **Sizing / risk budget** (`paperStakeRoom`) — portfolio / group / symbol risk budget,
   margin cap, cash headroom; names whichever one binds.
10. **Reluctance** — a reluctant/peripheral symbol needs its own, higher conviction bar.
11. **Below minimum stake** after every cap above has been applied.
12. **Cost vs. edge** (`paperEntryEconomics`) — the round-trip cost must clear
    `minEdgeOverCost` against the expected gain, else `cost-vs-edge`.
13. Otherwise: `take = true`, at the sized stake.

## 10. End-to-end call order, one scan of one candidate

```
DecisionEngine::computeDecisionRows            (§2: composite dir/confidence, all instruments)
        │
BotSimRunner::onDecisions
        ├─ markAndExit()                        (existing open positions are marked/exited FIRST)
        ├─ [if aiMode != Off] buildDecisionEvidence + requestProposal()  (async ask, §6)
        │        └─ onProposals() → considerEntriesForScan() → considerEntries()  (per row, confidence-sorted)
        │                 └─ tryOpen(row)
        │
        └─ tryOpen(row):
             1. preTradeRefusal            (focus set, then broker tradability)
             2. gateFor(row) → paperAiGate                       (§6: composite × model)
             3. candidateFor(row, …) → indexReads/confluenceFor  (§3)
                                      → leadSignal                (§4)
             4. buildEntrySignal(candidate, cfg)                  (stop/target geometry, leverage)
             5. paperLeverageWithAi(sized, asked, aiMode)         (§6: leverage cap only)
             6. paperEntryVerdict(candidate, signal, book, cfg)   (§9: the entry gate)
             7. [if taken] applyNetGate → paperNetGate            (§7: BotNet last-chance veto)
             8. PaperBook::open(signal, stake, now)                — or refuse(code, why) and log it
```

Every step that refuses names a stable `code`; every step that could not measure something
treats that as *unknown*, never as agreement or disagreement — the same discipline running
through §§2-9, restated once here because it is the property that makes the whole pipeline
trustworthy rather than merely plausible.
