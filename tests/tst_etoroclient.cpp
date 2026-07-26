// Integration tests for the eToro client's closed-trade history walk
// (DES-SVC-CLIENT) against an in-process mock of the public API: the pager
// accumulates multiple pages, and the cost estimator prices each trade from
// the bulk-rates spread (half-spread × invest × leverage per side).

#include "MockHttpServer.h"
#include "services/Config.h"
#include "services/EtoroClient.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

// Two history pages (newest first) followed by an empty page; instrument 27
// with a live 100/101 quote → spread 1/100.5 ≈ 0.995% of mid.
QByteArray historyPage(qint32 page)
{
    if (page == 1) {
        return R"([
            {"instrumentId":27,"isBuy":true,"leverage":20,"investment":1000.0,
             "units":4.0,"openRate":5000.0,"closeRate":5050.0,"netProfit":150.0,
             "fees":-2.5,"openTimestamp":"2026-07-01T10:00:00Z",
             "closeTimestamp":"2026-07-02T10:00:00Z","positionId":1},
            {"instrumentId":27,"isBuy":false,"leverage":10,"investment":500.0,
             "units":1.0,"openRate":5100.0,"closeRate":5150.0,"netProfit":-60.0,
             "fees":-1.0,"openTimestamp":"2026-07-03T10:00:00Z",
             "closeTimestamp":"2026-07-04T10:00:00Z","positionId":2}
        ])";
    }
    if (page == 2) {
        return R"([
            {"instrumentId":38,"isBuy":true,"leverage":5,"investment":200.0,
             "units":0.5,"openRate":20000.0,"closeRate":20100.0,"netProfit":10.0,
             "fees":0.0,"openTimestamp":"2026-06-20T10:00:00Z",
             "closeTimestamp":"2026-06-21T10:00:00Z","positionId":3}
        ])";
    }
    return "[]";
}

} // namespace

class TestEtoroClient : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-CLI-001 @design DES-SVC-CLIENT
    // @relation(REQ-F-014, REQ-N-003, scope=function)
    void TS_CLI_001_historyPagerAccumulates()
    {
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/trading/info/trade/history"))) {
                qint32 page = 1;
                const QUrlQuery q(QUrl(path).query());
                if (q.hasQueryItem(QStringLiteral("page"))) {
                    page = q.queryItemValue(QStringLiteral("page")).toInt();
                }
                return MockHttpServer::Response{200, historyPage(page), {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"rates":[{"instrumentId":27,"bid":100.0,"ask":101.0},
                                 {"instrumentId":38,"bid":200.0,"ask":201.0}]})",
                    {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        Config cfg;
        cfg.apiKey = QStringLiteral("k");   // credentials → real mode code path
        cfg.userKey = QStringLiteral("u");
        cfg.mode = QStringLiteral("demo");
        cfg.baseUrl = server.baseUrl() + QStringLiteral("/api");
        EtoroClient client(cfg);

        QSignalSpy summary(&client, &EtoroClient::monthlyPnlReady);
        QSignalSpy trades(&client, &EtoroClient::closedTradesReady);
        client.fetchClosedTrades(8);
        QVERIFY(summary.wait(15000));
        QCOMPARE(trades.count(), 1);

        const auto pnl = summary.takeFirst().at(0).value<MonthlyPnl>();
        QCOMPARE(pnl.accountTrades, 3);                 // both pages accumulated
        QCOMPARE(pnl.accountNet, 150.0 - 60.0 + 10.0);
        // ~8 weeks back requested.
        QVERIFY(pnl.fromDate <= QDate::currentDate().addDays(-7 * 8 + 1));
    }

    //! @tstid TS-CLI-002 @design DES-SVC-CLIENT
    // @relation(REQ-F-014, scope=function)
    void TS_CLI_002_closedTradeCostEstimates()
    {
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/trading/info/trade/history"))) {
                const QUrlQuery q(QUrl(path).query());
                const qint32 page = q.queryItemValue(QStringLiteral("page")).toInt();
                return MockHttpServer::Response{200, historyPage(page), {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                return MockHttpServer::Response{
                    200, R"({"rates":[{"instrumentId":27,"bid":100.0,"ask":101.0}]})", {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        Config cfg;
        cfg.apiKey = QStringLiteral("k");
        cfg.userKey = QStringLiteral("u");
        cfg.mode = QStringLiteral("demo");
        cfg.baseUrl = server.baseUrl() + QStringLiteral("/api");
        EtoroClient client(cfg);

        QSignalSpy trades(&client, &EtoroClient::closedTradesReady);
        client.fetchClosedTrades(8);
        QVERIFY(trades.wait(15000));
        const auto list = trades.takeFirst().at(0).value<QList<ClosedTrade>>();
        QCOMPARE(list.size(), 3);

        // Instrument 27 has a quote: spread% = 1/100.5; each side costs
        // invest × leverage × spread%/2.
        const double spreadPct = (1.0 / 100.5) * 100.0;
        const ClosedTrade &first = list[0];
        QCOMPARE(first.instrumentId, static_cast<qint64>(27));
        QVERIFY(first.costEstValid);
        QVERIFY(std::abs(first.openCostEst
                         - (1000.0 * 20.0 * (spreadPct / 100.0) / 2.0)) < 1e-9);
        QCOMPARE(first.openCostEst, first.closeCostEst);
        QVERIFY(first.isBuy);
        QCOMPARE(first.netProfit, 150.0);
        QCOMPARE(first.fees, -2.5);

        // Instrument 38 got no quote in this run → costs honestly unknown.
        const ClosedTrade &last = list[2];
        QCOMPARE(last.instrumentId, static_cast<qint64>(38));
        QVERIFY(!last.costEstValid);
    }
};

QTEST_GUILESS_MAIN(TestEtoroClient)
#include "tst_etoroclient.moc"
