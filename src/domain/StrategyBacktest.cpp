// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/StrategyBacktest.h"

#include "domain/Indicators.h"

#include <QTimeZone>

#include <algorithm>

namespace trading {

namespace {
// The bars available up to and including day `i` — walk-forward, no lookahead.
QList<double> closesUpTo(const QList<DailyBar> &bars, qint32 i)
{
    QList<double> closes;
    closes.reserve(i + 1);
    for (qint32 j = 0; j <= i; ++j) {
        closes.append(bars[j].close);
    }
    return closes;
}

// The lowest LOW of the last 5 bars up to and including day i (fewer than 5
// available near the start simply uses what there is).
double fiveDayLowOf(const QList<DailyBar> &bars, qint32 i)
{
    const qint32 first = std::max(0, i - 4);
    double low = bars[first].low;
    for (qint32 j = first; j <= i; ++j) {
        low = std::min(low, bars[j].low);
    }
    return low;
}

// Everything the per-day steps below share, bundled so each stays under the
// parameter-count budget.
struct BacktestContext {
    PaperBook &book;
    const BacktestInput &input;
    const BotConfig &cfg;
    const SwingPullbackConfig &exitConfig;
    double effectiveSpread = 0.0;
};

// The one open position's own bookkeeping the strategy interface does not
// carry — kept outside PaperTrade so this backtester's state does not leak
// into the shared struct every other caller of PaperBook also uses.
struct RunState {
    qint64 openId = 0;
    SwingPositionState pos;
    double entryPrice = 0.0;
    double initialStop = 0.0;
};

// The exit half of one day: the stop first (conservative same-candle
// resolution), then the strategy's own dynamic exit rules. Split out of
// runBacktest purely to keep its own McCabe complexity within budget.
BacktestDayResult stepExit(BacktestContext &ctx, RunState &rs, qint32 i, const QDateTime &now)
{
    BacktestDayResult day;
    day.dayIndex = i;

    const BarOutcome stopOutcome = resolveBarAgainstStop(ctx.input.bars[i], rs.pos.stopPrice);
    if (stopOutcome.stopHit) {
        ctx.book.close(rs.openId, stopOutcome.executionPrice, ctx.effectiveSpread,
                       CloseReason::StopLoss, now);
        day.closed = true;
        day.code = QStringLiteral("stop-loss");
        rs.openId = 0;
        return day;
    }

    const QList<double> closes = closesUpTo(ctx.input.bars, i);
    const double ema10 = emaSeries(closes, 10).last();
    const SwingExitInputs exitIn{rs.entryPrice, rs.initialStop, ctx.input.bars[i].close,
                                 fiveDayLowOf(ctx.input.bars, i), ema10};
    const SwingExitAction action = swingExitDecision(rs.pos, exitIn, ctx.exitConfig);
    rs.pos = action.nextState;
    day.code = action.code;

    if (action.fullClose) {
        ctx.book.close(rs.openId, ctx.input.bars[i].close, ctx.effectiveSpread,
                       CloseReason::AiExit, now);
        day.closed = true;
        rs.openId = 0;
    } else if (action.partialClose) {
        const PaperBook::ExitPricing pricing{ctx.input.bars[i].close, ctx.effectiveSpread,
                                             CloseReason::TakeProfit, now};
        ctx.book.partialClose(rs.openId, action.partialFraction, pricing);
        day.partialClosed = true;
    }
    return day;
}

// The entry half of one day: ask the strategy, size by explicit risk, open if
// both agree there is a trade worth taking. Split out of runBacktest purely to
// keep its own McCabe complexity within budget.
BacktestDayResult stepEntry(BacktestContext &ctx, RunState &rs, const ITradingStrategy &strategy,
                            qint32 i, const QDateTime &now)
{
    BacktestDayResult day;
    day.dayIndex = i;

    StrategySnapshot snap;
    snap.symbol = ctx.input.symbol;
    snap.bars = ctx.input.bars.first(i + 1);   // walk-forward: only what is known so far
    const StrategyDecision decision = strategy.evaluate(snap);
    day.code = decision.code.isEmpty() ? QStringLiteral("no-signal") : decision.code;
    if (!decision.enter) {
        return day;
    }

    const double fillPrice = ctx.input.bars[i].close;
    const double stopPrice = fillPrice * (1.0 - decision.stopFraction);
    const double riskPerTrade = riskPerTradeFor(ctx.cfg, ctx.input.symbol);
    const ExplicitRiskSizing sizing = sizeByExplicitRisk(
        ctx.book.state().equity, riskPerTrade, decision.stopFraction, ctx.input.leverage);
    if (!sizing.valid) {
        day.code = QStringLiteral("sizing-refused");
        return day;
    }

    EntrySignal sig;
    sig.valid = true;
    sig.symbol = ctx.input.symbol;
    sig.isBuy = decision.isBuy;
    sig.fillRate = fillPrice;
    sig.spreadPct = ctx.effectiveSpread;
    sig.leverage = ctx.input.leverage;
    sig.slRate = stopPrice;
    const qint64 id = ctx.book.open(sig, sizing.margin, now);
    if (id == 0) {
        day.code = QStringLiteral("open-refused");
        return day;
    }
    rs.openId = id;
    rs.pos = SwingPositionState{};
    rs.pos.stopPrice = stopPrice;
    rs.entryPrice = fillPrice;
    rs.initialStop = stopPrice;
    day.opened = true;
    day.code = QStringLiteral("entered");
    return day;
}
} // namespace

BarOutcome resolveBarAgainstStop(const DailyBar &bar, double stopPrice)
{
    BarOutcome out;
    if (stopPrice <= 0.0) {
        return out;   // no stop to check against
    }
    if (bar.open <= stopPrice) {
        // A gap through the stop before the bar even opens: the market never
        // traded at the stale stop price, so the fill is at the open.
        out.stopHit = true;
        out.executionPrice = bar.open;
        return out;
    }
    if (bar.low <= stopPrice) {
        out.stopHit = true;
        out.executionPrice = stopPrice;
    }
    return out;
}

BacktestSummary runBacktest(const ITradingStrategy &strategy, const SwingPullbackConfig &exitConfig,
                            const BacktestInput &input, const BotConfig &cfg)
{
    BacktestSummary summary;
    PaperBook book(cfg);
    RunState rs;
    BacktestContext ctx{book, input, cfg, exitConfig, input.spreadPct * input.spreadMultiplier};

    // A backtest has no real calendar date per bar (DailyBar carries none) —
    // an arbitrary but strictly ADVANCING clock is enough for PaperBook's own
    // day-ledger bookkeeping (rollover nights, the daily target/loss rules) to
    // behave sensibly; it is not meant to be read as real dates.
    const QDateTime epoch(QDate(2000, 1, 1), QTime(0, 0), QTimeZone::UTC);

    for (qint32 i = 0; i < input.bars.size(); ++i) {
        const QDateTime now = epoch.addDays(i);
        BacktestDayResult day = (rs.openId != 0) ? stepExit(ctx, rs, i, now)
                                                 : stepEntry(ctx, rs, strategy, i, now);
        summary.days.append(day);
    }
    summary.stats = book.stats();
    return summary;
}

} // namespace trading
