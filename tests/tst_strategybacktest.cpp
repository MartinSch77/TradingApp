// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The backtester (2026-08-12 redesign, item 7): runs a strategy against
// historical bars using the SAME PaperBook/BotConfig logic the live paper bot
// uses. These tests drive it with a small MOCK strategy that enters on a named
// day with a named stop — SwingPullbackStrategyV1's own entry conditions are
// already covered by tst_swingpullbackstrategy.cpp, so re-deriving a synthetic
// uptrend here would test numeric coincidence, not the backtest LOOP itself
// (walk-forward, sizing, the stop/exit mechanics, the cost-multiplier sweep).

#include "domain/StrategyBacktest.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {
// Enters exactly once, on `enterDay`, long, with a fixed stop distance —
// everything the backtest loop's own mechanics need to be exercised without
// depending on a real strategy's entry conditions lining up numerically.
class AlwaysEnterOnDay : public ITradingStrategy
{
public:
    AlwaysEnterOnDay(qint32 enterDay, double stopFraction)
        : m_enterDay(enterDay)
        , m_stopFraction(stopFraction)
    {
    }

    [[nodiscard]] StrategyDecision evaluate(const StrategySnapshot &snapshot) const override
    {
        StrategyDecision out;
        const qsizetype today = snapshot.bars.size() - 1;
        if (today == m_enterDay) {
            out.enter = true;
            out.isBuy = true;
            out.stopFraction = m_stopFraction;
        } else {
            out.code = QStringLiteral("not-the-day");
        }
        return out;
    }

    [[nodiscard]] QString version() const override { return QStringLiteral("test-always-enter"); }

private:
    qint32 m_enterDay;
    double m_stopFraction;
};

// A flat, gently-drifting series with an event injected at a chosen index via
// `edit` — flat by default so a stop/target check has a known, quiet baseline
// to deviate from.
QList<DailyBar> flatSeries(qint32 count, double price = 100.0)
{
    QList<DailyBar> bars;
    for (qint32 i = 0; i < count; ++i) {
        DailyBar b;
        b.open = price;
        b.high = price + 0.5;
        b.low = price - 0.5;
        b.close = price;
        bars.append(b);
    }
    return bars;
}
} // namespace

class TestStrategyBacktest : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-BT-001 @design DES-DOM-BACKTEST
    // @relation(REQ-F-031, scope=function)
    //
    // The conservative same-candle rule: a gap through the stop before the bar
    // even opens executes at the OPEN (the market never traded at the stale
    // stop price); a bar whose low merely reaches the stop executes AT the
    // stop; a bar that never comes near it is untouched.
    void TS_BT_001_barResolutionIsConservative()
    {
        DailyBar gapDown;
        gapDown.open = 90.0;
        gapDown.high = 91.0;
        gapDown.low = 89.0;
        gapDown.close = 90.5;
        const BarOutcome gapped = resolveBarAgainstStop(gapDown, 95.0);
        QVERIFY(gapped.stopHit);
        QCOMPARE(gapped.executionPrice, 90.0);   // the open, not the stale 95.0 stop

        DailyBar touchesStop;
        touchesStop.open = 100.0;
        touchesStop.high = 101.0;
        touchesStop.low = 94.0;
        touchesStop.close = 96.0;
        const BarOutcome touched = resolveBarAgainstStop(touchesStop, 95.0);
        QVERIFY(touched.stopHit);
        QCOMPARE(touched.executionPrice, 95.0);

        DailyBar untouched;
        untouched.open = 100.0;
        untouched.high = 101.0;
        untouched.low = 99.0;
        untouched.close = 100.5;
        QVERIFY(!resolveBarAgainstStop(untouched, 95.0).stopHit);

        // No stop set at all: nothing to check against.
        QVERIFY(!resolveBarAgainstStop(touchesStop, 0.0).stopHit);
    }

    //! @tstid TS-BT-002 @design DES-DOM-BACKTEST
    // @relation(REQ-F-031, scope=function)
    //
    // Walk-forward with no lookahead: the strategy is asked with EXACTLY the
    // bars known up to and including each day, never a bar past it — proven by
    // a mock strategy that enters only on the day it recognises the snapshot's
    // OWN length as "today".
    void TS_BT_002_walkForwardNeverLeaksFutureBars()
    {
        const AlwaysEnterOnDay strategy(30, 0.02);
        BacktestInput input;
        input.symbol = QStringLiteral("SPX500");
        input.bars = flatSeries(60);
        input.spreadPct = 0.02;
        input.leverage = 1;

        const BotConfig cfg;
        const SwingPullbackConfig exitCfg;
        const BacktestSummary summary = runBacktest(strategy, exitCfg, input, cfg);

        QCOMPARE(summary.days.size(), 60);
        for (qint32 i = 0; i < 30; ++i) {
            QVERIFY2(!summary.days[i].opened,
                     qPrintable(QStringLiteral("day %1 opened early").arg(i)));
        }
        QVERIFY(summary.days[30].opened);
    }

    //! @tstid TS-BT-003 @design DES-DOM-BACKTEST
    // @relation(REQ-F-031, scope=function)
    //
    // A stop-loss day fires the CONSERVATIVE bar resolution and closes the
    // position, freeing the book to consider a new entry afterward.
    void TS_BT_003_stopLossClosesAndFreesTheBook()
    {
        const AlwaysEnterOnDay strategy(5, 0.05);   // 5% stop distance
        BacktestInput input;
        input.symbol = QStringLiteral("SPX500");
        input.bars = flatSeries(20, 100.0);
        // Day 6 (the day after entry) gaps down through the 5% stop (95.0).
        input.bars[6].open = 90.0;
        input.bars[6].high = 90.5;
        input.bars[6].low = 89.0;
        input.bars[6].close = 89.5;
        input.spreadPct = 0.02;
        input.leverage = 1;

        const BotConfig cfg;
        const SwingPullbackConfig exitCfg;
        const BacktestSummary summary = runBacktest(strategy, exitCfg, input, cfg);

        QVERIFY(summary.days[5].opened);
        QVERIFY(summary.days[6].closed);
        QCOMPARE(summary.days[6].code, QStringLiteral("stop-loss"));
        // A LOSS: the account gave back money, gross before spread/rollover.
        QVERIFY(summary.stats.realized < 0.0);
        QCOMPARE(summary.stats.closedTrades, 1);
    }

    //! @tstid TS-BT-004 @design DES-DOM-BACKTEST
    // @relation(REQ-F-031, scope=function)
    //
    // Widening the assumed spread via spreadMultiplier, with every other input
    // identical, never IMPROVES the recorded result — the whole point of the
    // design's 1x/1.5x/2x cost sweep is to see how much a wrong cost estimate
    // would have cost, and a sweep that could show a HIGHER cost as better
    // would be measuring nothing.
    void TS_BT_004_widerSpreadNeverImprovesTheResult()
    {
        const AlwaysEnterOnDay strategy(5, 0.05);
        BacktestInput base;
        base.symbol = QStringLiteral("SPX500");
        base.bars = flatSeries(20, 100.0);
        // A profitable exit: price drifts up after entry, no stop or dynamic
        // exit fires, so the position rides to the end of the series where
        // stats() reports it as an OPEN, marked position — spread is still the
        // only thing that differs between the two runs.
        for (qint32 i = 6; i < base.bars.size(); ++i) {
            base.bars[i].open = 100.0 + (i - 5) * 0.2;
            base.bars[i].close = base.bars[i].open + 0.1;
            base.bars[i].high = base.bars[i].close + 0.3;
            base.bars[i].low = base.bars[i].open - 0.3;
        }
        base.leverage = 1;

        BacktestInput cheap = base;
        cheap.spreadPct = 0.02;
        cheap.spreadMultiplier = 1.0;
        BacktestInput expensive = base;
        expensive.spreadPct = 0.02;
        expensive.spreadMultiplier = 2.0;

        const BotConfig cfg;
        const SwingPullbackConfig exitCfg;
        const BacktestSummary cheapRun = runBacktest(strategy, exitCfg, cheap, cfg);
        const BacktestSummary expensiveRun = runBacktest(strategy, exitCfg, expensive, cfg);

        QVERIFY(expensiveRun.stats.totalPnl <= cheapRun.stats.totalPnl);
        QVERIFY(expensiveRun.stats.costsPaid >= cheapRun.stats.costsPaid);
    }

    //! @tstid TS-BT-005 @design DES-DOM-BACKTEST
    // @relation(REQ-F-031, scope=function)
    //
    // A refused sizing (riskPerTrade switched off entirely, so
    // sizeByExplicitRisk has no risk budget to size a stake from at all)
    // leaves the day unopened and named, rather than opening at whatever
    // sizing happened to be computed.
    void TS_BT_005_refusedSizingLeavesTheDayNamed()
    {
        const AlwaysEnterOnDay strategy(5, 0.05);
        BacktestInput input;
        input.symbol = QStringLiteral("SPX500");
        input.bars = flatSeries(10, 100.0);
        input.spreadPct = 0.02;
        input.leverage = 1;

        BotConfig cfg;
        cfg.riskPerTrade = 0.0;   // no per-symbol override for SPX500 to fall back on either
        const SwingPullbackConfig exitCfg;
        const BacktestSummary summary = runBacktest(strategy, exitCfg, input, cfg);

        QVERIFY(!summary.days[5].opened);
        QCOMPARE(summary.days[5].code, QStringLiteral("sizing-refused"));
        QCOMPARE(summary.stats.closedTrades, 0);
        QCOMPARE(summary.stats.openTrades, 0);
    }
};

QTEST_GUILESS_MAIN(TestStrategyBacktest)
#include "tst_strategybacktest.moc"
