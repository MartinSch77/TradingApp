// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_STRATEGYBACKTEST_H
#define TRADINGAPP_DOMAIN_STRATEGYBACKTEST_H

#include "domain/PaperTrader.h"
#include "domain/SwingPullbackStrategy.h"
#include "domain/TradingStrategy.h"

// A BACKTESTER (2026-08-12 redesign, item 7) — deliberately NOT named "Market
// Replay" even though the redesign's own wording calls this feature that:
// docs/roadmap.md already uses "Market Replay" for an UNRELATED, already-planned
// feature — a QML time-slider VIEWER over prediction-ledger.jsonl's own
// already-recorded LIVE sessions (inspection/showcase: "what did the app know
// at this point in a session that actually ran"). This module instead runs a
// strategy against HISTORICAL bars it was never live for. Same word, two
// different features; naming this one distinctly avoids the collision.
//
// Runs the SAME PaperBook/BotConfig booking and risk logic the live paper bot
// uses — the design's own explicit requirement, since a separate backtest
// implementation would sooner or later disagree with the live one for no
// principled reason.
namespace trading {

// Deterministic resolution of what a single OHLC bar cannot itself answer: the
// ORDER in which the stop was reached within the bar. Conservative per the
// design: a stop below a gap-down OPEN executes at the open itself (the
// market never actually traded at the stale stop price); otherwise a bar
// whose low reaches the stop is assumed to have hit it, long-only (matching
// SwingPullbackStrategyV1's own scope).
struct BarOutcome {
    bool stopHit = false;
    double executionPrice = 0.0;   // meaningful only when stopHit
};
[[nodiscard]] BarOutcome resolveBarAgainstStop(const DailyBar &bar, double stopPrice);

// What one backtest run needs: the bars to replay, and the conservative,
// explicitly-stated cost assumptions the design calls for — including a
// MULTIPLIER on the spread so a run can be repeated at 1x/1.5x/2x the assumed
// cost to see how sensitive the result is to a wrong assumption, rather than
// reporting one number that quietly assumes the cost estimate was exact.
struct BacktestInput {
    QString symbol;
    QList<DailyBar> bars;             // oldest first
    double spreadPct = 0.0;
    double spreadMultiplier = 1.0;    // the design's own cost-sensitivity sweep
    InstrumentFees fees;
    bool feesKnown = false;
    double eurPerUsd = 1.0;
    qint32 leverage = 1;              // "x1 at first" — the design's own starting point
};

// One evaluated day's outcome — every day the strategy was ASKED, not just the
// ones it traded, so a no-signal day is visible in the record exactly like
// PredictionLedger already keeps every evaluation (item 8 of the redesign
// builds on this; this backtester does not itself write to the ledger).
struct BacktestDayResult {
    qint32 dayIndex = 0;
    bool opened = false;
    bool partialClosed = false;
    bool closed = false;
    QString code;   // the strategy's/exit rule's own code: entry, refusal, or exit reason
};

struct BacktestSummary {
    QList<BacktestDayResult> days;
    PaperStats stats;   // the SAME performance measure paperPerformance/stats() reports live
};

// Walk-forward only: day i's entry decision sees bars[0..i], never bars[i+1..]
// — no lookahead. At most one open position at a time. Entries are sized by
// the explicit risk-per-trade model (sizeByExplicitRisk) at input.leverage;
// exits are decided by swingExitDecision plus the conservative same-candle
// stop resolution above. Coupled to SwingPullbackStrategyV1's own exit model
// (swingExitDecision, SwingPullbackConfig) rather than a strategy-agnostic
// exit interface — a stated V1 simplification to generalise once a second
// strategy exists, not an oversight.
[[nodiscard]] BacktestSummary runBacktest(const ITradingStrategy &strategy,
                                          const SwingPullbackConfig &exitConfig,
                                          const BacktestInput &input, const BotConfig &cfg);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_STRATEGYBACKTEST_H
