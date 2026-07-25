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
    Q_OBJECT
private slots:
    //! @tstid TS-PLAN-001 @verifies REQ-F-010 REQ-F-012 @design DES-DOM-PLAN
    void TS_PLAN_001_slFractionScalingAndClamps()
    {
        // 1.5σ·√horizon: 0.2%/bar over 24 bars → 1.5·0.002·√24 ≈ 0.0147.
        const double f24 = proposedSlFraction(0.2, 24);
        QVERIFY(std::abs(f24 - (1.5 * 0.002 * std::sqrt(24.0))) < 1e-9);
        QVERIFY(proposedSlFraction(0.2, 96) > f24);        // grows with horizon
        QCOMPARE(proposedSlFraction(0.0, 24), 0.001);      // lower clamp
        QCOMPARE(proposedSlFraction(50.0, 96), 0.5);       // upper clamp
    }

    //! @tstid TS-PLAN-002 @verifies REQ-F-010 @design DES-DOM-PLAN
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

    //! @tstid TS-PLAN-003 @verifies REQ-F-010 @design DES-DOM-PLAN
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

    //! @tstid TS-PLAN-004 @verifies REQ-F-011 @design DES-DOM-PLAN
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

    //! @tstid TS-PLAN-005 @verifies REQ-F-010 REQ-F-011 @design DES-DOM-PLAN
    void TS_PLAN_005_costsEatEdgeStaysOut()
    {
        PlanInput in = baseInput(trend(120, 1.004, 1.001));
        in.spreadPct = 25.0;  // absurd spread: costs dwarf any plausible edge
        const TradePlan plan = buildTradePlan(in);
        QVERIFY(plan.valid);
        QCOMPARE(plan.verdict, QStringLiteral("STAY OUT"));
        QVERIFY(plan.expectedNet <= 0.0);
    }

    //! @tstid TS-PLAN-006 @verifies REQ-F-010 @design DES-DOM-PLAN
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
};

QTEST_GUILESS_MAIN(TestTradePlan)
#include "tst_tradeplan.moc"
