// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The pure half of the real-money mirror (DES-DOM-LIVEMIRROR, REQ-F-076): how much real
// money one paper trade becomes, why it becomes none, and the day-loss ledger the kill
// switch is tripped on. Every figure here is pinned to the cent, because these are the
// numbers that leave the account.

#include "domain/LiveMirror.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {

Money eur(double major)
{
    return Money::fromDouble(major, Currency::Eur);
}

Money usd(double major)
{
    return Money::fromDouble(major, Currency::Usd);
}

// The owner's grant: SPX500 + NSDQ100, 250 EUR per order, 250 EUR daily loss.
LiveMirrorConfig grant()
{
    LiveMirrorConfig cfg;
    cfg.maxPerOrder = eur(250.0);
    cfg.maxDailyLoss = eur(250.0);
    return cfg;
}

// A paper SPX500 long the bot would open on a 50 000 account: 3 000 EUR at x10, entered
// at 5 000 with the stop 25 points below and the target 37.5 above (reward:risk 1.5).
PaperTrade spxTrade()
{
    PaperTrade t;
    t.id = 41;
    t.symbol = QStringLiteral("SPX500");
    t.instrumentId = 27;
    t.isBuy = true;
    t.stake = 3000.0;
    t.leverage = 10;
    t.openRate = 5000.0;
    t.slRate = 4975.0;
    t.tpRate = 5037.5;
    return t;
}

LiveMirrorInputs inputs(const PaperTrade &trade, double eurPerUsd = 0.9)
{
    LiveMirrorInputs in;
    in.trade = trade;
    in.eurPerUsd = eurPerUsd;
    return in;
}

QDateTime at(qint32 day, qint32 hour)
{
    return {QDate(2026, 9, day), QTime(hour, 0), QTimeZone::UTC};
}

}   // namespace

class TestLiveMirror : public QObject
{
    Q_OBJECT;

private slots:
    //! @tstid TS-LIVE-001 @design DES-DOM-LIVEMIRROR
    // @relation(REQ-F-076, scope=function)
    //
    // The stake is the cap, not the paper stake; it is converted ONCE; the leverage is
    // kept; and the stop/target amounts scale by the same ratio so the RATES agree.
    void TS_LIVE_001_theCapBindsAndTheGeometryScalesWithIt()
    {
        const LiveOrderPlan plan = liveOrderFor(inputs(spxTrade()), grant());
        QVERIFY2(plan.ok, qPrintable(plan.why));
        QVERIFY(plan.code.isEmpty());
        // 3 000 EUR paper stake against a 250 EUR cap: the cap wins. 250 / 0.9 = 277.777…
        // USD, rounded half away from zero to the cent — 277.78, exactly once.
        QCOMPARE(plan.amounts.stake, usd(277.78));
        QCOMPARE(plan.amounts.stake.minorUnits(), 27778);
        QCOMPARE(plan.request.amount, 277.78);
        QCOMPARE(plan.request.instrumentId, 27);
        QVERIFY(plan.request.isBuy);
        QCOMPARE(plan.request.leverage, 10.0);
        QVERIFY(!plan.request.isLimit());   // the paper trade is open NOW: a market order
        // Paper loss at stop: units 3000×10/5000 = 6 × 25 = 150 (EUR terms); gain at target
        // 6 × 37.5 = 225. Scaled by 277.78/3000 = 0.0925933…: 13.889 → 13.89 and 20.83.
        QCOMPARE(plan.amounts.stopLoss, usd(13.89));
        QCOMPARE(plan.amounts.takeProfit, usd(20.83));
        QCOMPARE(plan.request.stopLossAmount, 13.89);
        QCOMPARE(plan.request.takeProfitAmount, 20.83);
        // …which puts the live stop and target at the paper RATES: live units are
        // 277.78×10/5000 = 0.55556, and 13.89/0.55556 = 25.0 points, 20.83/0.55556 = 37.5.
        const double liveUnits = (277.78 * 10.0) / 5000.0;
        QVERIFY(std::fabs((13.89 / liveUnits) - 25.0) < 0.01);
        QVERIFY(std::fabs((20.83 / liveUnits) - 37.5) < 0.01);
        QVERIFY(plan.why.contains(QStringLiteral("BUY SPX500 277.78 USD at x10")));

        // A paper stake BELOW the cap goes out at the paper stake: the cap is a ceiling,
        // not a size. 100 EUR / 0.9 = 111.11 USD.
        PaperTrade small = spxTrade();
        small.stake = 100.0;
        const LiveOrderPlan smallPlan = liveOrderFor(inputs(small), grant());
        QVERIFY(smallPlan.ok);
        QCOMPARE(smallPlan.amounts.stake, usd(111.11));

        // A SHORT keeps its side, and a EUR account needs no rate at all.
        PaperTrade shortTrade = spxTrade();
        shortTrade.isBuy = false;
        shortTrade.symbol = QStringLiteral("NSDQ100");
        LiveMirrorInputs eurAccount = inputs(shortTrade, 0.0);
        eurAccount.accountCurrency = Currency::Eur;
        const LiveOrderPlan shortPlan = liveOrderFor(eurAccount, grant());
        QVERIFY2(shortPlan.ok, qPrintable(shortPlan.why));
        QVERIFY(!shortPlan.request.isBuy);
        QCOMPARE(shortPlan.amounts.stake, eur(250.0));
        QCOMPARE(shortPlan.amounts.stopLoss, eur(12.50));   // 150 × 250/3000
        QCOMPARE(shortPlan.amounts.takeProfit, eur(18.75));   // 225 × 250/3000

        // A paper trade with NO stop gets none live, and says so rather than inventing one.
        PaperTrade noStop = spxTrade();
        noStop.slRate = 0.0;
        const LiveOrderPlan noStopPlan = liveOrderFor(inputs(noStop), grant());
        QVERIFY(noStopPlan.ok);
        QVERIFY(noStopPlan.amounts.stopLoss.isZero());
        QCOMPARE(noStopPlan.request.stopLossAmount, 0.0);
        QVERIFY(noStopPlan.why.contains(QStringLiteral("NO stop")));
    }

    //! @tstid TS-LIVE-002 @design DES-DOM-LIVEMIRROR
    // @relation(REQ-F-076, scope=function)
    //
    // Every refusal has its own countable code, and each is reached by exactly its fault.
    void TS_LIVE_002_eachRefusalIsNamedByItsOwnCode()
    {
        // Outside the fixed scope: a crypto and a metal the paper bot trades happily.
        for (const QString &symbol : {QStringLiteral("BTC"), QStringLiteral("GOLD")}) {
            PaperTrade other = spxTrade();
            other.symbol = symbol;
            const LiveOrderPlan plan = liveOrderFor(inputs(other), grant());
            QVERIFY(!plan.ok);
            QCOMPARE(plan.code, QStringLiteral("live-scope"));
            QVERIFY(plan.why.contains(symbol));
            QVERIFY(plan.why.contains(QStringLiteral("SPX500+NSDQ100")));
        }
        // One live position per instrument: the second SPX500 open is refused.
        LiveMirrorInputs stacked = inputs(spxTrade());
        stacked.openLivePositionsForSymbol = 1;
        QCOMPARE(liveOrderFor(stacked, grant()).code, QStringLiteral("live-position-cap"));
        // …unless the grant allows more.
        LiveMirrorConfig two = grant();
        two.maxPositionsPerSymbol = 2;
        QVERIFY(liveOrderFor(stacked, two).ok);

        // No venue id: the paper bot opens id-less (crypto prices off its candle), a real
        // order cannot be placed without one.
        PaperTrade unresolved = spxTrade();
        unresolved.instrumentId = 0;
        QCOMPARE(liveOrderFor(inputs(unresolved), grant()).code, QStringLiteral("live-unresolved"));

        // No FX rate yet on a USD account: the EUR cap cannot become a USD stake.
        const LiveOrderPlan noFx = liveOrderFor(inputs(spxTrade(), 0.0), grant());
        QCOMPARE(noFx.code, QStringLiteral("live-fx-unknown"));
        QVERIFY(noFx.why.contains(QStringLiteral("EUR/USD")));

        // An unset cap, a ZERO cap (0 is "nothing may be sent", never "no cap"), an unset
        // daily-loss cap, and a paper trade without a positive stake are all unsized.
        LiveMirrorConfig unset;
        QCOMPARE(liveOrderFor(inputs(spxTrade()), unset).code, QStringLiteral("live-unsized"));
        LiveMirrorConfig zero = grant();
        zero.maxPerOrder = eur(0.0);
        QCOMPARE(liveOrderFor(inputs(spxTrade()), zero).code, QStringLiteral("live-unsized"));
        LiveMirrorConfig noLossCap = grant();
        noLossCap.maxDailyLoss = Money();
        QCOMPARE(liveOrderFor(inputs(spxTrade()), noLossCap).code, QStringLiteral("live-unsized"));
        PaperTrade unstaked = spxTrade();
        unstaked.stake = 0.0;
        QCOMPARE(liveOrderFor(inputs(unstaked), grant()).code, QStringLiteral("live-unsized"));

        // The structural refusals come BEFORE the transient one: an out-of-scope trade with
        // no FX rate is refused for its scope, which is what it will still be tomorrow.
        PaperTrade btc = spxTrade();
        btc.symbol = QStringLiteral("BTC");
        QCOMPARE(liveOrderFor(inputs(btc, 0.0), grant()).code, QStringLiteral("live-scope"));
    }

    //! @tstid TS-LIVE-003 @design DES-DOM-LIVEMIRROR
    // @relation(REQ-F-076, scope=function)
    //
    // The day-loss ledger: reached exactly at the cap, not a cent before, reset by a new
    // date, and never tripped by an unknown result or a foreign currency.
    void TS_LIVE_003_theDailyLossIsReachedAtTheCapAndResetsWithTheDate()
    {
        const Money cap = usd(250.0);
        LiveDay day;
        QVERIFY(!liveDailyLossReached(day, at(28, 10), cap));   // nothing booked yet

        day = liveDayAfterClose(day, at(28, 10), usd(-100.0));
        QCOMPARE(day.date, QDate(2026, 9, 28));
        QCOMPARE(day.realized, usd(-100.0));
        QVERIFY(!liveDailyLossReached(day, at(28, 10), cap));
        day = liveDayAfterClose(day, at(28, 11), usd(-149.99));
        QCOMPARE(day.realized, usd(-249.99));
        QVERIFY(!liveDailyLossReached(day, at(28, 11), cap));   // one cent short: not reached
        day = liveDayAfterClose(day, at(28, 12), usd(-0.01));
        QCOMPARE(day.realized, usd(-250.0));
        QVERIFY(liveDailyLossReached(day, at(28, 12), cap));   // exactly at the cap: reached
        day = liveDayAfterClose(day, at(28, 13), usd(-30.0));
        QVERIFY(liveDailyLossReached(day, at(28, 13), cap));   // and beyond it

        // A win books against the loss: −280 + 40 = −240 is not reached.
        day = liveDayAfterClose(day, at(28, 14), usd(40.0));
        QCOMPARE(day.realized, usd(-240.0));
        QVERIFY(!liveDailyLossReached(day, at(28, 14), cap));

        // Yesterday's ledger does not govern today, and the first close of the new date
        // starts the ledger over.
        QVERIFY(!liveDailyLossReached(day, at(29, 9), cap));
        day = liveDayAfterClose(day, at(29, 9), usd(-5.0));
        QCOMPARE(day.date, QDate(2026, 9, 29));
        QCOMPARE(day.realized, usd(-5.0));

        // An UNKNOWN result books nothing (the executor says so in its log) — but a new
        // date still opens a fresh, zero ledger of no particular currency.
        const LiveDay unchanged = liveDayAfterClose(day, at(29, 10), Money());
        QCOMPARE(unchanged.realized, usd(-5.0));
        const LiveDay fresh = liveDayAfterClose(day, at(30, 10), Money());
        QCOMPARE(fresh.date, QDate(2026, 9, 30));
        QVERIFY(!fresh.realized.isValid());
        QVERIFY(!liveDailyLossReached(fresh, at(30, 10), cap));

        // An invalid cap never trips; a cap in the WRONG currency never trips either
        // (unordered), rather than tripping or passing on a number that means nothing.
        LiveDay deep;
        deep = liveDayAfterClose(deep, at(28, 10), usd(-1000.0));
        QVERIFY(!liveDailyLossReached(deep, at(28, 10), Money()));
        QVERIFY(!liveDailyLossReached(deep, at(28, 10), eur(250.0)));
        QVERIFY(liveDailyLossReached(deep, at(28, 10), cap));
    }

    //! @tstid TS-LIVE-004 @design DES-DOM-LIVEMIRROR
    // @relation(REQ-F-076, REQ-N-005, scope=function)
    //
    // The grant's action string names EVERYTHING the double press permits, so a changed
    // cap or scope is a different action the gate will not combine with the old one.
    void TS_LIVE_004_theGrantActionNamesScopeCapsAndDuration()
    {
        const QString action = liveGrantAction(grant());
        QCOMPARE(action, QStringLiteral("ARM REAL MONEY: SPX500+NSDQ100, max 250.00 EUR per "
                                        "order, 250.00 EUR daily loss, 480 min"));
        LiveMirrorConfig other = grant();
        other.maxPerOrder = eur(100.0);
        QVERIFY(liveGrantAction(other) != action);
        other = grant();
        other.armMinutes = 60;
        QVERIFY(liveGrantAction(other) != action);
        other = grant();
        other.symbols = {QStringLiteral("SPX500")};
        QVERIFY(liveGrantAction(other) != action);
        QVERIFY(liveGrantAction(other).contains(QStringLiteral("SPX500,")));
    }
};

QTEST_GUILESS_MAIN(TestLiveMirror)
#include "tst_livemirror.moc"
