// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_TRADEPLAN_H
#define TRADINGAPP_DOMAIN_TRADEPLAN_H

#include "domain/Models.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// The trade planner: turns the raw signal inputs for ONE instrument into a
// complete, costed trade proposal — direction verdict, win probability, a
// 1..5 risk factor, a volatility-targeted leverage recommendation, stop-loss /
// take-profit levels, and the full cost bill (half-spread to open and close,
// overnight rollover, the ~3× weekend rollover) netted against the expected
// gain. Pure computation over its inputs — the UI gathers the data and renders
// the result. All model estimates, not guarantees.
namespace trading {

// Everything the planner needs, as plain data. Monetary inputs/outputs are in
// the DISPLAY currency (euro): the notional identity invest × leverage / rate
// makes the account↔display conversion cancel out, exactly as the trade
// panel's opening-cost estimate does.
struct PlanInput {
    QList<double> closes;      // hourly closes, oldest first (last = ~live)
    double price = 0.0;        // live price (0 → use the last close)
    qint32 dir = 0;            // forced side: +1 long / -1 short; 0 = let the ensemble pick
    double invest = 0.0;       // stake, display currency
    qint32 maxLeverage = 0;    // instrument cap (0 = unknown → cap at 20)
    QList<qint32> leverageSteps;  // allowed multipliers, ascending; empty → {1,2,5,10,20}
    qint32 horizonHours = 24;  // intended holding horizon (hourly bars = hours)
    double spreadPct = 0.0;    // live (ask−bid)/mid × 100; 0 = unknown → costs incomplete
    InstrumentFees fees;       // per-unit rollover fees (weekend = the one-off ~3× night)
    bool feesKnown = false;    // false → rollover left out of the bill (flagged)
    QDateTime now;             // "now" for the weekend-crossing check (invalid → skip)
    // Market context for the confidence trim and the risk factor.
    bool vixValid = false;
    double vix = 0.0;
    double vixChangePct = 0.0;
    bool eventRisk = false;    // a high-impact calendar event is imminent
    bool fgValid = false;      // a Fear & Greed reading is available
    double fearGreed = 50.0;   // CNN Fear & Greed index, 0 (fear) .. 100 (greed)
    quint32 mcSeed = 0;        // nonzero → fixed Monte-Carlo seed (tests); 0 → a seed
                               // derived from closes+price, so identical inputs give
                               // identical plans (no sampling-noise verdict flips)
};

struct TradePlan {
    bool valid = false;        // false = series too short to plan
    qint32 dir = 0;            // evaluated side: +1 long / -1 short (0 = none available)
    QString verdict;           // "BUY" / "SELL" / "STAY OUT"
    QString verdictReason;     // one line explaining a STAY OUT (empty when actionable)
    double confidence = 0.0;   // ensemble confidence after the VIX haircut, 0..100
    double pWin = 0.0;         // P(take-profit struck first), Monte-Carlo, 0..1
    double pLose = 0.0;        // P(stop-loss struck first); the remainder expires between
    double breakeven = 0.0;    // win-rate the reward:risk needs to break even, 0..1
    qint32 riskFactor = 1;     // 1 (low) .. 5 (very high)
    QStringList riskNotes;     // what drove the risk factor up
    qint32 leverage = 1;       // recommended multiplier (vol-targeted, ≤ instrument max)
    double marginSwingPct = 0.0;  // expected 1h price move × leverage, % of the stake
    double slAmount = 0.0;     // proposed stop-loss loss amount (display currency)
    double tpAmount = 0.0;     // proposed take-profit gain amount
    double slRate = 0.0;       // the price levels those amounts correspond to
    double tpRate = 0.0;
    // The cost bill for holding this position over the horizon (display currency).
    double openCost = 0.0;     // half-spread crossed on opening
    double closeCost = 0.0;    // half-spread crossed on closing
    double feePerNight = 0.0;  // rollover per ordinary night (negative = credit)
    double weekendFee = 0.0;   // the one-off weekend rollover, when the horizon crosses it
    qint32 nights = 0;         // nights the horizon spans
    bool crossesWeekend = false;
    bool costsComplete = false;  // false = spread and/or fees unknown → bill is partial
    // Expected value of the proposal (display currency): win/lose the TP/SL
    // amounts at the Monte-Carlo win rate, minus the cost bill.
    double expectedGross = 0.0;
    double expectedCosts = 0.0;
    double expectedNet = 0.0;
};

// Stop-loss / take-profit distances from recent volatility: the stop sits
// ~1.5σ of the horizon's expected move away (noise shouldn't hit it), the
// take-profit 1.5× further (reward:risk 1.5). Returned as a fraction of price.
[[nodiscard]] double proposedSlFraction(double volPctPerBar, qint32 horizonHours) noexcept;

// The largest allowed multiplier that keeps the loss-at-stop within
// riskBudgetFrac of the stake (default: quarter of the stake), stepped down to
// the instrument's allowed leverage values.
[[nodiscard]] qint32 recommendLeverage(double slFrac, double riskBudgetFrac,
                                       qint32 maxLeverage, const QList<qint32> &steps);

[[nodiscard]] TradePlan buildTradePlan(const PlanInput &in);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_TRADEPLAN_H
