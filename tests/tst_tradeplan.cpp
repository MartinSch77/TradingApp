// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Unit tests for the costed trade planner (DES-DOM-PLAN).

#include "domain/TradePlan.h"

#include <QtTest/QtTest>

#include <cmath>

using namespace trading;

namespace {

QList<double> trend(qint32 n, double factorEven, double factorOdd)
{
    QList<double> s;
    double p = 100.0;
    for (qint32 i = 0; i < n; ++i) {
        p *= (i % 2 == 0) ? factorEven : factorOdd;
        s.append(p);
    }
    return s;
}

PlanInput baseInput(const QList<double> &closes)
{
    PlanInput in;
    in.closes = closes;
    in.price = closes.isEmpty() ? 0.0 : closes.last();
    in.invest = 3750.0;
    in.maxLeverage = 20;
    in.leverageSteps = {1, 2, 5, 10, 20};
    in.horizonHours = 24;
    return in;
}

} // namespace

class TestTradePlan : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-PLAN-001 @design DES-DOM-PLAN
    // @relation(REQ-F-010, REQ-F-012, scope=function)
    void TS_PLAN_001_slFractionScalingAndClamps()
    {
        // 1.5σ·√horizon: 0.2%/bar over 24 bars → 1.5·0.002·√24 ≈ 0.0147.
        const double f24 = proposedSlFraction(0.2, 24);
        QVERIFY(std::abs(f24 - (1.5 * 0.002 * std::sqrt(24.0))) < 1e-9);
        QVERIFY(proposedSlFraction(0.2, 96) > f24);        // grows with horizon
        QCOMPARE(proposedSlFraction(0.0, 24), 0.001);      // lower clamp
        QCOMPARE(proposedSlFraction(50.0, 96), 0.5);       // upper clamp
    }

    //! @tstid TS-PLAN-002 @design DES-DOM-PLAN
    // @relation(REQ-F-010, scope=function)
    void TS_PLAN_002_leverageRecommendation()
    {
        // Budget 25% of stake, stop at 1.8% → raw 13.9 → step down to 10.
        QCOMPARE(recommendLeverage(0.018, 0.25, 20, {1, 2, 5, 10, 20}), 10);
        // Tiny stop distance → raw huge → capped by the instrument max.
        QCOMPARE(recommendLeverage(0.001, 0.25, 10, {1, 2, 5, 10, 20}), 10);
        // Wide stop → x1 only.
        QCOMPARE(recommendLeverage(0.30, 0.25, 20, {1, 2, 5, 10, 20}), 1);
        // Empty steps default to {1,2,5,10,20}; cap 0 means "unknown" → 20.
        QCOMPARE(recommendLeverage(0.001, 0.25, 0, {}), 20);
    }

    //! @tstid TS-PLAN-003 @design DES-DOM-PLAN
    // @relation(REQ-F-010, scope=function)
    void TS_PLAN_003_stayOutWithoutSignal()
    {
        // A flat series with a pinch of noise: no directional ensemble call.
        QList<double> flat;
        for (qint32 i = 0; i < 120; ++i) {
            flat.append(100.0 + ((i % 2 == 0) ? 0.01 : -0.01));
        }
        const TradePlan plan = buildTradePlan(baseInput(flat));
        QVERIFY(plan.valid);
        QCOMPARE(plan.dir, 0);
        QCOMPARE(plan.verdict, QStringLiteral("STAY OUT"));
        QCOMPARE(plan.verdictReason, QStringLiteral("no clear directional signal"));
    }

    //! @tstid TS-PLAN-004 @design DES-DOM-PLAN
    // @relation(REQ-F-011, scope=function)
    void TS_PLAN_004_costBillAndWeekend()
    {
        PlanInput in = baseInput(trend(120, 1.004, 1.001));
        in.spreadPct = 0.10;  // 0.1% of mid
        in.fees.buyOvernight = 1.0;   // USD/unit/night (debit)
        in.fees.sellOvernight = 0.5;
        in.fees.buyWeekend = 3.0;
        in.fees.sellWeekend = 1.5;
        in.feesKnown = true;
        in.now = QDateTime(QDate(2026, 7, 24), QTime(15, 0));  // a Friday

        const TradePlan plan = buildTradePlan(in);
        QVERIFY(plan.valid);
        // Half-spread each way on the notional.
        const double half = in.invest * plan.leverage * (in.spreadPct / 100.0) / 2.0;
        QVERIFY(std::abs(plan.openCost - half) < 1e-9);
        QVERIFY(std::abs(plan.closeCost - half) < 1e-9);
        QVERIFY(plan.crossesWeekend);           // Friday 15:00 + 24h spans the weekend
        QCOMPARE(plan.nights, 1);
        // The single night IS the weekend night: bill = spread both ways + weekend fee.
        const double units = (in.invest * plan.leverage) / in.price;
        const double weekend = ((plan.dir >= 0) ? in.fees.buyWeekend : in.fees.sellWeekend) * units;
        QVERIFY(std::abs(plan.weekendFee - weekend) < 1e-9);
        QVERIFY(std::abs(plan.expectedCosts - ((2.0 * half) + weekend)) < 1e-9);
        QVERIFY(plan.costsComplete);

        // A Monday open with the same horizon has no weekend night in the bill.
        in.now = QDateTime(QDate(2026, 7, 20), QTime(10, 0));  // a Monday
        const TradePlan monday = buildTradePlan(in);
        QVERIFY(!monday.crossesWeekend);
        QCOMPARE(monday.weekendFee, 0.0);
    }

    //! @tstid TS-PLAN-005 @design DES-DOM-PLAN
    // @relation(REQ-F-010, REQ-F-011, scope=function)
    void TS_PLAN_005_costsEatEdgeStaysOut()
    {
        PlanInput in = baseInput(trend(120, 1.004, 1.001));
        in.spreadPct = 25.0;  // absurd spread: costs dwarf any plausible edge
        const TradePlan plan = buildTradePlan(in);
        QVERIFY(plan.valid);
        QCOMPARE(plan.verdict, QStringLiteral("STAY OUT"));
        QVERIFY(plan.expectedNet <= 0.0);
        // Pin the gate that fired: the refusal must name the cost bill, not
        // the confidence or break-even gates that precede it.
        QVERIFY(plan.verdictReason.contains(QStringLiteral("costs eat")));
    }

    //! @tstid TS-PLAN-006 @design DES-DOM-PLAN
    // @relation(REQ-F-010, scope=function)
    void TS_PLAN_006_riskFactorBumps()
    {
        PlanInput calm = baseInput(trend(120, 1.004, 1.001));
        calm.now = QDateTime(QDate(2026, 7, 20), QTime(10, 0));  // Monday, no weekend
        const qint32 baseRisk = buildTradePlan(calm).riskFactor;

        PlanInput stressed = calm;
        stressed.vixValid = true;
        stressed.vix = 32.0;          // +1
        stressed.eventRisk = true;    // +1
        stressed.fgValid = true;
        stressed.fearGreed = 8.0;     // extreme → +1
        const TradePlan plan = buildTradePlan(stressed);
        QVERIFY(plan.riskFactor >= baseRisk + 3 || plan.riskFactor == 5);
        QVERIFY(plan.riskFactor <= 5);
        QVERIFY(!plan.riskNotes.isEmpty());

        PlanInput friday = calm;
        friday.now = QDateTime(QDate(2026, 7, 24), QTime(15, 0));
        const TradePlan weekendPlan = buildTradePlan(friday);
        bool noted = false;
        for (const QString &n : weekendPlan.riskNotes) {
            noted = noted || n.contains(QStringLiteral("weekend"));
        }
        QVERIFY(noted);
    }

    //! @tstid TS-PLAN-007 @design DES-DOM-PLAN
    // @relation(REQ-F-010, scope=function)
    void TS_PLAN_007_verdictGatesConfidenceAndThinEdge()
    {
        // (a) Confidence gate: force a side on a signal-less flat series — the
        // ensemble keeps ~no agreement, so the verdict must refuse the trade.
        QList<double> flat;
        for (qint32 i = 0; i < 120; ++i) {
            flat.append(100.0 + ((i % 2 == 0) ? 0.01 : -0.01));
        }
        PlanInput forced = baseInput(flat);
        forced.dir = 1;
        const TradePlan weak = buildTradePlan(forced);
        QVERIFY(weak.valid);
        QCOMPARE(weak.verdict, QStringLiteral("STAY OUT"));
        QVERIFY(weak.verdictReason.contains(QStringLiteral("confidence too low")));

        // (b) Thin-edge gate: identical seeded Monte-Carlo draws (in.mcSeed), so
        // the only difference between the two plans is the cost bill. Size the
        // spread so the net edge lands inside (0, 0.25% of stake) — actionable
        // gross edge, but not worth the risk.
        PlanInput in = baseInput(trend(120, 1.004, 1.001));
        in.mcSeed = 7U;
        const TradePlan base = buildTradePlan(in);
        QCOMPARE(base.verdict, QStringLiteral("BUY"));  // precondition: clear edge
        const double minEdge = 0.0025 * in.invest;
        QVERIFY(base.expectedNet > minEdge);
        // Bill = 2 half-spreads = invest·lev·spread%/100; leave half the floor.
        const double targetBill = base.expectedNet - (0.5 * minEdge);
        in.spreadPct = (targetBill * 100.0) / (in.invest * base.leverage);
        const TradePlan thin = buildTradePlan(in);
        QCOMPARE(thin.leverage, base.leverage);  // spread must not move the geometry
        QVERIFY(thin.expectedNet > 0.0);
        QVERIFY(thin.expectedNet < minEdge);
        QCOMPARE(thin.verdict, QStringLiteral("STAY OUT"));
        QVERIFY(thin.verdictReason.contains(QStringLiteral("too thin")));
    }

    //! @tstid TS-PLAN-008 @design DES-DOM-PLAN
    // @relation(REQ-F-010, scope=function)
    void TS_PLAN_008_breakEvenGateRefusesPoorWinRate()
    {
        // Gate (b): an actionable call must clear the reward:risk break-even
        // win-rate by two standard errors of its Monte-Carlo estimate. Force a
        // SHORT against a confidently rising market: the ensemble confidence
        // stays high (it measures signal agreement, not the forced side), so
        // the confidence gate passes — but the short's win rate sits far below
        // break-even and the verdict must refuse on exactly that ground.
        PlanInput in = baseInput(trend(120, 1.004, 1.001));
        in.mcSeed = 7U;
        QCOMPARE(buildTradePlan(in).verdict, QStringLiteral("BUY"));  // market is long

        in.dir = -1;
        const TradePlan shortPlan = buildTradePlan(in);
        QVERIFY(shortPlan.valid);
        QCOMPARE(shortPlan.verdict, QStringLiteral("STAY OUT"));
        QVERIFY(shortPlan.verdictReason.contains(QStringLiteral("break-even")));
        // And the quantitative ground holds: the measured conditional win rate
        // itself is below the break-even rate (not merely inside its noise band).
        const double decided = shortPlan.pWin + shortPlan.pLose;
        QVERIFY(decided > 0.0);
        QVERIFY((shortPlan.pWin / decided) < shortPlan.breakeven);
    }
    //! @tstid TS-PLAN-009 @design DES-DOM-PLAN
    // @relation(REQ-F-012, scope=function)
    void TS_PLAN_009_theShortSideTheWeekendAndTheUnmeasurableCases()
    {
        // Everything the existing plan tests establish for a LONG, on the SHORT side —
        // where the fee leg, the win/lose probabilities and the verdict all switch
        // which field they read, and a copy-paste error is invisible until money moves
        // the wrong way.
        const QList<double> falling = trend(120, 0.995, 0.999);
        PlanInput shortIn = baseInput(falling);
        shortIn.now = QDateTime(QDate(2026, 8, 4), QTime(11, 0), QTimeZone::UTC);  // Tuesday
        shortIn.feesKnown = true;
        shortIn.fees.buyOvernight = -0.5;
        shortIn.fees.sellOvernight = -0.2;   // the SHORT's rate, deliberately different
        shortIn.fees.buyWeekend = -1.5;
        shortIn.fees.sellWeekend = -0.6;
        const TradePlan shortPlan = buildTradePlan(shortIn);
        QVERIFY(shortPlan.dir < 0);
        // The short's own rollover was used, not the long's.
        QVERIFY(shortPlan.feePerNight < 0.0);
        QVERIFY(qAbs(shortPlan.feePerNight) < qAbs(shortIn.fees.buyOvernight * 1e6));
        QVERIFY(!shortPlan.verdict.isEmpty());

        // A horizon that crosses a Friday night carries the TRIPLED weekend charge;
        // one inside the working week does not. Both the Saturday and the Sunday start
        // count, which is what a 24/7 instrument needs.
        PlanInput weekend = shortIn;
        weekend.now = QDateTime(QDate(2026, 8, 7), QTime(11, 0), QTimeZone::UTC);   // Friday
        weekend.horizonHours = 24;
        QVERIFY(buildTradePlan(weekend).crossesWeekend);
        weekend.now = QDateTime(QDate(2026, 8, 8), QTime(11, 0), QTimeZone::UTC);   // Saturday
        QVERIFY(buildTradePlan(weekend).crossesWeekend);
        weekend.now = QDateTime(QDate(2026, 8, 9), QTime(11, 0), QTimeZone::UTC);   // Sunday
        QVERIFY(buildTradePlan(weekend).crossesWeekend);
        PlanInput midweek = shortIn;
        midweek.now = QDateTime(QDate(2026, 8, 4), QTime(11, 0), QTimeZone::UTC);   // Tuesday
        midweek.horizonHours = 12;
        QVERIFY(!buildTradePlan(midweek).crossesWeekend);
        // An unknown clock cannot decide it either way — and says no rather than
        // inventing a charge.
        PlanInput noClock = shortIn;
        noClock.now = QDateTime();
        QVERIFY(!buildTradePlan(noClock).crossesWeekend);

        // Fees NOT known: the bill is flagged partial rather than priced at zero,
        // which is the same rule the bot's carry gate follows.
        PlanInput noFees = shortIn;
        noFees.feesKnown = false;
        const TradePlan partial = buildTradePlan(noFees);
        QCOMPARE(partial.feePerNight, 0.0);
        QCOMPARE(partial.weekendFee, 0.0);
        // …and fees that are "known" but empty are the same situation.
        PlanInput emptyFees = shortIn;
        emptyFees.fees = InstrumentFees{};
        QCOMPARE(buildTradePlan(emptyFees).feePerNight, 0.0);

        // Below the minimum history the plan is INVALID rather than a guess: no
        // verdict, no probabilities, nothing a caller could mistake for advice. The
        // volatility and ensemble reads need 31 bars, and a plan built from fewer
        // would be a number with no measurement behind it.
        PlanInput thin = baseInput(QList<double>(30, 100.0));
        thin.now = shortIn.now;
        const TradePlan thinPlan = buildTradePlan(thin);
        QVERIFY(!thinPlan.valid);
        QVERIFY(thinPlan.verdict.isEmpty());
        QCOMPARE(thinPlan.pWin, 0.0);
        QCOMPARE(thinPlan.pLose, 0.0);

        // Enough bars but no price and no stake: the same refusal, for its own reason.
        PlanInput priceless = baseInput(falling);
        priceless.price = 0.0;
        priceless.closes = QList<double>(40, 0.0);
        priceless.now = shortIn.now;
        QVERIFY(!buildTradePlan(priceless).valid);
        PlanInput broke = baseInput(falling);
        broke.invest = 0.0;
        broke.now = shortIn.now;
        QVERIFY(!buildTradePlan(broke).valid);
    }
    //! @tstid TS-PLAN-010 @design DES-DOM-PLAN
    // @relation(REQ-F-012, scope=function)
    void TS_PLAN_010_everyRiskNoteHasItsOwnTrigger()
    {
        // The risk notes are what a person reads before committing money, so each has
        // to have its OWN trigger — a note that appears for the wrong reason teaches
        // the reader to ignore all of them.
        const QList<double> closes = trend(120, 1.004, 1.001);
        PlanInput base = baseInput(closes);
        base.now = QDateTime(QDate(2026, 8, 4), QTime(11, 0), QTimeZone::UTC);   // Tuesday

        const auto notesOf = [](const PlanInput &in) {
            return buildTradePlan(in).riskNotes.join(QStringLiteral(" | "));
        };
        const QString quiet = notesOf(base);

        // An elevated VIX adds its own note; an ordinary one does not, and an ABSENT
        // reading is not the same as a calm market.
        PlanInput fearful = base;
        fearful.vixValid = true;
        fearful.vix = 30.0;
        QVERIFY(notesOf(fearful).contains(QStringLiteral("VIX")));
        PlanInput ordinary = base;
        ordinary.vixValid = true;
        ordinary.vix = 15.0;
        QVERIFY(!notesOf(ordinary).contains(QStringLiteral("elevated VIX")));
        PlanInput noVix = base;
        noVix.vixValid = false;
        noVix.vix = 30.0;   // a number nobody measured must not be read
        QVERIFY(!notesOf(noVix).contains(QStringLiteral("elevated VIX")));

        // Crowd sentiment at EITHER extreme, and neither at the middle.
        PlanInput greedy = base;
        greedy.fgValid = true;
        greedy.fearGreed = 85.0;
        QVERIFY(notesOf(greedy).contains(QStringLiteral("crowd sentiment")));
        PlanInput fearfulCrowd = base;
        fearfulCrowd.fgValid = true;
        fearfulCrowd.fearGreed = 10.0;
        QVERIFY(notesOf(fearfulCrowd).contains(QStringLiteral("crowd sentiment")));
        PlanInput middling = base;
        middling.fgValid = true;
        middling.fearGreed = 50.0;
        QVERIFY(!notesOf(middling).contains(QStringLiteral("crowd sentiment")));
        PlanInput unmeasured = base;
        unmeasured.fgValid = false;
        unmeasured.fearGreed = 95.0;
        QVERIFY(!notesOf(unmeasured).contains(QStringLiteral("crowd sentiment")));

        // An imminent high-impact event.
        PlanInput risky = base;
        risky.eventRisk = true;
        QVERIFY(notesOf(risky).contains(QStringLiteral("event")));
        QVERIFY(!quiet.contains(QStringLiteral("event")));

        // The weekend carry is a risk note of its own, and it follows the same clock
        // the cost bill does.
        PlanInput weekend = base;
        weekend.now = QDateTime(QDate(2026, 8, 7), QTime(11, 0), QTimeZone::UTC);   // Friday
        QVERIFY(buildTradePlan(weekend).crossesWeekend);

        // …and the risk SCORE rises with the number of notes rather than staying flat.
        PlanInput everything = base;
        everything.vixValid = true;
        everything.vix = 30.0;
        everything.fgValid = true;
        everything.fearGreed = 90.0;
        everything.eventRisk = true;
        QVERIFY(buildTradePlan(everything).riskNotes.size()
                > buildTradePlan(base).riskNotes.size());
    }
};

QTEST_GUILESS_MAIN(TestTradePlan)
#include "tst_tradeplan.moc"
