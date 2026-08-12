// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The swing-pullback strategy (2026-08-12 redesign, item 5): only trades WITH an
// established daily uptrend, on a shallow, controlled pullback that has just turned
// back up. Every gate below is a way a naive "buy the dip" rule could fire on the
// wrong dip, which is exactly what this strategy exists to refuse.

#include "domain/SwingPullbackStrategy.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {
// A steep uptrend for the bulk of the history (so EMA200 sits well below price),
// then a slow, flattening rise for the tail (so EMA20 has time to converge close
// to price) — the two-phase shape a real "buy the pullback to EMA20" setup needs,
// since the fast EMA cannot catch a steeply-rising price within a shallow pullback.
QList<DailyBar> uptrendWithPullback(qint32 steepDays, double steepSlope,
                                    qint32 flatDays, double flatSlope,
                                    const QList<double> &pullbackDeltas)
{
    QList<DailyBar> bars;
    double price = 100.0;
    auto addBar = [&bars](double close) {
        DailyBar b;
        b.open = close - 0.1;
        b.high = close + 0.3;
        b.low = close - 0.3;
        b.close = close;
        bars.append(b);
    };
    for (qint32 i = 0; i < steepDays; ++i) {
        price += steepSlope;
        addBar(price);
    }
    for (qint32 i = 0; i < flatDays; ++i) {
        price += flatSlope;
        addBar(price);
    }
    // The pullback and confirmation days, as explicit deltas off the peak — the
    // caller states exactly what each test scenario needs.
    for (const double delta : pullbackDeltas) {
        addBar(price + delta);
    }
    return bars;
}
} // namespace

class TestSwingPullbackStrategy : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-SWING-001 @design DES-DOM-SWING
    // @relation(REQ-F-031, scope=function)
    //
    // The full happy path: an established uptrend, a controlled 3-session pullback
    // that reaches the fast EMA, confirmed today by a higher low — enters long, with
    // a stop below the pullback's own low.
    void TS_SWING_001_confirmedPullbackToEma20Enters()
    {
        // 150 steep days (100 -> 175), 50 flattening days (175 -> 180, so EMA20
        // has time to converge close to price), then a 3-session pullback of
        // roughly the fast EMA's lag off the peak, confirmed by a higher low.
        const QList<DailyBar> bars =
            uptrendWithPullback(150, 0.5, 100, 0.1, {-1.0, -2.0, -3.0, -2.5});

        SwingPullbackConfig cfg;
        // A generous proximity so the test is about the STRATEGY'S LOGIC (trend,
        // session count, confirmation), not about hand-computing where a lagging
        // EMA sits to the cent — that arithmetic is exercised directly in
        // TS-SWING-004 below with a config built to fail it on purpose.
        cfg.emaProximityAtr = 20.0;
        const SwingPullbackStrategyV1 strategy(cfg);

        StrategySnapshot snap;
        snap.symbol = QStringLiteral("SPX500");
        snap.bars = bars;
        const StrategyDecision decision = strategy.evaluate(snap);
        QVERIFY2(decision.enter, qPrintable(decision.why));
        QVERIFY(decision.isBuy);
        QVERIFY(decision.stopFraction > 0.0);
        QVERIFY(decision.stopFraction < 0.05);   // a sane swing stop, not a wild one
        QVERIFY(decision.code.isEmpty());
        QCOMPARE(strategy.version(), QStringLiteral("swing-pullback-v1"));
    }

    //! @tstid TS-SWING-002 @design DES-DOM-SWING
    // @relation(REQ-F-031, scope=function)
    //
    // No uptrend, no trade — a flat/declining series never satisfies EMA20 > EMA50
    // > EMA200 above price, whatever else about it might look like a pullback.
    void TS_SWING_002_noUptrendRefuses()
    {
        QList<DailyBar> bars = uptrendWithPullback(150, 0.5, 100, 0.1, {-1.0, -2.0, -3.0, -2.5});
        // Flatten the whole series into a range instead of an uptrend.
        for (qsizetype i = 0; i < bars.size(); ++i) {
            bars[i].close = 150.0 + ((i % 5 == 0) ? 1.0 : -1.0);
            bars[i].high = bars[i].close + 0.3;
            bars[i].low = bars[i].close - 0.3;
        }
        const SwingPullbackStrategyV1 strategy;
        const StrategyDecision decision = strategy.evaluate(StrategySnapshot{
            QStringLiteral("SPX500"), bars, TermStructure{}, false});
        QVERIFY(!decision.enter);
        QCOMPARE(decision.code, QStringLiteral("no-uptrend"));
    }

    //! @tstid TS-SWING-003 @design DES-DOM-SWING
    // @relation(REQ-F-031, scope=function)
    //
    // A strongly inverted VIX term structure refuses even an otherwise-perfect
    // setup, and an imminent scheduled release refuses independently of it.
    void TS_SWING_003_regimeAndEventRiskRefuse()
    {
        const QList<DailyBar> bars =
            uptrendWithPullback(150, 0.5, 100, 0.1, {-1.0, -2.0, -3.0, -2.5});
        SwingPullbackConfig cfg;
        cfg.emaProximityAtr = 20.0;
        const SwingPullbackStrategyV1 strategy(cfg);

        TermStructure inverted;
        inverted.known = true;
        inverted.nearFarRatio = 1.20;   // above the 1.05 default ceiling
        const StrategyDecision viaVix =
            strategy.evaluate(StrategySnapshot{QStringLiteral("SPX500"), bars, inverted, false});
        QVERIFY(!viaVix.enter);
        QCOMPARE(viaVix.code, QStringLiteral("vix-inverted"));

        const StrategyDecision viaEvent = strategy.evaluate(
            StrategySnapshot{QStringLiteral("SPX500"), bars, TermStructure{}, true});
        QVERIFY(!viaEvent.enter);
        QCOMPARE(viaEvent.code, QStringLiteral("event-risk"));
    }

    //! @tstid TS-SWING-004 @design DES-DOM-SWING
    // @relation(REQ-F-031, scope=function)
    //
    // A pullback that never reaches the fast EMA is refused — an essentially
    // impossible proximity tolerance (0.0001 ATR) turns even the same otherwise-
    // qualifying setup used in TS-SWING-001 into a refusal, pinning that this gate
    // is load-bearing rather than a no-op.
    void TS_SWING_004_pullbackTooShallowOrDeepRefuses()
    {
        const QList<DailyBar> bars =
            uptrendWithPullback(150, 0.5, 100, 0.1, {-1.0, -2.0, -3.0, -2.5});
        SwingPullbackConfig cfg;
        cfg.emaProximityAtr = 0.0001;
        const SwingPullbackStrategyV1 strategy(cfg);
        const StrategyDecision decision = strategy.evaluate(
            StrategySnapshot{QStringLiteral("SPX500"), bars, TermStructure{}, false});
        QVERIFY(!decision.enter);
        QCOMPARE(decision.code, QStringLiteral("pullback-too-shallow-or-deep"));
    }

    //! @tstid TS-SWING-005 @design DES-DOM-SWING
    // @relation(REQ-F-031, scope=function)
    //
    // Session-count bounds: a one-session dip is too short (never counted as a
    // real pullback) and a seven-session decline has run longer than the design's
    // 5-session ceiling — both refused, distinctly, by name.
    void TS_SWING_005_pullbackSessionBoundsAreEnforced()
    {
        SwingPullbackConfig cfg;
        cfg.emaProximityAtr = 20.0;
        const SwingPullbackStrategyV1 strategy(cfg);

        const QList<DailyBar> tooShort =
            uptrendWithPullback(150, 0.5, 100, 0.1, {-1.0, -0.5});   // 1 down day, then confirms
        const StrategyDecision shortDecision = strategy.evaluate(
            StrategySnapshot{QStringLiteral("SPX500"), tooShort, TermStructure{}, false});
        QVERIFY(!shortDecision.enter);
        QCOMPARE(shortDecision.code, QStringLiteral("no-pullback"));

        const QList<DailyBar> tooLong = uptrendWithPullback(
            150, 0.5, 100, 0.1,
            {-0.5, -1.0, -1.5, -2.0, -2.5, -3.0, -3.5, -3.0});   // 7 down days, then confirms
        const StrategyDecision longDecision = strategy.evaluate(
            StrategySnapshot{QStringLiteral("SPX500"), tooLong, TermStructure{}, false});
        QVERIFY(!longDecision.enter);
        QCOMPARE(longDecision.code, QStringLiteral("pullback-too-long"));
    }

    //! @tstid TS-SWING-006 @design DES-DOM-SWING
    // @relation(REQ-F-031, scope=function)
    //
    // A pullback that has not yet turned back up — today merely the smallest of
    // the down days, neither a higher low nor a close above yesterday's high —
    // is refused: the reversal has to show itself today, not be inferred from the
    // pullback merely having gotten shallower.
    void TS_SWING_006_noConfirmationRefuses()
    {
        // Every day, including "today", closes lower than the last — the pullback
        // never turns, so neither confirmation condition can be true.
        const QList<DailyBar> bars =
            uptrendWithPullback(150, 0.5, 100, 0.1, {-1.0, -2.0, -3.0});
        SwingPullbackConfig cfg;
        cfg.emaProximityAtr = 20.0;
        const SwingPullbackStrategyV1 strategy(cfg);
        const StrategyDecision decision = strategy.evaluate(
            StrategySnapshot{QStringLiteral("SPX500"), bars, TermStructure{}, false});
        QVERIFY(!decision.enter);
        // Today (the last -3.0 day) is itself still part of the decline, so the
        // walk-back sees a 4-session pullback with today not yet turning up —
        // confirmation fails before the EMA-proximity check is even reached,
        // since today's own low/close cannot exceed yesterday's.
        QCOMPARE(decision.code, QStringLiteral("no-confirmation"));
    }

    //! @tstid TS-SWING-007 @design DES-DOM-SWING
    // @relation(REQ-F-031, scope=function)
    //
    // Too little daily history (short of EMA200 plus the pullback/ATR windows)
    // refuses outright rather than computing an EMA200 that is mostly padding.
    void TS_SWING_007_insufficientHistoryRefuses()
    {
        QList<DailyBar> bars;
        for (qint32 i = 0; i < 50; ++i) {
            DailyBar b;
            b.open = 100.0;
            b.high = 100.5;
            b.low = 99.5;
            b.close = 100.0 + (i * 0.1);
            bars.append(b);
        }
        const SwingPullbackStrategyV1 strategy;
        const StrategyDecision decision = strategy.evaluate(
            StrategySnapshot{QStringLiteral("SPX500"), bars, TermStructure{}, false});
        QVERIFY(!decision.enter);
        QCOMPARE(decision.code, QStringLiteral("insufficient-history"));
    }
};

QTEST_GUILESS_MAIN(TestSwingPullbackStrategy)
#include "tst_swingpullbackstrategy.moc"
