// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Integration tests for the eToro client's closed-trade history walk
// (DES-SVC-CLIENT) against an in-process mock of the public API: the pager
// accumulates multiple pages, and the cost estimator prices each trade from
// the bulk-rates spread (half-spread × invest × leverage per side).

#include "MockHttpServer.h"
#include "domain/PositionMath.h"
#include "services/Config.h"
#include "services/EtoroClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTimeZone>
#include <QtTest/QtTest>

#include <algorithm>

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

// One mock for every limit-order test: SPX500 resolves to id 27 with a live 5000/5001
// quote, an order POST is answered with postBody(), a DELETE is accepted, and
// orders:lookup is answered with lookupBody(). The limit-order tests differ only in those
// two payloads, so everything else lives here once (which is also what keeps them off the
// clone gate).
// The endpoints every ORDER-side test needs answered the same way: the instrument
// search that resolves SPX500 and a live rates row. Extracted because three tests
// otherwise repeat the same fifteen lines, which the clone gate rightly objects to.
MockHttpServer::Response commonMarketData(const QString &path, bool *handled)
{
    *handled = true;
    if (path.contains(QStringLiteral("/market-data/search"))) {
        const bool ger = path.contains(QStringLiteral("GER40"));
        return MockHttpServer::Response{
            200,
            QStringLiteral(R"({"items":[{"instrumentId":%1,
                "internalSymbolFull":"%2","displayname":"%2",
                "currentRate":5000.0}]})")
                .arg(ger ? QStringLiteral("1001") : QStringLiteral("27"),
                     ger ? QStringLiteral("GER40") : QStringLiteral("SPX500"))
                .toUtf8(),
            {}};
    }
    if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        return MockHttpServer::Response{
            200,
            QStringLiteral(R"({"rates":[{"instrumentId":27,"bid":5000.0,"ask":5001.0,
                                         "date":"%1"}]})")
                .arg(now)
                .toUtf8(),
            {}};
    }
    *handled = false;
    return MockHttpServer::Response{404, "{}", {}};
}

MockHttpServer::Handler limitOrderMock(std::function<QByteArray()> postBody,
                                      std::function<QByteArray()> lookupBody,
                                      std::function<QByteArray()> portfolioBody = {})
{
    return [postBody = std::move(postBody), lookupBody = std::move(lookupBody),
            portfolioBody = std::move(portfolioBody)](
               const QByteArray &method, const QString &path) {
        if (portfolioBody && path.contains(QStringLiteral("/portfolio"))) {
            return MockHttpServer::Response{200, portfolioBody(), {}};
        }
        if (path.contains(QStringLiteral("/market-data/search"))) {
            return MockHttpServer::Response{200, R"({"items":[{"instrumentId":27,
                "internalSymbolFull":"SPX500","displayname":"S&P 500",
                "currentRate":5000.0}]})", {}};
        }
        if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
            const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            return MockHttpServer::Response{
                200,
                QStringLiteral(R"({"rates":[{"instrumentId":27,"bid":5000.0,"ask":5001.0,
                                             "date":"%1"}]})").arg(now).toUtf8(),
                {}};
        }
        if (path.contains(QStringLiteral("orders:lookup"))) {
            return MockHttpServer::Response{200, lookupBody(), {}};
        }
        if (path.contains(QStringLiteral("/execution"))) {
            if (method == "POST") {
                return MockHttpServer::Response{200, postBody(), {}};
            }
            if (method == "DELETE") {
                return MockHttpServer::Response{200, "{}", {}};
            }
        }
        return MockHttpServer::Response{404, "{}", {}};
    };
}

// Client + the two spies every limit-order test watches, wired and started against the
// mock. One place, because four tests otherwise open with the same dozen lines (which the
// clone gate rightly objects to). pollMs = 50000 means "no background price polling":
// what the test then observes can only be the limit-order paths.
struct LimitOrderFixture {
    explicit LimitOrderFixture(const MockHttpServer &server, qint32 pollMs)
        : client(clientConfig(server, pollMs))
        , ready(&client, &EtoroClient::ready)
        , pending(&client, &EtoroClient::pendingOrdersUpdated)
        , results(&client, &EtoroClient::orderResult)
    {
        client.setTradableSymbols({QStringLiteral("SPX500")});
        client.start();
    }

    // True once the instrument is resolved — until then no order can be placed.
    [[nodiscard]] bool resolved() { return ready.wait(kWaitMs); }

    EtoroClient client;
    QSignalSpy ready;
    QSignalSpy pending;
    QSignalSpy results;

private:
    static Config clientConfig(const MockHttpServer &server, qint32 pollMs)
    {
        Config cfg = mockConfig(server);
        cfg.pollIntervalMs = pollMs;
        return cfg;
    }
};

// A limit order with the trade-panel-shaped fields the tests vary.
OrderRequest limitOrder(bool isBuy, double trigger, double stopLoss = 0.0,
                        double takeProfit = 0.0)
{
    OrderRequest req;
    req.isBuy = isBuy;
    req.amount = 1000.0;
    req.leverage = 5.0;
    req.stopLossAmount = stopLoss;
    req.takeProfitAmount = takeProfit;
    req.triggerRate = trigger;
    return req;
}

// The one order-status payload shape the tests need: an id, a name, and optionally
// eToro's own rejection reason.
QByteArray orderStatusBody(qint64 orderId, qint32 statusId, const QString &name,
                           const QString &errorMessage = {}, qint32 errorCode = 0)
{
    return QStringLiteral(R"({"orderId":%1,"status":{"id":%2,"name":"%3","errorCode":%4,
                              "errorMessage":%5},"positionExecutions":[]})")
        .arg(orderId)
        .arg(statusId)
        .arg(name)
        .arg(errorCode)
        .arg(errorMessage.isEmpty() ? QStringLiteral("null")
                                    : QStringLiteral("\"%1\"").arg(errorMessage))
        .toUtf8();
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

    //! @tstid TS-CLI-007 @design DES-SVC-CLIENT
    // @relation(REQ-F-015, scope=function)
    void TS_CLI_007_marketOpenFollowsQuoteAdvance()
    {
        // Regression: market-open was judged from the ABSOLUTE age of the quote
        // against a 300 s threshold. eToro's public rates feed then began publishing
        // minutes behind real time (~6 min on the indices), so every quote looked
        // frozen, every instrument read "closed" at once and BUY/SELL stayed locked
        // through an open session. What decides it now is whether the timestamp
        // ADVANCES between two polls — a property no feed delay can fake.
        //
        // All three instruments are stamped behind real time. SPX500's stamp moves on
        // every reply (session live), HKG50's is frozen at its first value (session
        // over, the feed still serving the last price), EURUSD's is two days old
        // (weekend, i.e. stale on the very first look).
        auto frozenStamp = QSharedPointer<QString>::create();
        auto weekendStamp = QSharedPointer<QString>::create();
        MockHttpServer server([frozenStamp, weekendStamp](const QByteArray &,
                                                         const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                const QUrlQuery q(QUrl(path).query());
                const QString sym = q.queryItemValue(QStringLiteral("internalSymbolFull"));
                QByteArray body = "{\"items\":[]}";
                if (sym == QLatin1String("SPX500")) {
                    body = R"({"items":[{"instrumentId":27,"internalSymbolFull":"SPX500",
                                         "displayname":"SPX500 Index","currentRate":5000.0}]})";
                } else if (sym == QLatin1String("HKG50")) {
                    body = R"({"items":[{"instrumentId":38,"internalSymbolFull":"HKG50"}]})";
                } else if (sym == QLatin1String("EURUSD")) {
                    body = R"({"items":[{"instrumentId":1,"internalSymbolFull":"EURUSD"}]})";
                }
                return MockHttpServer::Response{200, body, {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                constexpr qint64 kFeedDelaySecs = 370;  // the real feed's lag, past the old gate
                const QDateTime now = QDateTime::currentDateTimeUtc();
                const QString live = now.addSecs(-kFeedDelaySecs).toString(Qt::ISODate);
                if (frozenStamp->isEmpty()) {
                    // Both closed markets get their stamp fixed on the first reply: a
                    // session that has ended leaves the timestamp standing where it
                    // stopped, which is precisely what makes it detectable.
                    *frozenStamp = live;
                    *weekendStamp = now.addDays(-2).toString(Qt::ISODate);
                }
                // Answers with all three rows whatever ids were asked for: the request
                // carries the ids resolved when it was BUILT, and the subject here is
                // the open/closed classification, not the batching.
                const QByteArray body =
                    QStringLiteral(R"({"rates":[
                        {"instrumentId":27,"bid":100.0,"ask":101.0,"date":"%1"},
                        {"instrumentId":38,"bid":200.0,"ask":201.0,"date":"%2"},
                        {"instrumentId":1,"bid":1.1,"ask":1.2,"date":"%3"}]})")
                        .arg(live, *frozenStamp, *weekendStamp)
                        .toUtf8();
                return MockHttpServer::Response{200, body, {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        Config cfg = mockConfig(server);
        cfg.pollIntervalMs = 25;  // the poll re-checks every 60 ticks → ~1.5 s apart
        EtoroClient client(cfg);
        client.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("HKG50"),
                                   QStringLiteral("EURUSD")});
        // Stated, not timed: the first rates reply waits until the CLIENT holds all
        // three ids, so the first classification covers all three instruments. Gating
        // on the mock's write would not prove the client had applied them.
        server.holdUntil(QStringLiteral("/market-data/instruments/rates"), [&client] {
            return (client.instrumentIdFor(QStringLiteral("SPX500")) > 0)
                   && (client.instrumentIdFor(QStringLiteral("HKG50")) > 0)
                   && (client.instrumentIdFor(QStringLiteral("EURUSD")) > 0);
        });

        QSignalSpy tradeable(&client, &EtoroClient::tradeabilityUpdated);
        client.start();
        QVERIFY(tradeable.wait(kWaitMs));

        // Poll 1 has no earlier timestamp to compare against, so the delay-absorbing
        // fallback decides: the two quotes minutes behind real time pass, the one two
        // days behind does not.
        const auto first = tradeable.at(0).at(0).value<QSet<QString>>();
        QVERIFY(first.contains(QStringLiteral("SPX500")));
        QVERIFY(first.contains(QStringLiteral("HKG50")));
        QVERIFY(!first.contains(QStringLiteral("EURUSD")));

        // Poll 2 has its baseline, and now advancement alone decides.
        while (tradeable.count() < 2) {
            QVERIFY(tradeable.wait(kWaitMs));
        }
        const auto second = tradeable.at(1).at(0).value<QSet<QString>>();
        QVERIFY(second.contains(QStringLiteral("SPX500")));  // minutes behind, yet moving
        QVERIFY(!second.contains(QStringLiteral("HKG50")));  // stamp never moved ⇒ closed
        QVERIFY(!second.contains(QStringLiteral("EURUSD")));
    }

    //! @tstid TS-CLI-008 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_008_limitOrderGoesOutAsMitAndRestsUntilFilled()
    {
        // A limit order must leave the app as eToro's OWN resting order — orderType
        // "mit" with a triggerRate — not as a market order the app fires itself when it
        // sees the price. Its SL/TP must be measured from the TRIGGER rate: the position
        // opens there, so pricing them off today's quote would put the stop at a
        // distance the trade never had.
        auto orderStatus = QSharedPointer<qint32>::create(11);  // 11 = WaitingForMarket
        // The status flips from "waiting" to "filled" when the test says so.
        MockHttpServer server(limitOrderMock(
            [] { return QByteArray(R"({"orderId":13902598})"); },
            [orderStatus] {
                return orderStatusBody(13902598, *orderStatus,
                                       (*orderStatus == 3) ? QStringLiteral("Filled")
                                                           : QStringLiteral("Waiting for market"));
            }));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        LimitOrderFixture fix(server, 25);
        QVERIFY(fix.resolved());
        EtoroClient &client = fix.client;
        QSignalSpy &pending = fix.pending;
        QSignalSpy &results = fix.results;

        // 1000 at x5 entered at 4000 = 1.25 units; a 100 stop is 80 below THAT rate.
        client.openPosition(limitOrder(true, 4000.0, 100.0, 200.0));
        QVERIFY(pending.wait(kWaitMs));

        const QJsonObject sent =
            QJsonDocument::fromJson(server.lastBodyFor(QStringLiteral("/execution"))).object();
        QCOMPARE(sent.value(QStringLiteral("orderType")).toString(), QStringLiteral("mit"));
        QCOMPARE(sent.value(QStringLiteral("triggerRate")).toDouble(), 4000.0);
        QCOMPARE(sent.value(QStringLiteral("transaction")).toString(), QStringLiteral("buy"));
        // 1000 × 5 / 4000 = 1.25 units → SL 80 below / TP 160 above the TRIGGER rate,
        // NOT below the 5001 ask a market order would have used.
        QCOMPARE(sent.value(QStringLiteral("stopLossRate")).toDouble(), 3920.0);
        QCOMPARE(sent.value(QStringLiteral("takeProfitRate")).toDouble(), 4160.0);

        // It is now resting, listed with the broker's id — and no position was claimed.
        const QList<PendingOrder> resting = client.pendingOrders();
        QCOMPARE(resting.size(), 1);
        const PendingOrder &only = resting.constFirst();
        QCOMPARE(only.orderId, QStringLiteral("13902598"));
        QCOMPARE(only.triggerRate, 4000.0);
        QVERIFY(only.isBuy);

        // The status poll picks up the broker's own wording while it waits…
        const auto restingStatus = [&client] {
            const QList<PendingOrder> now = client.pendingOrders();
            return now.isEmpty() ? QString() : now.constFirst().status;
        };
        while (restingStatus() != QLatin1String("Waiting for market")) {
            QVERIFY(pending.wait(kWaitMs));
        }
        // …and drops the order once eToro reports it filled, reporting the fill.
        *orderStatus = 3;
        const qsizetype resultsBefore = results.count();
        while (!client.pendingOrders().isEmpty()) {
            QVERIFY(pending.wait(kWaitMs));
        }
        QVERIFY(results.count() > resultsBefore);
        QVERIFY(results.last().at(1).toString().contains(QStringLiteral("triggered")));
    }

    //! @tstid TS-CLI-009 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_009_cancelSendsDeleteAndDropsTheRestingOrder()
    {
        MockHttpServer server(limitOrderMock(
            [] { return QByteArray(R"({"orderId":777})"); },
            [] { return orderStatusBody(777, 11, QStringLiteral("Waiting for market")); }));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        LimitOrderFixture fix(server, 50000);  // no price polling: only order paths run
        QVERIFY(fix.resolved());
        EtoroClient &client = fix.client;
        QSignalSpy &pending = fix.pending;

        OrderRequest req = limitOrder(false, 5200.0, 100.0, 200.0);
        req.amount = 500.0;
        req.leverage = 2.0;
        client.openPosition(req);
        QVERIFY(pending.wait(kWaitMs));
        QCOMPARE(client.pendingOrders().size(), 1);

        // A SHORT entry mirrors the long: eToro opens it with "sellShort", the stop sits
        // ABOVE the trigger and the target BELOW it. Getting these the wrong way round is
        // exactly what a broker rejects, so they are pinned here.
        // 500 × 2 / 5200 = 0.192307 units → SL 520 above, TP 1040 below the trigger.
        const QJsonObject shortBody =
            QJsonDocument::fromJson(server.lastBodyFor(QStringLiteral("/execution"))).object();
        QCOMPARE(shortBody.value(QStringLiteral("transaction")).toString(),
                 QStringLiteral("sellShort"));
        QCOMPARE(shortBody.value(QStringLiteral("orderType")).toString(), QStringLiteral("mit"));
        QCOMPARE(shortBody.value(QStringLiteral("triggerRate")).toDouble(), 5200.0);
        QCOMPARE(shortBody.value(QStringLiteral("stopLossRate")).toDouble(), 5720.0);
        QCOMPARE(shortBody.value(QStringLiteral("takeProfitRate")).toDouble(), 4160.0);
        QCOMPARE(shortBody.value(QStringLiteral("stopLossType")).toString(),
                 QStringLiteral("fixed"));

        client.cancelPendingOrder(QStringLiteral("777"));
        while (!client.pendingOrders().isEmpty()) {
            QVERIFY(pending.wait(kWaitMs));
        }
        // The cancel went to the documented per-order endpoint, as a DELETE.
        const auto sent = server.requests();
        const bool cancelled =
            std::any_of(sent.cbegin(), sent.cend(), [](const MockHttpServer::Recorded &r) {
                return (r.method == "DELETE")
                       && r.path.contains(QStringLiteral("/trading/execution/demo/orders/777"));
            });
        QVERIFY2(cancelled, "no DELETE to /trading/execution/demo/orders/777");
    }

    //! @tstid TS-CLI-010 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_010_rejectedLimitOrderReportsEtorosOwnReason()
    {
        // Field regression: eToro accepted the POST (so the app listed the order as
        // resting) and rejected the order seconds later. The app used to note that as a
        // bland "no longer resting" info line and to notice it only on the lazy periodic
        // poll — so it kept showing a dead order and never said why. The lookup carries
        // eToro's reason in status.errorMessage; that text is the whole point.
        MockHttpServer server(limitOrderMock(
            [] { return QByteArray(R"({"orderId":4242})"); },
            [] {
                return orderStatusBody(4242, 4, QStringLiteral("Rejected"),
                                       QStringLiteral("Market is closed for this instrument"),
                                       1503);
            }));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        LimitOrderFixture fix(server, 50000);  // the prompt check, not a poll, must catch it
        QVERIFY(fix.resolved());
        EtoroClient &client = fix.client;
        QSignalSpy &pending = fix.pending;
        QSignalSpy &results = fix.results;

        client.openPosition(limitOrder(false, 5200.0));
        QVERIFY(pending.wait(kWaitMs));
        QCOMPARE(client.pendingOrders().size(), 1);

        // The early watch fires within seconds and drops the order again…
        while (!client.pendingOrders().isEmpty()) {
            QVERIFY(pending.wait(kWaitMs));
        }
        // …reporting it as a FAILURE that quotes eToro's reason and its code.
        QVERIFY(!results.isEmpty());
        const QList<QVariant> last = results.last();
        QVERIFY2(!last.at(0).toBool(), "a rejected limit order must be reported as an error");
        const QString message = last.at(1).toString();
        QVERIFY2(message.contains(QStringLiteral("Market is closed for this instrument")),
                 qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("1503")), qPrintable(message));
    }

    //! @tstid TS-CLI-011 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_011_adjustingARestingOrderCancelsAndReplacesIt()
    {
        // eToro has no update-order endpoint, so changing a resting order's SL/TP means
        // DELETE + a fresh POST — in that order, or a failed cancel would leave two live
        // orders for one intended trade. The replacement keeps size, leverage and side.
        // A different order id per POST, as the broker would hand out.
        auto nextId = QSharedPointer<qint32>::create(900);
        MockHttpServer server(limitOrderMock(
            [nextId] {
                return QStringLiteral(R"({"orderId":%1})").arg(++(*nextId)).toUtf8();
            },
            [nextId] {
                return orderStatusBody(*nextId, 11, QStringLiteral("Waiting for market"));
            }));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        LimitOrderFixture fix(server, 50000);
        QVERIFY(fix.resolved());
        EtoroClient &client = fix.client;
        QSignalSpy &pending = fix.pending;

        client.openPosition(limitOrder(true, 4000.0, 100.0));
        QVERIFY(pending.wait(kWaitMs));
        const QString firstId = client.pendingOrders().constFirst().orderId;

        // New trigger 4100 with SL 200 / TP 400: 1000 × 5 / 4100 = 1.2195 units →
        // SL 164 below, TP 328 above the NEW trigger rate.
        client.modifyPendingOrder(firstId, 4100.0, 200.0, 400.0);
        while (client.pendingOrders().isEmpty()
               || (client.pendingOrders().constFirst().orderId == firstId)) {
            QVERIFY(pending.wait(kWaitMs));
        }
        const PendingOrder replaced = client.pendingOrders().constFirst();
        QCOMPARE(client.pendingOrders().size(), 1);   // one order, not two
        QCOMPARE(replaced.triggerRate, 4100.0);
        QCOMPARE(replaced.stopLossAmount, 200.0);
        QCOMPARE(replaced.takeProfitAmount, 400.0);
        QCOMPARE(replaced.amount, 1000.0);            // size and leverage carried over
        QCOMPARE(replaced.leverage, 5.0);
        QVERIFY(replaced.isBuy);

        // The cancel went out BEFORE the replacement POST, and the new body carries the
        // new trigger with SL/TP measured from it.
        const QList<MockHttpServer::Recorded> sent = server.requests();
        qsizetype deleteAt = -1;
        qsizetype secondPostAt = -1;
        QByteArray replacementBody;
        qint32 posts = 0;
        for (qsizetype i = 0; i < sent.size(); ++i) {
            if (!sent[i].path.contains(QStringLiteral("/execution"))) {
                continue;
            }
            if (sent[i].method == "DELETE") {
                deleteAt = i;
            } else if (sent[i].method == "POST") {
                ++posts;
                if (posts == 2) {
                    secondPostAt = i;
                    replacementBody = sent[i].body;
                }
            }
        }
        QCOMPARE(posts, 2);
        QVERIFY2((deleteAt >= 0) && (deleteAt < secondPostAt),
                 "the cancel must precede the replacement order");
        const QJsonObject body = QJsonDocument::fromJson(replacementBody).object();
        QCOMPARE(body.value(QStringLiteral("triggerRate")).toDouble(), 4100.0);
        QCOMPARE(body.value(QStringLiteral("stopLossRate")).toDouble(), 3936.0);
        QCOMPARE(body.value(QStringLiteral("takeProfitRate")).toDouble(), 4428.0);
        QCOMPARE(body.value(QStringLiteral("instrumentId")).toInt(), 27);
    }

    //! @tstid TS-CLI-014 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_014_adjustingTargetsTheOrdersOwnInstrument()
    {
        // Field regression: adjusting an order was refused unless its instrument happened
        // to be the one on screen ("Select SPX500 first"). A limit order is priced off its
        // own trigger rate, so the replacement can — and must — go to the ORDER's
        // instrument whatever the app is showing.
        // The broker reports one resting order on instrument 38 — until it is replaced.
        auto listOldOrder = QSharedPointer<bool>::create(true);
        MockHttpServer server(limitOrderMock(
            [] { return QByteArray(R"({"orderId":4712})"); },
            [] { return orderStatusBody(4712, 11, QStringLiteral("Waiting for market")); },
            [listOldOrder] {
                return *listOldOrder
                           ? QByteArray(R"({"clientPortfolio":{"positions":[],
                                "orders":[{"orderID":4711,"instrumentID":38,"isBuy":false,
                                           "rate":200.0,"amount":500.0,"leverage":2,
                                           "units":5.0,"stopLossRate":0.0,
                                           "takeProfitRate":0.0,"isTslEnabled":false,
                                           "openDateTime":"2026-07-29T09:00:00Z"}]}})")
                           : QByteArray(R"({"clientPortfolio":{"positions":[],"orders":[]}})");
            }));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        LimitOrderFixture fix(server, 25);  // the app trades instrument 27 (SPX500)
        QVERIFY(fix.resolved());

        // It shows up from the portfolio, on its own instrument.
        while (fix.client.pendingOrders().isEmpty()) {
            QVERIFY(fix.pending.wait(kWaitMs));
        }
        QCOMPARE(fix.client.pendingOrders().constFirst().instrumentId, static_cast<qint64>(38));

        *listOldOrder = false;  // the broker stops listing it once it is cancelled
        fix.client.modifyPendingOrder(QStringLiteral("4711"), 210.0, 50.0, 100.0);
        while (fix.client.pendingOrders().isEmpty()
               || (fix.client.pendingOrders().constFirst().orderId
                   != QStringLiteral("4712"))) {
            QVERIFY(fix.pending.wait(kWaitMs));
        }
        // The replacement went to instrument 38, not to the 27 on screen, and kept the
        // order's own side, size and leverage.
        const QJsonObject body =
            QJsonDocument::fromJson(server.lastBodyFor(QStringLiteral("/execution"))).object();
        QCOMPARE(body.value(QStringLiteral("instrumentId")).toInt(), 38);
        QCOMPARE(body.value(QStringLiteral("transaction")).toString(),
                 QStringLiteral("sellShort"));
        QCOMPARE(body.value(QStringLiteral("triggerRate")).toDouble(), 210.0);
        QCOMPARE(body.value(QStringLiteral("amount")).toDouble(), 500.0);
        QCOMPARE(body.value(QStringLiteral("leverage")).toInt(), 2);
        // 500 × 2 / 210 = 4.7619 units → SL 10.5 above, TP 21 below the new trigger.
        QCOMPARE(body.value(QStringLiteral("stopLossRate")).toDouble(), 220.5);
        QCOMPARE(body.value(QStringLiteral("takeProfitRate")).toDouble(), 189.0);
    }

    //! @tstid TS-CLI-016 @design DES-SVC-CLIENT
    // @relation(REQ-F-019, REQ-F-024, scope=function)
    void TS_CLI_016_aRejectedOrderNamesTheFieldTheBrokerObjectedTo()
    {
        // eToro rejects with ASP.NET ValidationProblemDetails: a generic title and the
        // real reasons under "errors": {field: [msg, …]}. Reporting only the title
        // ("One or more validation errors occurred.") is what made a rejected order
        // unexplainable — the message has to name the offending field.
        MockHttpServer server([](const QByteArray &method, const QString &path) {
            bool handled = false;
            MockHttpServer::Response common = commonMarketData(path, &handled);
            if (handled) {
                return common;   // moved out, so not const
            }
            if (path.contains(QStringLiteral("/execution")) && (method == "POST")) {
                return MockHttpServer::Response{
                    400,
                    R"({"title":"One or more validation errors occurred.","status":400,
                        "errors":{"orderCurrency":["'eur' is not supported for this account"],
                                  "units":["exceeds maxUnitsPerOrder"]}})",
                    {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        LimitOrderFixture fix(server, 50000);
        QVERIFY(fix.resolved());

        fix.client.openPosition(limitOrder(/*isBuy=*/true, 4900.0));
        QVERIFY(fix.results.wait(kWaitMs));
        const QList<QVariant> args = fix.results.takeLast();
        QVERIFY(!args.at(0).toBool());
        const QString message = args.at(1).toString();
        // The generic title alone is useless; the per-field reasons are the answer.
        QVERIFY2(message.contains(QStringLiteral("orderCurrency")), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("not supported")), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("units")), qPrintable(message));
    }

    //! @tstid TS-CLI-017 @design DES-SVC-CLIENT
    // @relation(REQ-F-019, scope=function)
    void TS_CLI_017_leverageComesFromTheAccountsCfdConfiguration()
    {
        // The eligibility endpoint answers per instrument with leverageConfigs; only the
        // CFD settlement type applies to what this app trades, and the values arrive
        // unsorted and with duplicates.
        MockHttpServer server([](const QByteArray & /*method*/, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, R"({"items":[{"instrumentId":27,
                    "internalSymbolFull":"SPX500","displayname":"S&P 500",
                    "currentRate":5000.0}]})", {}};
            }
            if (path.contains(QStringLiteral("/eligibility"))) {
                // The real shape: {"eligibilities":[…]} — one entry per requested id.
                return MockHttpServer::Response{
                    200,
                    R"({"eligibilities":[{"instrumentId":27,"leverageConfigs":[
                          {"settlementType":"realStock","leverageValues":[1]},
                          {"settlementType":"CFD","leverageValues":[10,2,20,2,0,5]}]}]})",
                    {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"rates":[{"instrumentId":27,"bid":5000.0,"ask":5001.0,
                                                 "date":"%1"}]})").arg(now).toUtf8(),
                    {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        EtoroClient client(mockConfig(server));
        QSignalSpy levels(&client, &EtoroClient::leverageOptions);
        client.setTradableSymbols({QStringLiteral("SPX500")});
        client.start();
        QVERIFY(levels.wait(kWaitMs));
        const auto values = levels.takeLast().at(0).value<QList<int>>();
        // CFD only (the realStock 1 is not offered), sorted, de-duplicated, and the
        // nonsense 0 dropped.
        QCOMPARE(values, QList<int>({2, 5, 10, 20}));
    }

    //! @tstid TS-CLI-018 @design DES-SVC-CLIENT
    // @relation(REQ-F-019, REQ-F-025, scope=function)
    void TS_CLI_018_closingAndAdjustingAPositionReportTheirOutcome()
    {
        // Both money-moving position operations, including the failure branch: a
        // rejected close has to say so rather than leave the row looking closed.
        auto failNext = QSharedPointer<bool>::create(false);
        MockHttpServer server([failNext](const QByteArray &method, const QString &path) {
            bool handled = false;
            MockHttpServer::Response common = commonMarketData(path, &handled);
            if (handled) {
                return common;   // moved out, so not const
            }
            if (path.contains(QStringLiteral("/positions"))) {
                if (*failNext) {
                    return MockHttpServer::Response{
                        409, R"({"detail":"position already closed"})", {}};
                }
                // A close is POSTed to market-close-orders/positions/<id>; an SL/TP
                // change is a PATCH on positions/<id>. Both land here.
                static_cast<void>(method);
                return MockHttpServer::Response{200, "{}", {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        LimitOrderFixture fix(server, 50000);
        QVERIFY(fix.resolved());

        QSignalSpy closed(&fix.client, &EtoroClient::positionClosed);
        fix.client.closePosition(QStringLiteral("998877"));
        QVERIFY(closed.wait(kWaitMs));
        QVERIFY(closed.takeLast().at(0).toBool());

        // Adjusting SL/TP goes down the same road and reports through the same signal.
        QSignalSpy logs(&fix.client, &EtoroClient::log);
        fix.client.modifyPosition(QStringLiteral("998877"), 4900.0, 5200.0, false);
        QVERIFY(logs.wait(kWaitMs) || closed.wait(kWaitMs));

        // …and a refusal is reported as one, with the broker's own reason.
        *failNext = true;
        fix.client.closePosition(QStringLiteral("998877"));
        QVERIFY(closed.wait(kWaitMs));
        const QList<QVariant> refusal = closed.takeLast();
        QVERIFY(!refusal.at(0).toBool());
        QVERIFY2(refusal.at(1).toString().contains(QStringLiteral("already closed")),
                 qPrintable(refusal.at(1).toString()));
    }

    //! @tstid TS-CLI-019 @design DES-SVC-CLIENT
    // @relation(REQ-F-019, REQ-F-020, scope=function)
    void TS_CLI_019_theScreenerWalksEveryResolvedInstrument()
    {
        // The screener is one bulk eligibility call for the leverage caps, then one
        // candle request per instrument, then a row each. Progress is reported as it
        // goes and finished exactly once — a scan that ends silently is a scan the
        // window waits on forever.
        MockHttpServer server([](const QByteArray & /*method*/, const QString &path) {
            bool handled = false;
            MockHttpServer::Response common = commonMarketData(path, &handled);
            if (handled) {
                return common;   // search resolves BOTH listed instruments (moved out)
            }
            if (path.contains(QStringLiteral("/eligibility"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"eligibilities":[
                         {"instrumentId":27,"leverageConfigs":[
                            {"settlementType":"CFD","leverageValues":[2,5,20]}]},
                         {"instrumentId":1001,"leverageConfigs":[
                            {"settlementType":"CFD","leverageValues":[2,10]}]}]})",
                    {}};
            }
            if (path.contains(QStringLiteral("/history/candles"))) {
                // 60 hourly candles: enough for the indicators the row carries.
                QByteArray candles = "{\"candles\":[";
                for (int i = 0; i < 60; ++i) {
                    if (i > 0) {
                        candles += ',';
                    }
                    const double close = 5000.0 + (i * 3.0);
                    candles += QStringLiteral(R"({"fromDate":"2026-08-01T%1:00:00Z",
                        "open":%2,"high":%3,"low":%4,"close":%2})")
                                   .arg(i % 24, 2, 10, QLatin1Char('0'))
                                   .arg(close, 0, 'f', 2)
                                   .arg(close + 5.0, 0, 'f', 2)
                                   .arg(close - 5.0, 0, 'f', 2)
                                   .toUtf8();
                }
                candles += "]}";
                return MockHttpServer::Response{200, candles, {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"rates":[{"instrumentId":27,"bid":5000.0,"ask":5001.0,
                                                 "date":"%1"}]})").arg(now).toUtf8(),
                    {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        Config cfg = mockConfig(server);
        cfg.pollIntervalMs = 50000;   // no background polling: only the scan is observed
        EtoroClient client(cfg);
        QSignalSpy ready(&client, &EtoroClient::ready);
        const QSignalSpy progress(&client, &EtoroClient::screenerProgress);
        QSignalSpy finished(&client, &EtoroClient::screenerFinished);
        const QSignalSpy rows(&client, &EtoroClient::screenerRow);
        client.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("GER40")});
        client.start();
        QVERIFY(ready.wait(kWaitMs));

        // A scan works on the ids resolved SO FAR and nudges the stragglers, so the
        // documented contract is that a rescan is complete — which is what this waits
        // for rather than racing the first one.
        client.scanInstruments();
        QVERIFY(finished.wait(kWaitMs * 4));
        QCOMPARE(finished.size(), 1);          // exactly once per scan
        QVERIFY(!progress.isEmpty());          // and it reported as it went
        for (int attempt = 0; (attempt < 4) && (rows.size() < 2); ++attempt) {
            finished.clear();
            client.scanInstruments();
            QVERIFY(finished.wait(kWaitMs * 4));
        }

        // One signal per instrument, as its data arrives.
        QVERIFY2(rows.size() >= 2, qPrintable(QStringLiteral("%1 rows").arg(rows.size())));
        QList<ScreenerRow> scanned;
        for (const QList<QVariant> &emitted : rows) {
            scanned.append(emitted.at(0).value<ScreenerRow>());
        }
        // The leverage column is the CAP from the bulk eligibility call, per instrument.
        for (const ScreenerRow &row : scanned) {
            if (row.symbol == QStringLiteral("SPX500")) {
                QCOMPARE(row.maxLeverage, 20);
            }
            if (row.symbol == QStringLiteral("GER40")) {
                QCOMPARE(row.maxLeverage, 10);
            }
            QVERIFY(!row.closes.isEmpty());    // the candles reached the row
        }
    }

    //! @tstid TS-CLI-012 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_012_restingOrdersAreRefreshedOnTheirOwn4sCycle()
    {
        // The resting-order list must stay current on its OWN cadence: the price poll is
        // configurable (here 50 s), and how fresh "is my order still waiting?" is must not
        // depend on that setting. The order stays WaitingForMarket throughout, so every
        // lookup after the first is the cycle doing its work.
        auto lookups = QSharedPointer<qint32>::create(0);
        MockHttpServer server(limitOrderMock(
            [] { return QByteArray(R"({"orderId":555})"); },
            [lookups] {
                ++(*lookups);
                return orderStatusBody(555, 11, QStringLiteral("Waiting for market"));
            }));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        LimitOrderFixture fix(server, 50000);  // the price poll cannot be what refreshes it
        QVERIFY(fix.resolved());
        EtoroClient &client = fix.client;
        QSignalSpy &pending = fix.pending;

        client.openPosition(limitOrder(true, 4000.0));
        QVERIFY(pending.wait(kWaitMs));  // shown as resting immediately

        // Three lookups = the prompt post-placement check plus two cycle ticks.
        QElapsedTimer clock;
        clock.start();
        while ((*lookups < 3) && (clock.elapsed() < kWaitMs)) {
            QTest::qWait(200);
        }
        QVERIFY2(*lookups >= 3, qPrintable(QStringLiteral("only %1 lookups in %2 ms")
                                               .arg(*lookups)
                                               .arg(clock.elapsed())));
        // …and they are spaced, not spun: at a 4 s cycle the third cannot arrive early.
        QVERIFY2(clock.elapsed() >= 4000,
                 qPrintable(QStringLiteral("%1 lookups within only %2 ms — the cycle is "
                                           "polling far faster than 4 s")
                                .arg(*lookups)
                                .arg(clock.elapsed())));
        QVERIFY(client.pendingOrders().size() == 1);  // still resting, still listed
    }

    //! @tstid TS-CLI-013 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_013_restingOrdersComeFromThePortfolioNotOnlyFromThisSession()
    {
        // Field regression: the panel was empty although two limit orders were open at
        // eToro, because the list only held what THIS session had submitted. There is no
        // "list my orders" endpoint, but /portfolio carries clientPortfolio.orders[] —
        // so orders placed earlier, or in eToro's own UI, must appear from that.
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                const QUrlQuery q(QUrl(path).query());
                const QString sym = q.queryItemValue(QStringLiteral("internalSymbolFull"));
                const qint32 id = (sym == QLatin1String("HKG50")) ? 38 : 27;
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"items":[{"instrumentId":%1,"internalSymbolFull":"%2",
                                                 "currentRate":5000.0}]})").arg(id).arg(sym)
                        .toUtf8(),
                    {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                // The bulk snapshot every listed instrument shares: it is what gives the
                // panel a "Now" rate for an order on an instrument that is NOT on screen.
                return MockHttpServer::Response{
                    200,
                    R"({"rates":[{"instrumentId":27,"bid":5000.0,"ask":5002.0},
                                 {"instrumentId":38,"bid":200.0,"ask":204.0}]})",
                    {}};
            }
            if (path.contains(QStringLiteral("/portfolio"))) {
                // A long on the listed instrument 27 (SL 4900 = 100 below the 5000
                // trigger on 10 units → 1000; TP 5200 → 2000) and a short on the
                // unlisted 999, whose "no stop-loss" sentinel must not read as a stop.
                return MockHttpServer::Response{200, R"({"clientPortfolio":{"positions":[],
                    "orders":[
                      {"orderID":881,"instrumentID":27,"isBuy":true,"rate":5000.0,
                       "amount":2500.0,"leverage":20,"units":10.0,
                       "stopLossRate":4900.0,"takeProfitRate":5200.0,
                       "isTslEnabled":false,"openDateTime":"2026-07-29T08:00:00Z"},
                      {"orderID":882,"instrumentID":999,"isBuy":false,"rate":200.0,
                       "amount":500.0,"leverage":2,"units":5.0,
                       "stopLossRate":0.0,"takeProfitRate":0.0,
                       "isTslEnabled":false,"openDateTime":"2026-07-29T09:00:00Z"}]}})", {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        Config cfg = mockConfig(server);
        cfg.pollIntervalMs = 25;
        EtoroClient client(cfg);
        client.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("HKG50")});
        QSignalSpy pending(&client, &EtoroClient::pendingOrdersUpdated);
        const QSignalSpy tradeable(&client, &EtoroClient::tradeabilityUpdated);
        client.start();
        QVERIFY(pending.wait(kWaitMs));

        const QList<PendingOrder> resting = client.pendingOrders();
        QCOMPARE(resting.size(), 2);            // both, though this session placed neither
        const PendingOrder &listed = resting.constFirst();
        QCOMPARE(listed.orderId, QStringLiteral("881"));
        QCOMPARE(listed.symbol, QStringLiteral("SPX500"));
        QVERIFY(listed.isBuy);
        QCOMPARE(listed.triggerRate, 5000.0);
        QCOMPARE(listed.amount, 2500.0);
        QCOMPARE(listed.leverage, 20.0);
        // The broker states SL/TP as rates; the panel needs amounts (distance × units).
        QCOMPARE(listed.stopLossAmount, 1000.0);
        QCOMPARE(listed.takeProfitAmount, 2000.0);

        // An order on an instrument outside the selector stays VISIBLE (an order the app
        // hides is an order nobody cancels) under a "#<id>" label, and its sentinel
        // zero SL/TP rates do not become a zero-distance stop.
        const PendingOrder &unlisted = resting.at(1);
        QCOMPARE(unlisted.orderId, QStringLiteral("882"));
        QCOMPARE(unlisted.symbol, QStringLiteral("#999"));
        QVERIFY(!unlisted.isBuy);
        QCOMPARE(unlisted.stopLossAmount, 0.0);
        QCOMPARE(unlisted.takeProfitAmount, 0.0);

        // Each row also shows its own instrument's current rate, so the panel needs one
        // for instruments OTHER than the one being traded: the bulk snapshot's mid.
        while (tradeable.isEmpty()) {
            QVERIFY(pending.wait(1000) || !tradeable.isEmpty());
        }
        QCOMPARE(client.lastRateFor(38), 202.0);       // (200 + 204) / 2, not on screen
        QVERIFY(client.lastRateFor(27) > 0.0);         // the traded one has its live price
        QCOMPARE(client.lastRateFor(999), 0.0);        // unknown instrument: honestly nothing
    }

    //! @tstid TS-CLI-015 @design DES-SVC-CLIENT
    // @relation(REQ-F-025, scope=function)
    void TS_CLI_015_delayedRatesRowIsRepairedFromTheLiveCandle()
    {
        // Field regression, measured on 2026-07-30: eToro's public rates row for
        // NSDQ100.24-7 was published 6-12 minutes behind real time (and said so in its
        // `date`), while eToro's own screen showed the live price. The app marked the
        // position off that row, so its P/L read €90 below eToro's on a fast-moving
        // index. /pnl is no better — its unrealizedPnL is computed from the same
        // delayed rate. The candle feed for the same instrument IS live and its
        // 1-minute close is exactly the bid, so the mark comes from there.
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        const QString stale = nowUtc.addSecs(-11 * 60LL).toString(Qt::ISODate);
        const QString thisMinute =
            QDateTime(nowUtc.date(), QTime(nowUtc.time().hour(), nowUtc.time().minute()),
                      QTimeZone::UTC).toString(Qt::ISODate);
        MockHttpServer server([stale, thisMinute](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, R"({"items":[{"instrumentId":27,
                    "internalSymbolFull":"SPX500","currentRate":5000.0}]})", {}};
            }
            if (path.contains(QStringLiteral("/history/candles"))) {
                // Live: this minute's candle, whose close is the current bid.
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"candles":[{"instrumentId":27,"candles":[
                        {"fromDate":"%1","open":5090.0,"high":5105.0,"low":5088.0,
                         "close":5100.0}]}]})").arg(thisMinute).toUtf8(),
                    {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                // Delayed: the price of 11 minutes ago, honestly stamped.
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"rates":[{"instrumentId":27,"bid":5020.0,"ask":5022.0,
                                                 "date":"%1"}]})").arg(stale).toUtf8(),
                    {}};
            }
            if (path.contains(QStringLiteral("/pnl"))) {
                // eToro's own snapshot, marked at ITS delayed rate (5015 → 150 on 10 units).
                return MockHttpServer::Response{200, R"({"clientPortfolio":{"positions":[
                    {"positionId":77,"instrumentID":27,"isBuy":true,"amount":2500.0,
                     "leverage":20,"units":10.0,"openRate":5000.0,
                     "unrealizedPnL":{"pnL":150.0,"closeRate":5015.0}}]}})", {}};
            }
            if (path.contains(QStringLiteral("/portfolio"))) {
                return MockHttpServer::Response{200, R"({"clientPortfolio":{"positions":[
                    {"positionId":77,"instrumentID":27,"isBuy":true,"amount":2500.0,
                     "leverage":20,"units":10.0,"openRate":5000.0,
                     "openDateTime":"2026-07-30T14:12:00Z"}],"orders":[]}})", {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        Config cfg = mockConfig(server);
        cfg.pollIntervalMs = 25;
        EtoroClient client(cfg);
        client.setTradableSymbols({QStringLiteral("SPX500")});
        const QSignalSpy portfolio(&client, &EtoroClient::portfolioUpdated);
        client.start();

        // The candle bid marks the trade: 10 units × (5100 − 5000) = 1000.
        const auto profitSeen = [&portfolio](double want) {
            return std::ranges::any_of(portfolio, [want](const QList<QVariant> &emission) {
                const auto list = emission.at(0).value<QList<Position>>();
                return !list.isEmpty() && (std::abs(list.constFirst().profit - want) < 1e-6);
            });
        };
        QTRY_VERIFY_WITH_TIMEOUT(profitSeen(1000.0), kWaitMs);

        // The delayed row must NEVER have decided the figure (that would be 200), and
        // the quote book says where the mark came from.
        QVERIFY(!profitSeen(200.0));
        const Quote quote = client.quotes().value(27);
        QVERIFY(quote.fromCandle);
        QCOMPARE(quote.bid, 5100.0);
        QCOMPARE(quote.ask, 5102.0);   // the delayed row's 2.0 spread still applies
    }

    //! @tstid TS-CLI-020 @design DES-SVC-CLIENT
    // @relation(REQ-F-025, scope=function)
    void TS_CLI_020_staleQuoteLeavesClosingCostAtZero()
    {
        // Regression, measured on an NSDQ100 position moments after a symbol switch:
        // closingCost used to be priced off the bulk quote's spread unconditionally,
        // even when that quote was too old to mark the position's profit from (the
        // exact staleness this function already refuses for p.profit). The candle
        // repair that normally rescues a delayed rates row is unavailable here
        // (/history/candles 404s), so the quote stays stale and closingCost must
        // stay at its 0.0 default rather than publish a cost derived from a price
        // eToro itself would not use to mark the position.
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        const QString stale = nowUtc.addSecs(-3 * 60LL).toString(Qt::ISODate);
        MockHttpServer server([stale](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, R"({"items":[{"instrumentId":27,
                    "internalSymbolFull":"SPX500","currentRate":5000.0}]})", {}};
            }
            if (path.contains(QStringLiteral("/history/candles"))) {
                return MockHttpServer::Response{404, "{}", {}};
            }
            if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
                // Stale beyond kQuoteStaleMs (2 min), with a non-zero spread — the
                // shape that used to leak into closingCost regardless of age.
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"rates":[{"instrumentId":27,"bid":5020.0,"ask":5022.0,
                                                 "date":"%1"}]})").arg(stale).toUtf8(),
                    {}};
            }
            if (path.contains(QStringLiteral("/pnl"))) {
                return MockHttpServer::Response{200, R"({"clientPortfolio":{"positions":[
                    {"positionId":77,"instrumentID":27,"isBuy":true,"amount":2500.0,
                     "leverage":20,"units":10.0,"openRate":5000.0,
                     "unrealizedPnL":{"pnL":150.0,"closeRate":5015.0}}]}})", {}};
            }
            if (path.contains(QStringLiteral("/portfolio"))) {
                return MockHttpServer::Response{200, R"({"clientPortfolio":{"positions":[
                    {"positionId":77,"instrumentID":27,"isBuy":true,"amount":2500.0,
                     "leverage":20,"units":10.0,"openRate":5000.0,
                     "openDateTime":"2026-07-30T14:12:00Z"}],"orders":[]}})", {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        Config cfg = mockConfig(server);
        cfg.pollIntervalMs = 25;
        EtoroClient client(cfg);
        client.setTradableSymbols({QStringLiteral("SPX500")});
        QSignalSpy portfolio(&client, &EtoroClient::portfolioUpdated);
        client.start();

        // First wait for the bulk quote poll to actually have parsed the stale rates
        // row — it runs on its own cadence, separate from the portfolio poll that
        // immediately overlays /pnl's 150.0, so asserting before the quote lands would
        // pass for the wrong reason (no quote to price off at all).
        QTRY_VERIFY_WITH_TIMEOUT(client.quotes().value(27).spread() > 0.0, kWaitMs);
        const Quote quote = client.quotes().value(27);
        QVERIFY(!quote.fromCandle);
        QVERIFY(quote.ageMs(nowUtc) > trading::kQuoteStaleMs);

        // Now that the stale, non-zero-spread quote is loaded, wait for a portfolio
        // poll to run against it and check what it produced: eToro's own snapshot P/L
        // stands (the stale quote never overwrites it), and closingCost — despite that
        // live 2.0 spread sitting in the quote book — stays at its 0.0 default rather
        // than pricing a cost off a price too old to trust: (spread/2) × perPoint here
        // would wrongly be 10.0.
        portfolio.clear();
        QVERIFY(portfolio.wait(kWaitMs));
        const auto positions = portfolio.constLast().at(0).value<QList<Position>>();
        QCOMPARE(positions.size(), 1);
        QCOMPARE(positions[0].profit, 150.0);
        QCOMPARE(positions[0].closingCost, 0.0);
    }
};

QTEST_GUILESS_MAIN(TestEtoroClient)
#include "tst_etoroclient.moc"
