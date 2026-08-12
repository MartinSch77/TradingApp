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

    //! @tstid TS-PATH-007 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // Touching a stop or target EXACTLY (bar low/high equal to the price, not past it)
    // counts as a hit, in both directions — the boundary a stop/limit order actually
    // fills on, pinned explicitly rather than left to whichever side an off-by-one
    // comparison happens to land on.
    void TS_PATH_007_exactTouchCountsAsAHitBothDirections()
    {
        PathOutcomeConfig cfg;
        cfg.stopFraction = 0.05;   // long: stop exactly 95, target exactly 105

        // Long: the bar's LOW lands exactly on the stop, its high stays under target.
        const QList<DailyBar> longStopTouch{barAt(100.0, 95.0, 97.0)};
        const DirectionalPathResult longStopped = walkPath(longStopTouch, 100.0, true, cfg);
        QVERIFY(!longStopped.reachedTargetFirst);
        QCOMPARE(longStopped.barsToResolve, 1);

        // Long: the bar's HIGH lands exactly on the target, its low stays above stop.
        const QList<DailyBar> longTargetTouch{barAt(105.0, 99.0, 103.0)};
        const DirectionalPathResult longTargeted = walkPath(longTargetTouch, 100.0, true, cfg);
        QVERIFY(longTargeted.reachedTargetFirst);

        // Short: the bar's HIGH lands exactly on the (higher) stop. Checking
        // barsToResolve (not just reachedTargetFirst) matters here: a mutated
        // >= -> > on this comparison does not manufacture a false target hit, it
        // just fails to resolve at all — invisible to a reachedTargetFirst-only check.
        const QList<DailyBar> shortStopTouch{barAt(105.0, 100.0, 103.0)};
        const DirectionalPathResult shortStopped = walkPath(shortStopTouch, 100.0, false, cfg);
        QVERIFY(!shortStopped.reachedTargetFirst);
        QCOMPARE(shortStopped.barsToResolve, 1);

        // Short: the bar's LOW lands exactly on the (lower) target.
        const QList<DailyBar> shortTargetTouch{barAt(101.0, 95.0, 97.0)};
        const DirectionalPathResult shortTargeted = walkPath(shortTargetTouch, 100.0, false, cfg);
        QVERIFY(shortTargeted.reachedTargetFirst);
    }

    //! @tstid TS-PATH-008 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // The degenerate-input refusal fires exactly AT zero, not only below it — bars chosen
    // so that skipping the refusal (rather than actually computing a distance of zero)
    // would visibly change the answer, which a bar that merely happens to agree either
    // way could never prove.
    void TS_PATH_008_refusalBoundaryIsExactlyZero()
    {
        // A bar whose low sits ABOVE 100 and whose high sits above it too: if a
        // zero-stopFraction failed to refuse, stopPrice == targetPrice == 100 would make
        // this bar's high "reach" the target on the very first check — a refusing
        // implementation must return the untouched default instead.
        const QList<DailyBar> bars{barAt(105.0, 101.0, 103.0)};

        PathOutcomeConfig zeroStopFraction;
        zeroStopFraction.stopFraction = 0.0;
        const DirectionalPathResult a = walkPath(bars, 100.0, true, zeroStopFraction);
        QVERIFY(!a.reachedTargetFirst);
        QCOMPARE(a.barsToResolve, 0);

        // Same argument for entryPrice: a zero entry makes stopPrice == targetPrice == 0,
        // and any ordinary positive bar's high "reaches" that target trivially unless the
        // refusal actually fires.
        PathOutcomeConfig validFraction;
        validFraction.stopFraction = 0.05;
        const DirectionalPathResult b = walkPath(bars, 0.0, true, validFraction);
        QVERIFY(!b.reachedTargetFirst);
        QCOMPARE(b.barsToResolve, 0);
    }

    //! @tstid TS-PATH-009 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // targetR actually SCALES the target distance (multiplication, not accidentally
    // interchangeable with division) — the default targetR/stopR of 1.0 cannot tell the
    // two apart (x*1.0 == x/1.0), so this pins it with a target reward twice the size of
    // the risk.
    void TS_PATH_009_targetRScalesTheTargetDistance()
    {
        PathOutcomeConfig cfg;
        cfg.stopFraction = 0.05;   // R = 5 at entry 100 -> stop 95
        cfg.targetR = 2.0;         // target 2R away = entry + 10 = 110, NOT entry + 2.5 = 102.5

        // A bar that clears the honest 2R target (110) but would ALSO have cleared a
        // mistakenly-divided target (100 + 5/2 = 102.5) — so reaching 111 only
        // distinguishes multiplication from division if the halved target is what
        // actually gets checked. Use a bar that reaches 105 (clears the WRONG
        // divided-by-2 target of 102.5, stays under the correct target of 110) to prove
        // the real target is 110, not 102.5.
        const QList<DailyBar> reachesHalfButNotDouble{barAt(105.0, 99.0, 103.0)};
        const DirectionalPathResult notYetAtRealTarget =
            walkPath(reachesHalfButNotDouble, 100.0, true, cfg);
        QVERIFY(!notYetAtRealTarget.reachedTargetFirst);

        const QList<DailyBar> reachesRealTarget{barAt(111.0, 99.0, 110.5)};
        const DirectionalPathResult atRealTarget = walkPath(reachesRealTarget, 100.0, true, cfg);
        QVERIFY(atRealTarget.reachedTargetFirst);
    }

    //! @tstid TS-PATH-010 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // barsToResolve on the TARGET branch is the exact bar index (1-based), not merely
    // "some value that happens to agree at index 0" — pinned at an index where +1 and -1
    // give different, checkable answers.
    void TS_PATH_010_barsToResolveIsExactOnTheTargetBranch()
    {
        PathOutcomeConfig cfg;
        cfg.stopFraction = 0.05;
        // Two quiet bars, then the third (index 2) reaches the target.
        const QList<DailyBar> bars{barAt(101.0, 99.0, 100.0), barAt(101.0, 99.0, 100.0),
                                  barAt(106.0, 99.0, 105.5)};
        const DirectionalPathResult result = walkPath(bars, 100.0, true, cfg);
        QVERIFY(result.reachedTargetFirst);
        QCOMPARE(result.barsToResolve, 3);   // index 2 + 1, never 2 - 1 == 1
    }

    //! @tstid TS-PATH-011 @design DES-DOM-PATHOUTCOME
    // @relation(REQ-F-037, scope=function)
    //
    // stopR actually SCALES the stop distance (multiplication, not accidentally
    // interchangeable with division) — the default stopR of 1.0 cannot tell the two
    // apart, so this pins it with a stop twice as far away as the raw stopFraction.
    void TS_PATH_011_stopRScalesTheStopDistance()
    {
        PathOutcomeConfig cfg;
        cfg.stopFraction = 0.05;   // raw distance = 5 at entry 100
        cfg.stopR = 2.0;           // honest stop is 2R away = entry - 10 = 90, NOT entry - 2.5 = 97.5

        // A bar whose low dips to 96 clears the WRONG halved stop (97.5) but stays
        // above the honest doubled stop (90) — must NOT be flagged as stopped.
        const QList<DailyBar> clearsHalvedStopOnly{barAt(101.0, 96.0, 99.0)};
        const DirectionalPathResult notYetStopped =
            walkPath(clearsHalvedStopOnly, 100.0, true, cfg);
        QVERIFY(!notYetStopped.reachedTargetFirst);
        QCOMPARE(notYetStopped.barsToResolve, 0);

        // A bar whose low reaches the real stop at 90.
        const QList<DailyBar> reachesRealStop{barAt(101.0, 89.0, 91.0)};
        const DirectionalPathResult stopped = walkPath(reachesRealStop, 100.0, true, cfg);
        QVERIFY(!stopped.reachedTargetFirst);
        QCOMPARE(stopped.barsToResolve, 1);
    }
};

QTEST_GUILESS_MAIN(TestPathOutcome)
#include "tst_pathoutcome.moc"
