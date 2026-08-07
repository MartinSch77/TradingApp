# Prediction strategy — what is measured, what is reachable, what is not

@page predictionstrategy Prediction strategy: signals, validation discipline, and the swing setup
@tableofcontents

**A perfect prediction of NSDQ100 or SPX500 is impossible.** This page is deliberately
built on that sentence, because every design decision below follows from it. The realistic
objective is:

> Estimate the probability, the expected range and the confidence of a move — and output
> **NO TRADE** when the evidence is weak.

```
NSDQ100, next 15 minutes
P(up):             UNCALIBRATED (11 of 40 samples)
Expected range:    -0.24% … +0.51%
Market regime:     high-volatility trend
Decision:          NO TRADE — no-confluence (4 of 9 reads measured)
Invalidation:      price below session VWAP
```

That the probability line reads UNCALIBRATED rather than "68%" is the point, not a
shortcoming: REQ-F-037 forbids printing a percentage that was not measured.

## The signal inventory, split by what it costs

The honest division is not "good signals vs bad signals" but **implemented / reachable /
unreachable with the data this application can obtain**.

### Already implemented and measured

| Signal | Where |
|---|---|
| Futures leadership (NQ vs ES) | `futuresLeadRead`, REQ-F-035 |
| The leading future's push over 1/5/15 min | one read, since three horizons off one series are one piece of evidence |
| Volatility **direction** (^VXN / ^VIX) | `volatilityRead` |
| Volatility **term structure** (^VIX9D vs ^VIX3M) | a regime damper, never a direction |
| US 10-year, and the **curve** (2YY=F else ^IRX vs ^TNX) | `yieldRead`, `curveRead` |
| Heavyweight participation, and how many are above their own session VWAP | `participationRead`, `aboveVwapRead` |
| Whether session **volume** sits behind the rising or falling names | `upDownVolumeRead` |
| Opening range and where price sits against it | `openingRange`, REQ-F-022 |
| Scheduled events, the policy window, session phase on the instrument's own clock | `sessionPhaseFor` |
| Calibration against baselines + Brier score, UNCALIBRATED below the sample floor | `PredictionLedger`, REQ-F-037 |
| Cost-inclusive record: half-spread per side, rollover, tripled weekend night | `PaperTrader` |
| An ML model with a **time-based** split and an AUC gate | `BotNet`, REQ-F-033 |

That is most of a sensible feature set already, and the parts that are usually done badly —
calibration, baselines, costs, chronological splits — are the parts already done.

### Reachable, not yet built

- **Anchored walk-forward evaluation over the existing ledger**: strictly chronological,
  purging around overlapping horizons, results split by regime and by event day. This needs
  no new feed — it is analysis code over data already being recorded.
- **A quantile model** for the expected range, rather than only a direction.
- **Separate models per horizon.** One model asked to predict 1 minute and one session is
  two different problems wearing one hat.
- **FRED/ALFRED vintages.** Training on a *revised* macro figure that was not available at
  the historical decision time is look-ahead leakage; ALFRED exists precisely to avoid it.
- **LightGBM/XGBoost in Python, ONNX Runtime for inference in C++.** The right split:
  training where the tooling lives, inference and risk control where the money is.

### Unreachable with eToro data — and not faked

The application states these absences rather than approximating them, which is why they
belong in a strategy document:

- **Order flow**: volume delta, CVD, bid/ask imbalance, absorption, liquidity withdrawal.
  Needs CME level-2; eToro provides one bid/ask with no sizes.
- **True market breadth**: advance/decline, up-volume, share of *all* constituents above
  VWAP. Needs per-constituent data this app does not fetch. Heavyweight participation is
  labelled a STAND-IN everywhere it appears.
- **Nasdaq TotalView** depth and auction imbalance; **NYSE TICK**.
- **Options**: SPX skew, 0DTE volume, VIX1D, and dealer-gamma estimates — the last of which
  is an *estimate built on assumptions about dealer positioning*, not an observation, and
  should never be presented as one.

## The binding constraint is sample count, not algorithm

This is the most important finding and the least glamorous. Measured on the live ledger:

| | |
|---|---|
| Decisions recorded | **286** |
| Positions actually opened | **3** |
| Closed trades | **3** (OIL.24-7 ×2, Semiconductors ×1) |

LightGBM, quantile models, walk-forward validation and calibration are all correct answers
that are **untrainable and unvalidatable at n = 3**. Swapping in a stronger learner now
would produce a confident number with nothing behind it — exactly what
`paperLiveReadiness` and `kMinSamplesPerBucket` exist to prevent.

So the ordering is: **let the ledger fill**, then build walk-forward evaluation over it,
then reach for a stronger model. Not the reverse.

## Validation matters more than the algorithm

What the app should measure, beyond directional accuracy:

- profit **after all costs** — spread, slippage, financing, currency conversion
- maximum drawdown, profit factor, expectancy
- **calibration**: does a stated 70% actually happen ~70% of the time?
- results per **regime** and separately on **event days**
- the share of opportunities correctly rejected as NO TRADE
- comparison against trivial baselines on the same samples

A 53% system that is well calibrated with sound risk/reward is useful. A claimed 75% system
produced by leakage or selective backtesting is worth less than nothing, because it will be
believed.

## A concrete swing setup (SPX500 first, NSDQ100 second)

A trend–pullback continuation trade held roughly 3–10 sessions — more robust than trying to
catch turns. **Not implemented**; specified here so it can be, and so that it goes through
`requirements/requirements.sdoc` when it does rather than appearing as behaviour nobody
asked for.

**1. Regime filter (long).** Daily close above the 200-day EMA; EMA20 > EMA50 > EMA200;
50-day EMA rising; breadth positive or improving; VIX term structure not strongly inverted;
no CPI/NFP/FOMC imminent. Any failure → NO TRADE.

**2. A controlled pullback.** 2–5 sessions down, roughly 1–2 daily ATRs, toward the 20-day
EMA / prior breakout / anchored VWAP, still above the 50-day EMA, with *declining* selling
volume — not panic liquidation, and not alongside a spiking VIX and collapsing breadth.

**3. Entry trigger.** A one-hour higher low; close above the previous day's high; >55% of
constituents above intraday VWAP; NQ and ES confirming each other; and the model giving at
least 60% **calibrated** probability of +1R before −1R. Buy-stop just above the signal
candle, cancelled on a large gap.

**4. Stop and size.** Stop below the pullback low − 0.2 × daily ATR. Risk 0.25–0.50% of
capital, sized as `permitted loss ÷ (entry − stop + costs)`. At €50,000 and 0.4%, a €200
risk over a 2.50 stop distance is 80 units.

**5. Exits.** Take 40–50% at +2R; trail the rest under the 5-day low or 10-day EMA; exit
fully on a daily close below the original support; time-stop after five sessions below
+0.5R; ten sessions maximum. **Never widen a stop to avoid taking a loss.**

At 2R winners against 1R losers this does not need a high win rate — under 50% can be
profitable before costs, which is why the cost accounting is not optional.

### A ten-point setup score

| Component | Points |
|---|---|
| Daily trend | 0–2 |
| Pullback quality | 0–2 |
| Breadth confirmation | 0–2 |
| Volatility structure | 0–1 |
| Interest-rate environment | 0–1 |
| NQ/ES and sector confirmation | 0–1 |
| Event risk | 0–1 |

0–5 NO TRADE · 6–7 watchlist · 8 valid, reduced size · 9–10 normal risk. **"High quality"
never means exceeding the predefined maximum risk** — the score gates entry, never size
beyond the cap.

### SPX500 vs NSDQ100

| | SPX500 | NSDQ100 |
|---|---|---|
| Build and validate first | yes | second |
| Volatility | lower | higher |
| Yield sensitivity | moderate | high |
| Risk allocation | full | ~60–75% of SPX500 |
| Overnight gap risk | lower | higher |

**On leverage:** ×20 is wrong for a multi-day hold. Overnight gaps pass straight through a
stop, and leveraged ETFs reset daily so multi-day performance diverges from the advertised
multiple. Micro futures (MES $5 ×, MNQ $2 ×) allow precise sizing, but a small margin
requirement is not a small risk.

Nothing here becomes a real-money recommendation until it has survived walk-forward testing
on each index separately with realistic spread, financing and slippage, followed by unseen
paper trading — the discipline REQ-F-031's live gate already enforces.
