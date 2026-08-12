// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Path-dependent decision labelling (2026-08-12 redesign, item 8): a label is
// decided by the PATH the price actually took, never by an endpoint question.

#include "domain/PathOutcome.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {
DailyBar barAt(double high, double low, double close)
{
    DailyBar b;
    b.open = close;
    b.high = high;
    b.low = low;
    b.close = close;
    return b;
}

Prediction predictionAt(const QDateTime &at, const QString &symbol, double price)
{
    Prediction p;
    p.at = at;
    p.symbol = symbol;
    p.price = price;
    p.dir = 1;
    return p;
}
} // namespace

class TestPathOutcome : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-PATH-001 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // A path that rises to +1R without ever touching -1R labels Long; the
    // mirror (falls to -1R-equivalent for a short without touching its stop)
    // labels Short.
    void TS_PATH_001_longAndShortWinsAreLabelledCorrectly()
    {
        PathOutcomeConfig cfg;
        cfg.stopFraction = 0.05;   // R = 5% of entry (100 -> stop 95 / target 105)

        const QList<DailyBar> risesToTarget{barAt(106.0, 99.0, 105.5)};
        QCOMPARE(resolvePathLabel(risesToTarget, 100.0, cfg), PathLabel::Long);

        const QList<DailyBar> fallsToShortTarget{barAt(101.0, 94.0, 94.5)};
        QCOMPARE(resolvePathLabel(fallsToShortTarget, 100.0, cfg), PathLabel::Short);

        QCOMPARE(pathLabelWord(PathLabel::Long), QStringLiteral("long"));
        QCOMPARE(pathLabelWord(PathLabel::Short), QStringLiteral("short"));
        QCOMPARE(pathLabelWord(PathLabel::NoTrade), QStringLiteral("no-trade"));
    }

    //! @tstid TS-PATH-002 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // Neither side ever reaches its own target within the horizon (a path
    // that chops inside the stop/target band the whole way) labels NoTrade —
    // "neither side covered its own costs and minimum move".
    void TS_PATH_002_aPathThatNeverResolvesIsNoTrade()
    {
        PathOutcomeConfig cfg;
        cfg.stopFraction = 0.05;
        cfg.maxBars = 5;

        QList<DailyBar> chop;
        for (int i = 0; i < 5; ++i) {
            chop.append(barAt(102.0, 98.0, 100.0));   // inside (95, 105) every day
        }
        QCOMPARE(resolvePathLabel(chop, 100.0, cfg), PathLabel::NoTrade);
    }

    //! @tstid TS-PATH-003 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // Same-candle ambiguity: a bar wide enough to cover BOTH a direction's own
    // stop and its own target is resolved conservatively — the stop is
    // assumed hit FIRST, the same rule StrategyBacktest's own
    // resolveBarAgainstStop uses for a live position.
    void TS_PATH_003_sameCandleAmbiguityAssumesTheStopFirst()
    {
        PathOutcomeConfig cfg;
        cfg.stopFraction = 0.05;   // long: stop 95, target 105

        // One wild bar spanning both levels.
        const QList<DailyBar> wild{barAt(110.0, 90.0, 100.0)};
        const DirectionalPathResult longResult = walkPath(wild, 100.0, true, cfg);
        QVERIFY(!longResult.reachedTargetFirst);
        QCOMPARE(longResult.barsToResolve, 1);
    }

    //! @tstid TS-PATH-004 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // costBufferFraction inflates the target distance: a move that clears
    // the raw R target but not the cost-inclusive one does not count as a
    // win — exactly the design's "covered costs AND minimum movement"
    // requirement, not just the nominal R.
    void TS_PATH_004_costBufferMustAlsoBeCleared()
    {
        PathOutcomeConfig noBuffer;
        noBuffer.stopFraction = 0.05;   // raw target 105
        const QList<DailyBar> reachesOnlyNominalTarget{barAt(106.0, 99.0, 105.5)};
        QCOMPARE(resolvePathLabel(reachesOnlyNominalTarget, 100.0, noBuffer), PathLabel::Long);

        PathOutcomeConfig withBuffer = noBuffer;
        withBuffer.costBufferFraction = 0.03;   // target now 108 — 106 does not clear it
        withBuffer.maxBars = 1;
        QCOMPARE(resolvePathLabel(reachesOnlyNominalTarget, 100.0, withBuffer),
                 PathLabel::NoTrade);
    }

    //! @tstid TS-PATH-005 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // A degenerate R (no stop distance configured) refuses to walk anything
    // and reports NoTrade rather than dividing by, or comparing against, zero.
    void TS_PATH_005_degenerateRRefusesToWalk()
    {
        const PathOutcomeConfig cfg;   // stopFraction defaults to 0.0
        const QList<DailyBar> bars{barAt(200.0, 50.0, 100.0)};   // would resolve SOMETHING if walked
        QCOMPARE(resolvePathLabel(bars, 100.0, cfg), PathLabel::NoTrade);
    }

    //! @tstid TS-PATH-006 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // The PredictionLedger bridge: only LATER rows (strictly after the
    // decision) for the SAME symbol become the path — an earlier row and a
    // different instrument's row must not corrupt the label, and the rows
    // are walked in TIME order regardless of the order they arrive in.
    void TS_PATH_006_predictionBridgeFiltersBySymbolAndTime()
    {
        const QDateTime t0(QDate(2026, 8, 12), QTime(10, 0), QTimeZone::UTC);
        const Prediction decision = predictionAt(t0, QStringLiteral("SPX500"), 100.0);

        PathOutcomeConfig cfg;
        cfg.stopFraction = 0.05;

        const QList<Prediction> later{
            // A different instrument at a wildly different price: must be excluded.
            predictionAt(t0.addSecs(60), QStringLiteral("NSDQ100"), 20000.0),
            // Before the decision: must be excluded (defensive — resolveOutcome's own
            // convention is that `later` already only holds later rows, but this
            // function does not trust that blindly).
            predictionAt(t0.addSecs(-60), QStringLiteral("SPX500"), 50.0),
            // Out of time order on purpose: the bridge must sort by `at` itself.
            predictionAt(t0.addSecs(180), QStringLiteral("SPX500"), 105.5),
            predictionAt(t0.addSecs(60), QStringLiteral("SPX500"), 102.0),
        };

        QCOMPARE(resolvePathLabelForPrediction(decision, later, cfg), PathLabel::Long);
    }
};

QTEST_GUILESS_MAIN(TestPathOutcome)
#include "tst_pathoutcome.moc"
