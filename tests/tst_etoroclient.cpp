// Integration tests for the eToro client's closed-trade history walk
// (DES-SVC-CLIENT) against an in-process mock of the public API: the pager
// accumulates multiple pages, and the cost estimator prices each trade from
// the bulk-rates spread (half-spread × invest × leverage per side).

#include "MockHttpServer.h"
#include "services/Config.h"
#include "services/EtoroClient.h"

#include <QSharedPointer>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

// Generous shared bound for spy waits: the mock answers in milliseconds, the
// margin only absorbs CI load. Deliberate short waits stay literal.
constexpr qint32 kWaitMs = 15000;

// Real-mode credentials pointed at the in-process mock — the one Config shape
// every walk test needs (default symbol SPX500 comes from Config itself).
Config mockConfig(const MockHttpServer &server)
{
    Config cfg;
    cfg.apiKey = QStringLiteral("k");   // credentials → real-mode code path
    cfg.userKey = QStringLiteral("u");
    cfg.mode = QStringLiteral("demo");
    cfg.baseUrl = server.baseUrl() + QStringLiteral("/api");
    return cfg;
}

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

        const Config cfg = mockConfig(server);
        EtoroClient client(cfg);

        QSignalSpy summary(&client, &EtoroClient::monthlyPnlReady);
        const QSignalSpy trades(&client, &EtoroClient::closedTradesReady);
        client.fetchClosedTrades(8);
        QVERIFY(summary.wait(kWaitMs));
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

        const Config cfg = mockConfig(server);
        EtoroClient client(cfg);

        QSignalSpy trades(&client, &EtoroClient::closedTradesReady);
        client.fetchClosedTrades(8);
        QVERIFY(trades.wait(kWaitMs));
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
        // The estimate must disclose the spread % it priced with, and flag it
        // stale: no tradeability poll ran here, so the quote's freshness is
        // unknown and the frozen-quote warning has to stay on (REQ-F-014).
        QVERIFY(std::abs(first.spreadPctUsed - spreadPct) < 1e-9);
        QVERIFY(first.spreadStale);
        QVERIFY(first.isBuy);
        QCOMPARE(first.netProfit, 150.0);
        QCOMPARE(first.fees, -2.5);

        // Instrument 38 got no quote in this run → costs honestly unknown.
        const ClosedTrade &last = list[2];
        QCOMPARE(last.instrumentId, static_cast<qint64>(38));
        QVERIFY(!last.costEstValid);
    }

    //! @tstid TS-CLI-003 @design DES-SVC-CLIENT
    // @relation(REQ-F-017, scope=function)
    void TS_CLI_003_simulationPublishesFxRate()
    {
        // Without credentials the client runs the simulation — which must
        // publish a display FX rate, otherwise the UI blocks every order
        // with "waiting for the EUR/USD rate" (regression test).
        const Config cfg;  // no apiKey/userKey -> simulation mode
        EtoroClient client(cfg);
        QSignalSpy fx(&client, &EtoroClient::fxRateUpdated);
        QSignalSpy ready(&client, &EtoroClient::ready);
        client.start();
        if (ready.isEmpty()) {
            QVERIFY(ready.wait(2000));
        }
        QVERIFY(!fx.isEmpty());
        QVERIFY(fx.first().first().toDouble() > 0.0);
    }

    //! @tstid TS-CLI-004 @design DES-SVC-CLIENT
    // @relation(REQ-F-025, scope=function)
    void TS_CLI_004_closedPositionDisappearsFromOpenTrades()
    {
        // Regression: a position closed at eToro (in its own UI, or automatically
        // by SL/TP) kept showing as an open trade here. The open SET used to come
        // from /pnl, which serves a cached snapshot up to ~1.5 h old. /portfolio is
        // the live view and now decides membership; /pnl only supplies eToro's own
        // P/L for the positions that are still open.
        //
        // /portfolio holds GER40 only. /pnl is stale: it still lists the closed
        // SPX500 trade, and carries a P/L figure for GER40.
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/portfolio"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"clientPortfolio":{"positions":[
                         {"positionId":11,"instrumentId":5,"symbolFull":"GER40","isBuy":true,
                          "amount":100.0,"leverage":10,"openRate":20000.0,
                          "openDateTime":"2026-07-27T09:00:00Z"}
                       ]}})",
                    {}};
            }
            if (path.contains(QStringLiteral("/pnl"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"clientPortfolio":{"positions":[
                         {"positionId":11,"instrumentId":5,"symbolFull":"GER40","isBuy":true,
                          "amount":100.0,"leverage":10,"openRate":20000.0,
                          "unrealizedPnL":{"pnL":42.5,"closeRate":20100.0}},
                         {"positionId":22,"instrumentId":27,"symbolFull":"SPX500","isBuy":true,
                          "amount":250.0,"leverage":5,"openRate":5000.0,
                          "unrealizedPnL":{"pnL":-7.0,"closeRate":4990.0}}
                       ]}})",
                    {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                return MockHttpServer::Response{
                    200, R"({"rates":[{"instrumentId":5,"bid":20100.0,"ask":20102.0}]})", {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        const Config cfg = mockConfig(server);
        EtoroClient client(cfg);
        // Both symbols are listed, so neither is dropped for being unknown — the
        // closed one must disappear because /portfolio omits it, not by accident.
        client.setTradableSymbols({QStringLiteral("GER40"), QStringLiteral("SPX500")});

        QSignalSpy portfolio(&client, &EtoroClient::portfolioUpdated);
        client.refreshPortfolio();
        QVERIFY(portfolio.wait(kWaitMs));

        const auto positions = portfolio.takeLast().at(0).value<QList<Position>>();
        QCOMPARE(positions.size(), 1);                       // the stale one is gone
        QCOMPARE(positions[0].symbol, QStringLiteral("GER40"));
        QCOMPARE(positions[0].positionId, QStringLiteral("11"));
        // eToro's own P/L was overlaid from /pnl, then re-anchored to the live
        // close rate (which equals the snapshot's here, so the figure stands).
        QVERIFY(positions[0].profitFromApi);
        QVERIFY(std::abs(positions[0].profit - 42.5) < 1e-9);
    }

    //! @tstid TS-CLI-005 @design DES-SVC-CLIENT
    // @relation(REQ-F-014, scope=function)
    void TS_CLI_005_lateIdResolutionStillNamesTrades()
    {
        // Regression: trades were named while the history pages were parsed, but
        // the listed-instrument id resolution runs concurrently at startup — so a
        // fast history walk froze "#<id>" onto every instrument whose resolution
        // hadn't landed yet, and the per-instrument summary showed only the
        // force-mapped current instrument. Naming now happens when the walk
        // completes.
        //
        // The ordering that reproduces it: the history pages are parsed BEFORE the
        // searches answer (so parse-time naming would see an empty map), and the
        // walk finishes AFTER they do. That order is stated, not timed: the
        // searches are held until the first history page has been served, and the
        // rates request — the walk's last step — is held until both searches have
        // been answered. Timing it instead (searches at 200 ms, rates at 1000 ms)
        // passed locally for months and then failed on a loaded Windows runner,
        // where the 600 ms margin was not enough.
        auto pagesServed = QSharedPointer<qint32>::create(0);
        MockHttpServer server([pagesServed](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                const QUrlQuery q(QUrl(path).query());
                const QString sym = q.queryItemValue(QStringLiteral("internalSymbolFull"));
                QByteArray body = "{\"items\":[]}";
                if (sym == QLatin1String("SPX500")) {
                    body = R"({"items":[{"instrumentId":27,"internalSymbolFull":"SPX500",
                                         "displayname":"SPX500 Index","currentRate":5000.0}]})";
                } else if (sym == QLatin1String("HKG50")) {
                    body = R"({"items":[{"instrumentId":38,"internalSymbolFull":"HKG50"}]})";
                }
                return MockHttpServer::Response{200, body, {}};
            }
            if (path.contains(QStringLiteral("/trading/info/trade/history"))) {
                const QUrlQuery q(QUrl(path).query());
                const qint32 page = q.queryItemValue(QStringLiteral("page")).toInt();
                ++(*pagesServed);
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

        const Config cfg = mockConfig(server);
        EtoroClient client(cfg);
        client.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("HKG50")});

        // The two ordering rules that reproduce the regression, stated rather than
        // timed. The second one asks the CLIENT, not the mock: a response having
        // been written to the socket says nothing about the client having applied
        // it, and gating on the write is what still let the walk finish first on a
        // loaded Windows runner.
        server.holdUntil(QStringLiteral("/market-data/search"),
                         [pagesServed] { return *pagesServed > 0; });
        server.holdUntil(QStringLiteral("/market-data/instruments/rates"), [&client] {
            return (client.instrumentIdFor(QStringLiteral("SPX500")) > 0)
                   && (client.instrumentIdFor(QStringLiteral("HKG50")) > 0);
        });

        QSignalSpy summary(&client, &EtoroClient::monthlyPnlReady);
        QSignalSpy trades(&client, &EtoroClient::closedTradesReady);
        client.start();                // id resolution goes out, answers in 200 ms
        client.fetchClosedTrades(8);   // history pages answer immediately
        QVERIFY(trades.wait(kWaitMs));

        const auto list = trades.takeFirst().at(0).value<QList<ClosedTrade>>();
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[0].symbol, QStringLiteral("SPX500"));
        QVERIFY(list[0].listed);
        QCOMPARE(list[2].symbol, QStringLiteral("HKG50"));
        QVERIFY(list[2].listed);

        // Both instruments made it into the listed per-instrument summary
        // (sorted by net descending: SPX500 nets 90, HKG50 nets 10).
        const auto pnl = summary.takeLast().at(0).value<MonthlyPnl>();
        QCOMPARE(pnl.perInstrument.size(), 2);
        QCOMPARE(pnl.perInstrument[0].symbol, QStringLiteral("SPX500"));
        QCOMPARE(pnl.perInstrument[0].trades, 2);
        QCOMPARE(pnl.perInstrument[1].symbol, QStringLiteral("HKG50"));
        QVERIFY(std::abs(pnl.perInstrument[1].netProfit - 10.0) < 1e-9);
        // The per-instrument Costs column input: open+close spread estimates
        // roll up per symbol — invest×lev×spread% summed over the trades
        // (SPX500: 20000/100.5 + 5000/100.5; HKG50: 1000/200.5) (REQ-F-014).
        QVERIFY(std::abs(pnl.perInstrument[0].estSpreadCosts - 25000.0 / 100.5) < 1e-6);
        QVERIFY(std::abs(pnl.perInstrument[1].estSpreadCosts - 1000.0 / 200.5) < 1e-6);
    }

    //! @tstid TS-CLI-006 @design DES-SVC-CLIENT
    // @relation(REQ-F-014, scope=function)
    void TS_CLI_006_busyWalkQueuesLatestLookback()
    {
        // Regression: a fetch requested while a walk was paging was silently
        // dropped — opening the details dialog (13 weeks) during the startup
        // 7-week walk left the dialog showing 7 weeks of data under a "13 weeks"
        // selector. The latest overlapping request must run right after the
        // current walk (and supersede any earlier queued one).
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/trading/info/trade/history"))) {
                return MockHttpServer::Response{200, "[]", {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        const Config cfg = mockConfig(server);
        EtoroClient client(cfg);

        QSignalSpy summary(&client, &EtoroClient::monthlyPnlReady);
        const QDate today = QDate::currentDate();
        client.fetchClosedTrades(8);
        client.fetchClosedTrades(9);   // walk busy → queued…
        client.fetchClosedTrades(13);  // …and superseded by the latest request
        QVERIFY(summary.wait(kWaitMs));
        if (summary.count() < 2) {
            QVERIFY(summary.wait(kWaitMs));
        }
        QCOMPARE(summary.count(), 2);  // 8-week walk + queued 13-week walk only
        const auto first = summary.at(0).at(0).value<MonthlyPnl>();
        const auto second = summary.at(1).at(0).value<MonthlyPnl>();
        QCOMPARE(first.fromDate, today.addDays(-7LL * 8));
        QCOMPARE(second.fromDate, today.addDays(-7LL * 13));
        // No third walk: the 9-week request was overwritten, not queued behind.
        QVERIFY(!summary.wait(300));
    }
};

QTEST_GUILESS_MAIN(TestEtoroClient)
#include "tst_etoroclient.moc"
