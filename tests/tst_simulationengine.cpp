// Integration tests for the self-contained simulation (DES-SVC-SIM): the
// synthetic feed, virtual account and SL execution work end-to-end through
// the same signal interface the real client exposes.

#include "services/SimulationEngine.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

class TestSimulationEngine : public QObject
{
    Q_OBJECT
private slots:
    //! @tstid TS-SIM-001 @verifies REQ-F-017 @design DES-SVC-SIM
    void TS_SIM_001_snapshotAndTick()
    {
        SimulationEngine sim;
        QSignalSpy history(&sim, &SimulationEngine::historyReady);
        QSignalSpy price(&sim, &SimulationEngine::priceUpdated);
        QSignalSpy cash(&sim, &SimulationEngine::cashUpdated);
        QSignalSpy leverage(&sim, &SimulationEngine::leverageOptions);

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

    //! @tstid TS-SIM-002 @verifies REQ-F-017 @design DES-SVC-SIM
    void TS_SIM_002_openPositionBooksCashAndPortfolio()
    {
        SimulationEngine sim;
        static_cast<void>(sim.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true));
        sim.emitSnapshot();

        QSignalSpy portfolio(&sim, &SimulationEngine::portfolioUpdated);
        QSignalSpy cash(&sim, &SimulationEngine::cashUpdated);
        QSignalSpy result(&sim, &SimulationEngine::orderResult);
        sim.openPosition(true, 1000.0, 5.0, 100.0, 200.0, false);

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

    //! @tstid TS-SIM-003 @verifies REQ-F-017 @design DES-SVC-SIM
    void TS_SIM_003_stopLossAutoCloseAndSummary()
    {
        SimulationEngine sim;
        static_cast<void>(sim.prepare(QStringLiteral("SPX500"), QStringLiteral("usd"), true));
        sim.emitSnapshot();
        // Tight stop, no take-profit: the random walk must strike the stop.
        sim.openPosition(true, 1000.0, 10.0, 1.0, 0.0, false);

        QSignalSpy closed(&sim, &SimulationEngine::positionClosed);
        QSignalSpy portfolio(&sim, &SimulationEngine::portfolioUpdated);
        for (qint32 i = 0; (i < 5000) && closed.isEmpty(); ++i) {
            sim.tick();
        }
        QVERIFY2(!closed.isEmpty(), "stop-loss never triggered in 5000 ticks");
        QVERIFY(closed.last().at(0).toBool());
        QVERIFY(portfolio.count() >= 1);
        QVERIFY(portfolio.last().at(0).value<QList<Position>>().isEmpty());

        QSignalSpy summary(&sim, &SimulationEngine::monthlyPnlReady);
        sim.summarizeMonthly();
        QCOMPARE(summary.count(), 1);
        const auto pnl = summary.takeFirst().at(0).value<MonthlyPnl>();
        QCOMPARE(pnl.trades, 1);                 // the auto-closed trade is logged
    }
};

QTEST_GUILESS_MAIN(TestSimulationEngine)
#include "tst_simulationengine.moc"
