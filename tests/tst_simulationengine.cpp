// Integration tests for the self-contained simulation (DES-SVC-SIM): the
// synthetic feed, virtual account and SL execution work end-to-end through
// the same signal interface the real client exposes.

#include "services/SimulationEngine.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

class TestSimulationEngine : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-SIM-001 @design DES-SVC-SIM
    // @relation(REQ-F-017, scope=function)
    void TS_SIM_001_snapshotAndTick()
    {
        SimulationEngine sim;
        QSignalSpy history(&sim, &SimulationEngine::historyReady);
        const QSignalSpy price(&sim, &SimulationEngine::priceUpdated);
        const QSignalSpy cash(&sim, &SimulationEngine::cashUpdated);
        const QSignalSpy leverage(&sim, &SimulationEngine::leverageOptions);

        const Instrument inst =
            sim.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true);
        QVERIFY(!inst.symbol.isEmpty());
        sim.emitSnapshot();

        QCOMPARE(history.count(), 1);
        const auto candles = history.takeFirst().at(0).value<QList<Candle>>();
        QVERIFY(candles.size() > 50);            // seeded synthetic history
        QVERIFY(price.count() >= 1);
        QVERIFY(cash.count() >= 1);
        QVERIFY(leverage.count() >= 1);

        const double before = sim.lastPrice();
        QVERIFY(before > 0.0);
        for (qint32 i = 0; i < 20; ++i) {
            sim.tick();
        }
        QVERIFY(price.count() >= 20);            // the feed moves on every tick
    }

    //! @tstid TS-SIM-002 @design DES-SVC-SIM
    // @relation(REQ-F-017, scope=function)
    void TS_SIM_002_openPositionBooksCashAndPortfolio()
    {
        SimulationEngine sim;
        static_cast<void>(sim.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true));
        sim.emitSnapshot();

        QSignalSpy portfolio(&sim, &SimulationEngine::portfolioUpdated);
        QSignalSpy cash(&sim, &SimulationEngine::cashUpdated);
        QSignalSpy result(&sim, &SimulationEngine::orderResult);
        OrderRequest req;
        req.isBuy = true;
        req.amount = 1000.0;
        req.leverage = 5.0;
        req.stopLossAmount = 100.0;
        req.takeProfitAmount = 200.0;
        sim.openPosition(req);

        QVERIFY(result.count() >= 1);
        QVERIFY(result.last().at(0).toBool());   // accepted
        QVERIFY(portfolio.count() >= 1);
        const auto positions = portfolio.last().at(0).value<QList<Position>>();
        QCOMPARE(positions.size(), 1);
        const Position &p = positions.first();
        QVERIFY(p.isBuy);
        QCOMPARE(p.amount, 1000.0);
        QCOMPARE(p.leverage, 5.0);
        QVERIFY(p.stopLossRate > 0.0);           // SL amount became a rate
        QVERIFY(p.stopLossRate < p.openRate);    // below the open for a long
        QVERIFY(p.takeProfitRate > p.openRate);
        QVERIFY(cash.count() >= 1);
        QVERIFY(cash.last().at(0).toDouble() < 100000.0);  // margin reserved
    }

    //! @tstid TS-SIM-003 @design DES-SVC-SIM
    // @relation(REQ-F-017, scope=function)
    void TS_SIM_003_stopLossAutoCloseAndSummary()
    {
        SimulationEngine sim;
        static_cast<void>(sim.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true));
        sim.seedRng(1234U);  // deterministic walk: the flaky "SL never hit" run is gone
        sim.emitSnapshot();
        // Tight stop, no take-profit: the random walk must strike the stop.
        OrderRequest req;
        req.isBuy = true;
        req.amount = 1000.0;
        req.leverage = 10.0;
        req.stopLossAmount = 1.0;
        sim.openPosition(req);

        QSignalSpy closed(&sim, &SimulationEngine::positionClosed);
        QSignalSpy portfolio(&sim, &SimulationEngine::portfolioUpdated);
        QSignalSpy cash(&sim, &SimulationEngine::cashUpdated);
        for (qint32 i = 0; (i < 5000) && closed.isEmpty(); ++i) {
            sim.tick();
        }
        QVERIFY2(!closed.isEmpty(), "stop-loss never triggered in 5000 ticks");
        QVERIFY(closed.last().at(0).toBool());
        QVERIFY(portfolio.count() >= 1);
        QVERIFY(portfolio.last().at(0).value<QList<Position>>().isEmpty());
        // The close frees the reserved margin: the 1000 stake returns to cash
        // and only the realized stop-loss (≈ the 1.0 SL amount, plus whatever
        // the discrete tick gapped past it) is gone from the 100000 start.
        QVERIFY(cash.count() >= 1);
        const double cashAfter = cash.last().at(0).toDouble();
        QVERIFY(cashAfter < 100000.0);   // a loss was realized
        QVERIFY(cashAfter > 99900.0);    // ... but the margin itself came back

        QSignalSpy summary(&sim, &SimulationEngine::monthlyPnlReady);
        sim.summarizeMonthly();
        QCOMPARE(summary.count(), 1);
        const auto pnl = summary.takeFirst().at(0).value<MonthlyPnl>();
        QCOMPARE(pnl.trades, 1);                 // the auto-closed trade is logged
    }

    //! @tstid TS-SIM-006 @design DES-SVC-SIM
    // @relation(REQ-F-017, scope=function)
    void TS_SIM_006_trailingStopsRatchetAndTakeProfitsRealiseAGain()
    {
        SimulationEngine sim;
        static_cast<void>(sim.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true));
        sim.seedRng(4321U);
        sim.emitSnapshot();

        // A take-profit that the walk must reach: the position closes in PROFIT,
        // which is the mirror of the stop-loss path and books cash the other way.
        OrderRequest win;
        win.isBuy = true;
        win.amount = 1000.0;
        win.leverage = 10.0;
        win.takeProfitAmount = 1.0;
        sim.openPosition(win);
        QSignalSpy closed(&sim, &SimulationEngine::positionClosed);
        QSignalSpy cash(&sim, &SimulationEngine::cashUpdated);
        for (qint32 i = 0; (i < 5000) && closed.isEmpty(); ++i) {
            sim.tick();
        }
        QVERIFY2(!closed.isEmpty(), "take-profit never triggered in 5000 ticks");
        QVERIFY(closed.last().at(0).toBool());
        QVERIFY(cash.last().at(0).toDouble() > 100000.0);   // a gain was realised

        // A trailing stop follows the price in the trade's favour and NEVER moves
        // against it — which is the whole property that distinguishes it from a
        // fixed stop, and the one a refactor is most likely to invert.
        SimulationEngine trail;
        static_cast<void>(trail.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true));
        trail.seedRng(99U);
        trail.emitSnapshot();
        QSignalSpy portfolio(&trail, &SimulationEngine::portfolioUpdated);
        OrderRequest req;
        req.isBuy = true;
        req.amount = 500.0;
        req.leverage = 2.0;
        req.stopLossAmount = 200.0;      // far away, so the walk cannot hit it at once
        req.trailingStop = true;
        trail.openPosition(req);
        QVERIFY(!portfolio.isEmpty());
        double highWater = -1.0;
        for (qint32 i = 0; i < 400; ++i) {
            trail.tick();
            const auto book = portfolio.last().at(0).value<QList<Position>>();
            if (book.isEmpty()) {
                break;                   // stopped out: still never moved backwards
            }
            const double stop = book.constFirst().stopLossRate;
            QVERIFY(stop > 0.0);
            QVERIFY2(stop >= highWater - 1e-9, "a trailing stop moved against the position");
            highWater = std::max(highWater, stop);
        }
        QVERIFY(highWater > 0.0);

        // Adjusting a live position rewrites both barriers, and asking about one
        // that is not there is answered rather than ignored.
        SimulationEngine edit;
        static_cast<void>(edit.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true));
        edit.emitSnapshot();
        OrderRequest plain;
        plain.isBuy = true;
        plain.amount = 500.0;
        plain.leverage = 1.0;
        edit.openPosition(plain);
        QSignalSpy book(&edit, &SimulationEngine::portfolioUpdated);
        const double open = edit.lastPrice();
        edit.modifyPosition(QStringLiteral("1"), open * 0.9, open * 1.2, true);
        QVERIFY(!book.isEmpty());
        const Position after = book.last().at(0).value<QList<Position>>().constFirst();
        QVERIFY(qAbs(after.stopLossRate - (open * 0.9)) < 1e-6);
        QVERIFY(qAbs(after.takeProfitRate - (open * 1.2)) < 1e-6);
        QVERIFY(after.trailingStop);
        QVERIFY(qAbs(after.trailDistance - qAbs(open - (open * 0.9))) < 1e-6);

        QSignalSpy log(&edit, &SimulationEngine::log);
        edit.modifyPosition(QStringLiteral("does-not-exist"), 1.0, 2.0, false);
        QVERIFY(!log.isEmpty());
        QVERIFY(log.last().at(0).toString().contains(QStringLiteral("not found")));
        QSignalSpy gone(&edit, &SimulationEngine::positionClosed);
        edit.closePosition(QStringLiteral("does-not-exist"));
        QVERIFY(!gone.isEmpty());
        QVERIFY(!gone.last().at(0).toBool());

        // …and an order larger than the simulated account is refused with its
        // numbers, not silently sized down.
        QSignalSpy result(&edit, &SimulationEngine::orderResult);
        OrderRequest huge;
        huge.isBuy = true;
        huge.amount = 1e9;
        huge.leverage = 1.0;
        edit.openPosition(huge);
        QVERIFY(!result.isEmpty());
        QVERIFY(!result.last().at(0).toBool());
        QVERIFY(result.last().at(1).toString().contains(QStringLiteral("Insufficient")));
    }

    //! @tstid TS-SIM-004 @design DES-SVC-SIM
    // @relation(REQ-F-027, scope=function)
    void TS_SIM_004_limitOrderRestsUntilTriggeredAndCanBeCancelled()
    {
        SimulationEngine sim;
        const Instrument inst =
            sim.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true);
        sim.seedRng(4321U);  // deterministic walk, so the trigger is reached in bounded time
        sim.emitSnapshot();
        const double start = sim.lastPrice();
        QVERIFY(start > 0.0);

        const QSignalSpy pending(&sim, &SimulationEngine::pendingOrdersUpdated);
        QSignalSpy portfolio(&sim, &SimulationEngine::portfolioUpdated);

        // Two resting orders: one just below the price (the walk reaches it), one far
        // out of reach that only exists to be cancelled. ("near"/"far" would be legacy
        // windef.h macros, hence the spelled-out names.)
        OrderRequest reachable;
        reachable.isBuy = true;
        reachable.amount = 1000.0;
        reachable.leverage = 5.0;
        reachable.stopLossAmount = 100.0;
        reachable.triggerRate = start * 0.999;
        sim.placePendingOrder(reachable);

        OrderRequest distant = reachable;
        distant.triggerRate = start * 0.5;  // unreachable within the test's ticks
        sim.placePendingOrder(distant);

        QCOMPARE(sim.pendingOrders().size(), 2);
        QVERIFY(pending.count() >= 2);
        // A resting order is NOT a position: nothing was booked into the portfolio.
        QVERIFY(sim.pendingOrders().at(0).orderId != sim.pendingOrders().at(1).orderId);
        QCOMPARE(portfolio.count(), 0);

        // Cancelling takes exactly that order out; the other keeps waiting. The list is
        // in placement order, so index 1 is the distant order.
        QCOMPARE(sim.pendingOrders().at(1).triggerRate, distant.triggerRate);
        sim.cancelPendingOrder(sim.pendingOrders().at(1).orderId);
        QCOMPARE(sim.pendingOrders().size(), 1);
        QCOMPARE(sim.pendingOrders().at(0).triggerRate, reachable.triggerRate);

        // Tick until the price touches the reachable trigger: the order becomes a real
        // position (with its SL applied) and leaves the pending list.
        for (qint32 i = 0; (i < 5000) && !sim.pendingOrders().isEmpty(); ++i) {
            sim.tick();
        }
        QVERIFY2(sim.pendingOrders().isEmpty(), "limit order never triggered in 5000 ticks");
        const auto positions = portfolio.last().at(0).value<QList<Position>>();
        QCOMPARE(positions.size(), 1);
        QCOMPARE(positions.at(0).symbol, inst.symbol);
        QVERIFY(positions.at(0).isBuy);
        QCOMPARE(positions.at(0).amount, 1000.0);
        QVERIFY(positions.at(0).openRate <= reachable.triggerRate);  // at/below the trigger
        QVERIFY(positions.at(0).stopLossRate > 0.0);            // the order's SL came with it
    }

    //! @tstid TS-SIM-005 @design DES-SVC-SIM
    // @relation(REQ-F-027, scope=function)
    void TS_SIM_005_adjustingARestingOrderKeepsSizeAndRenumbersIt()
    {
        SimulationEngine sim;
        static_cast<void>(sim.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true));
        sim.emitSnapshot();
        const double start = sim.lastPrice();

        OrderRequest req;
        req.isBuy = true;
        req.amount = 1000.0;
        req.leverage = 5.0;
        req.stopLossAmount = 100.0;
        req.triggerRate = start * 0.5;  // far out of reach: it stays resting
        sim.placePendingOrder(req);
        QCOMPARE(sim.pendingOrders().size(), 1);
        const QString firstId = sim.pendingOrders().constFirst().orderId;

        // Adjusting changes exactly the trigger and the SL/TP; size, leverage and side
        // carry over, and the order is renumbered because the real path can only cancel
        // and re-place it.
        const QSignalSpy pending(&sim, &SimulationEngine::pendingOrdersUpdated);
        sim.modifyPendingOrder(firstId, start * 0.6, 250.0, 500.0);
        QCOMPARE(pending.count(), 1);
        QCOMPARE(sim.pendingOrders().size(), 1);
        const PendingOrder adjusted = sim.pendingOrders().constFirst();
        QVERIFY(adjusted.orderId != firstId);
        QCOMPARE(adjusted.triggerRate, start * 0.6);
        QCOMPARE(adjusted.stopLossAmount, 250.0);
        QCOMPARE(adjusted.takeProfitAmount, 500.0);
        QCOMPARE(adjusted.amount, 1000.0);
        QCOMPARE(adjusted.leverage, 5.0);
        QVERIFY(adjusted.isBuy);

        // An unknown id changes nothing and is reported as a failure.
        QSignalSpy result(&sim, &SimulationEngine::orderResult);
        sim.modifyPendingOrder(QStringLiteral("nope"), start, 1.0, 2.0);
        QCOMPARE(result.count(), 1);
        QVERIFY(!result.last().at(0).toBool());
        QCOMPARE(sim.pendingOrders().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestSimulationEngine)
#include "tst_simulationengine.moc"
