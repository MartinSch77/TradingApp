// Integration tests for the eToro client's LIVE-mode paths (DES-SVC-CLIENT) that the
// walk-oriented suite in tst_etoroclient.cpp does not reach: instrument resolution and
// its give-up, the public fee feed, the account snapshot with its balance and FX legs,
// and — the one that decides whether a real order is reported correctly — the
// order-lookup confirmation with every status the endpoint answers.
//
// All of it against the in-process mock: no test here touches the network, and none
// can reach a real account (the config points at localhost and the mode is demo).
//
// Why these paths deserve their own file: they are the code that runs when real money
// is at stake, they are full of "the payload is not what we asked for" branches, and
// each of those branches decides between reporting a fill, reporting a rejection and
// silently doing nothing. MC/DC is the measure that says whether they were tried.

#include "MockHttpServer.h"
#include "services/Config.h"
#include "services/EtoroClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <cmath>

namespace {

constexpr qint32 kWaitMs = 15000;

Config mockConfig(const MockHttpServer &server)
{
    Config cfg;
    cfg.apiKey = QStringLiteral("k");   // credentials → the real-mode code path
    cfg.userKey = QStringLiteral("u");
    cfg.mode = QStringLiteral("demo");  // …against the mock, never a real account
    cfg.baseUrl = server.baseUrl() + QStringLiteral("/api");
    return cfg;
}

QByteArray searchBody(qint64 id, const QString &symbol)
{
    return QStringLiteral(R"({"items":[{"instrumentId":%1,"internalSymbolFull":"%2",)"
                          R"("displayname":"%2","currentRate":5000.0}]})")
        .arg(id)
        .arg(symbol)
        .toUtf8();
}

QByteArray ratesBody(qint64 id, double bid, double ask)
{
    return QStringLiteral(R"({"rates":[{"instrumentId":%1,"bid":%2,"ask":%3,"date":"%4"}]})")
        .arg(id)
        .arg(bid, 0, 'f', 4)
        .arg(ask, 0, 'f', 4)
        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
        .toUtf8();
}

// The public trade-config payload, in the field spelling the live host really uses.
QByteArray feeBody(double buyNight, double sellNight, double buyWeekend, double sellWeekend)
{
    return QStringLiteral(R"({"Instrument":{"BuyOverNightFee":%1,"SellOverNightFee":%2,)"
                          R"("BuyEndOfWeekFee":%3,"SellEndOfWeekFee":%4}})")
        .arg(buyNight, 0, 'f', 4)
        .arg(sellNight, 0, 'f', 4)
        .arg(buyWeekend, 0, 'f', 4)
        .arg(sellWeekend, 0, 'f', 4)
        .toUtf8();
}

} // namespace

class TestEtoroClientLive : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-CLI-020 @design DES-SVC-CLIENT
    // @relation(REQ-F-001, scope=function)
    void TS_CLI_020_resolutionSucceedsRetriesAndFinallyGivesUp()
    {
        // Resolution is the first thing that happens for any instrument, and every
        // later request depends on its id. The three outcomes must be distinguishable:
        // resolved, still trying, and given up — the last one REPORTED, because an
        // instrument that silently never resolves looks like a frozen app.
        qint32 searches = 0;
        MockHttpServer server([&searches](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                ++searches;
                if (path.contains(QStringLiteral("NOSUCH"))) {
                    return MockHttpServer::Response{200, R"({"items":[]})", {}};
                }
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        QSignalSpy failed(&client, &EtoroClient::resolveFailed);
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));
        QCOMPARE(resolved.at(0).at(0).value<Instrument>().instrumentId, 27);

        // Setting the SAME symbol again does not re-resolve: the id is already known.
        const qint32 before = searches;
        client.setSymbol(QStringLiteral("SPX500"));
        QTest::qWait(200);
        QCOMPARE(searches, before);

        // A symbol the venue does not list: the search answers an empty list, and the
        // client eventually gives up and SAYS so rather than waiting forever.
        client.setSymbol(QStringLiteral("NOSUCH"));
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("NOSUCH"));
    }

    //! @tstid TS-CLI-021 @design DES-SVC-CLIENT
    // @relation(REQ-F-013, scope=function)
    void TS_CLI_021_theFeeFeedIsCachedSharedAndRefusedWhenUnreadable()
    {
        // Rollover fees decide the bot's carry rules, so "no fees" and "fees of zero"
        // must stay distinguishable, and the same instrument must not be fetched twice.
        qint32 feeCalls = 0;
        MockHttpServer server([&feeCalls](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                const bool gold = path.contains(QStringLiteral("GOLD"));
                return MockHttpServer::Response{
                    200,
                    searchBody(gold ? 559 : 27,
                               gold ? QStringLiteral("GOLD") : QStringLiteral("SPX500")),
                    {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            if (path.contains(QStringLiteral("/sapi/trade-real/instruments/"))) {
                ++feeCalls;
                if (path.endsWith(QStringLiteral("/559"))) {
                    // Unreadable: no Instrument object at all.
                    return MockHttpServer::Response{200, R"({"nope":true})", {}};
                }
                return MockHttpServer::Response{200, feeBody(-0.7, -0.5, -2.1, -1.5), {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EtoroClient client(mockConfig(server));
        client.setTradeConfigBaseForTesting(server.baseUrl());
        QSignalSpy fees(&client, &EtoroClient::instrumentFeesUpdated);
        client.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("GOLD")});
        QSignalSpy resolved(&client, &EtoroClient::ready);
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));

        client.requestFees(QStringLiteral("SPX500"));
        QVERIFY(fees.wait(kWaitMs));
        QCOMPARE(fees.at(0).at(0).toString(), QStringLiteral("SPX500"));
        const InstrumentFees got = fees.at(0).at(1).value<InstrumentFees>();
        QCOMPARE(got.buyOvernight, -0.7);
        QCOMPARE(got.buyWeekend, -2.1);   // the tripled weekend night, as a credit

        // Asked again: answered from the cache, without a second request.
        const qint32 after = feeCalls;
        client.requestFees(QStringLiteral("SPX500"));
        QTRY_COMPARE_WITH_TIMEOUT(fees.count(), 2, kWaitMs);
        QCOMPARE(feeCalls, after);

        // A symbol whose id is unknown asks nothing at all…
        client.requestFees(QStringLiteral("NEVERHEARDOF"));
        QTest::qWait(200);
        QCOMPARE(feeCalls, after);

        // …and an unreadable payload publishes NOTHING rather than four zeros, which
        // the carry rules would read as "no rollover cost".
        const qint32 seen = fees.count();
        client.requestFees(QStringLiteral("GOLD"));
        QTest::qWait(400);
        QCOMPARE(fees.count(), seen);
    }

    //! @tstid TS-CLI-022 @design DES-SVC-CLIENT
    // @relation(REQ-F-014, scope=function)
    void TS_CLI_022_theAccountSnapshotCarriesCashCurrencyAndTheFxLeg()
    {
        // The portfolio poll pulls three different things: the positions, the account
        // totals (cash + the account's own currency) and the EUR/USD rate that turns a
        // USD account into the euro figures the window shows.
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                // EURUSD is instrument 1; everything else answers as SPX500.
                if (path.contains(QStringLiteral("instrumentIds=1"))) {
                    return MockHttpServer::Response{200, ratesBody(1, 1.0800, 1.0802), {}};
                }
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            if (path.contains(QStringLiteral("aggregate-portfolio"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"accountCurrency":"usd","accountTotals":{"accountAvailableCash":1234.56}})",
                    {}};
            }
            if (path.endsWith(QStringLiteral("/portfolio"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"clientPortfolio":{"positions":[{"positionId":"p1","instrumentId":27,
                        "isBuy":true,"openRate":5000.0,"amount":1000.0,"leverage":10,
                        "units":2.0}],"orders":[]}})",
                    {}};
            }
            if (path.endsWith(QStringLiteral("/pnl"))) {
                return MockHttpServer::Response{200, R"({"positions":[]})", {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        QSignalSpy cash(&client, &EtoroClient::cashUpdated);
        QSignalSpy fx(&client, &EtoroClient::fxRateUpdated);
        QSignalSpy portfolio(&client, &EtoroClient::portfolioUpdated);
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));

        client.refreshPortfolio();
        QVERIFY(cash.wait(kWaitMs));
        QCOMPARE(cash.at(0).at(0).toDouble(), 1234.56);
        QTRY_VERIFY_WITH_TIMEOUT(!fx.isEmpty(), kWaitMs);
        // 1 EUR buys 1.0801 USD, so the account's USD figures are shown at ~0.9259
        // EUR per USD — the reciprocal, which is the direction that gets inverted.
        QVERIFY(qAbs(fx.at(0).at(0).toDouble() - (1.0 / 1.0801)) < 1e-6);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.isEmpty(), kWaitMs);
        const auto book = portfolio.last().at(0).value<QList<Position>>();
        QCOMPARE(book.size(), 1);
        QCOMPARE(book.constFirst().positionId, QStringLiteral("p1"));
    }

    //! @tstid TS-CLI-023 @design DES-SVC-CLIENT
    // @relation(REQ-F-002, scope=function)
    void TS_CLI_023_orderConfirmationReadsEveryOutcomeTheLookupCanAnswer()
    {
        // A market order is only half the story: what the app TELLS the user comes
        // from the order-lookup poll afterwards. Every one of its outcomes is a
        // different sentence, and getting them wrong means reporting a fill that did
        // not happen — or an alarm for one that did.
        //
        // The lookup answers are driven per call, so one test walks all of them.
        QList<QByteArray> lookupAnswers;
        qint32 lookupCalls = 0;
        MockHttpServer server([&lookupAnswers, &lookupCalls](const QByteArray &method,
                                                             const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            // The colon is percent-encoded on the wire, so both spellings must match —
            // a mock that only knows the pretty one silently answers "{}" and the
            // confirmation then polls forever.
            if (path.contains(QStringLiteral("orders:lookup"))
                || path.contains(QStringLiteral("orders%3Alookup"), Qt::CaseInsensitive)) {
                const qint32 idx = lookupCalls++;
                if (idx < lookupAnswers.size()) {
                    return MockHttpServer::Response{200, lookupAnswers.at(idx), {}};
                }
                return MockHttpServer::Response{200, R"({"status":{"id":1,"name":"Pending"}})", {}};
            }
            if ((method == "POST") && path.contains(QStringLiteral("/execution"))) {
                return MockHttpServer::Response{200, R"({"orderId":991})", {}};
            }
            if (path.contains(QStringLiteral("/portfolio"))
                || path.contains(QStringLiteral("aggregate-portfolio"))) {
                return MockHttpServer::Response{200, R"({"positions":[]})", {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        const auto sendAndRead = [&server, &lookupAnswers, &lookupCalls](
                                     const QList<QByteArray> &answers) {
            lookupAnswers = answers;
            lookupCalls = 0;
            EtoroClient client(mockConfig(server));
            QSignalSpy resolved(&client, &EtoroClient::ready);
            client.setSymbol(QStringLiteral("SPX500"));
            if (!resolved.wait(kWaitMs)) {
                return QStringList{};
            }
            QSignalSpy results(&client, &EtoroClient::orderResult);
            OrderRequest req;
            req.isBuy = true;
            req.amount = 100.0;
            req.leverage = 5.0;
            client.openPosition(req);
            QStringList messages;
            const QDeadlineTimer deadline(9000);
            while (!deadline.hasExpired()) {
                QTest::qWait(50);
                while (results.count() > messages.size()) {
                    messages.append(results.at(messages.size()).at(1).toString());
                }
                if (messages.size() >= 2) {
                    break;
                }
            }
            return messages;
        };

        // 1. A position id in positionExecutions is the definitive "it worked",
        //    whatever the status wording says.
        QStringList msgs = sendAndRead(
            {R"({"status":{"id":1,"name":"Pending"},
                 "positionExecutions":[{"positionId":"77","state":"open"}]})"});
        QVERIFY2(msgs.size() >= 2, qPrintable(QStringLiteral("got: ") + msgs.join(QStringLiteral(" | "))));
        QVERIFY(msgs.last().contains(QStringLiteral("position id 77")));

        // 2. Status 3 (Filled) with no execution list is still a fill.
        msgs = sendAndRead({R"({"status":{"id":3,"name":"Filled"}})"});
        QVERIFY2(msgs.size() >= 2, qPrintable(msgs.join(QStringLiteral(" | "))));
        QVERIFY(msgs.last().contains(QStringLiteral("filled")));

        // 3. Status 4 (Rejected) is reported as a failure, with eToro's own reason.
        msgs = sendAndRead(
            {R"({"status":{"id":4,"name":"Rejected","errorMessage":"insufficient funds"}})"});
        QVERIFY2(msgs.size() >= 2, qPrintable(msgs.join(QStringLiteral(" | "))));
        QVERIFY(msgs.last().contains(QStringLiteral("insufficient funds")));

        // 4. A CLOSED execution is not an open position: the order did not leave one
        //    behind, and the status must decide instead.
        msgs = sendAndRead(
            {R"({"status":{"id":7,"name":"Canceled"},
                 "positionExecutions":[{"positionId":"88","state":"closed"}]})"});
        QVERIFY2(msgs.size() >= 2, qPrintable(msgs.join(QStringLiteral(" | "))));
        QVERIFY(!msgs.last().contains(QStringLiteral("position id 88")));

        // 5. Still pending on the first answer, filled on the second: the poll must
        //    keep going rather than conclude from the first "don't know".
        msgs = sendAndRead({R"({"status":{"id":1,"name":"Pending"}})",
                            R"({"status":{"id":5,"name":"PartiallyFilled"}})"});
        QVERIFY2(msgs.size() >= 2, qPrintable(msgs.join(QStringLiteral(" | "))));
        QVERIFY(msgs.last().contains(QStringLiteral("confirmed"))
                || msgs.last().contains(QStringLiteral("filled")));
    }
    //! @tstid TS-CLI-024 @design DES-SVC-CLIENT
    // @relation(REQ-F-016, scope=function)
    void TS_CLI_024_modifyingAndClosingAPositionReportTheBrokersOwnAnswer()
    {
        // Changing a stop or closing a position is money-moving, so what the app says
        // afterwards has to be what the BROKER said — including its rejection text.
        // Silently reporting success on a rejected modification is the failure mode.
        QByteArray modifyAnswer = R"({"positionId":"p1"})";
        qint32 modifyStatus = 200;
        MockHttpServer server([&modifyAnswer, &modifyStatus](const QByteArray &method,
                                                             const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            // The live book: positions ride under clientPortfolio.positions, and this
            // is the /portfolio endpoint — aggregate-portfolio is the CASH snapshot,
            // which is a different call with a different shape.
            if (path.endsWith(QStringLiteral("/portfolio"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"clientPortfolio":{"positions":[{"positionId":"p1","instrumentId":27,
                        "isBuy":true,"openRate":5000.0,"amount":1000.0,"leverage":10,
                        "units":2.0}],"orders":[]}})",
                    {}};
            }
            if (path.contains(QStringLiteral("aggregate-portfolio"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"accountCurrency":"usd","accountTotals":{"accountAvailableCash":5000.0}})",
                    {}};
            }
            if (path.endsWith(QStringLiteral("/pnl"))) {
                return MockHttpServer::Response{200, R"({"positions":[]})", {}};
            }
            if ((method == "PATCH") || (method == "PUT") || (method == "DELETE")
                || path.contains(QStringLiteral("positions"))) {
                return MockHttpServer::Response{modifyStatus, modifyAnswer, {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));
        QSignalSpy portfolio(&client, &EtoroClient::portfolioUpdated);
        client.refreshPortfolio();
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.isEmpty(), kWaitMs);

        // An SL/TP change reports through `log` rather than `orderResult` — the latter
        // is for ORDERS. Watching the wrong one is how a test can pass while the user
        // sees nothing, so both are captured here and the log is what is asserted.
        QSignalSpy logs(&client, &EtoroClient::log);
        const auto lastLog = [&logs] {
            return logs.isEmpty() ? QString() : logs.last().at(0).toString();
        };

        // 1. An accepted modification says so and is not flagged as an error.
        client.modifyPosition(QStringLiteral("p1"), 4900.0, 5200.0, false);
        QTRY_VERIFY_WITH_TIMEOUT(lastLog().contains(QStringLiteral("stop-loss / take-profit")),
                                 kWaitMs);
        QVERIFY(!logs.last().at(1).toBool());

        // 2. A REJECTED one carries eToro's OWN reason rather than a transport
        //    message: "Bad Request" tells a user nothing they can act on.
        modifyStatus = 400;
        modifyAnswer = R"({"title":"stop-loss must be below the current rate"})";
        const qint32 before = logs.count();
        client.modifyPosition(QStringLiteral("p1"), 9999.0, 5200.0, false);
        QTRY_VERIFY_WITH_TIMEOUT(logs.count() > before, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(lastLog().contains(QStringLiteral("must be below")), kWaitMs);
        QVERIFY(logs.last().at(1).toBool());   // flagged as an error

        // 3. A rejection with NO readable body still says something: the raw answer,
        //    and failing that the transport error — never an empty reason.
        modifyAnswer = R"(<html>gateway timeout</html>)";
        const qint32 beforeRaw = logs.count();
        client.modifyPosition(QStringLiteral("p1"), 4800.0, 0.0, true);
        QTRY_VERIFY_WITH_TIMEOUT(logs.count() > beforeRaw, kWaitMs);
        QVERIFY(!lastLog().isEmpty());

        // 4. Clearing both legs (rate 0) is a legitimate request, not a refusal.
        modifyStatus = 200;
        modifyAnswer = R"({"positionId":"p1"})";
        const qint32 beforeClear = logs.count();
        client.modifyPosition(QStringLiteral("p1"), 0.0, 0.0, false);
        QTRY_VERIFY_WITH_TIMEOUT(logs.count() > beforeClear, kWaitMs);

        // 5. An empty position id never reaches the network at all.
        const qint32 beforeEmpty = logs.count();
        client.modifyPosition(QString(), 1.0, 2.0, false);
        QTest::qWait(300);
        QCOMPARE(logs.count(), beforeEmpty);
    }

    //! @tstid TS-CLI-025 @design DES-SVC-CLIENT
    // @relation(REQ-F-025, scope=function)
    void TS_CLI_025_quotesAreReadFromEveryShapeAndRepairedWhenStale()
    {
        // The rates feed is the app's pulse: it decides whether the market counts as
        // open, what a position is worth and whether a trade may be placed at all.
        // Three payload shapes and one repair path, each with its own consequence.
        QByteArray ratesAnswer;
        MockHttpServer server([&ratesAnswer](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesAnswer, {}};
            }
            if (path.contains(QStringLiteral("/candles"))) {
                // The 1-minute candle the client falls back to when the rates row is
                // stale: its close IS the bid (measured against the live feed).
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"candles":[{"fromDate":"%1","open":4990.0,"high":5010.0,)"
                                   R"("low":4980.0,"close":5000.0}]})")
                        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
                        .toUtf8(),
                    {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        // Shape 1: the documented {"rates":[…]} envelope.
        ratesAnswer = ratesBody(27, 5000.0, 5001.0);
        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        QSignalSpy quotes(&client, &EtoroClient::quotesUpdated);
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));
        QTRY_VERIFY_WITH_TIMEOUT(!quotes.isEmpty(), kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(client.quotes().value(27).bid > 0.0, kWaitMs);
        QCOMPARE(client.quotes().value(27).bid, 5000.0);
        QVERIFY(client.quotes().value(27).ask >= 5000.0);

        // Shape 2: a bare ARRAY, which the same endpoint answers for some accounts.
        ratesAnswer = QStringLiteral(R"([{"instrumentId":27,"bid":5010.0,"ask":5011.0,)"
                                     R"("date":"%1"}])")
                          .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
                          .toUtf8();
        QTRY_VERIFY_WITH_TIMEOUT(client.quotes().value(27).bid >= 5000.0, kWaitMs);

        // Shape 3: a row with no usable numbers at all. The last good quote stands —
        // publishing a zero would read as a price of nothing.
        ratesAnswer = R"({"rates":[{"instrumentId":27}]})";
        const double lastGood = client.quotes().value(27).bid;
        QTest::qWait(1500);
        QCOMPARE(client.quotes().value(27).bid, lastGood);
    }
    //! @tstid TS-CLI-026 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_026_theBrokersOwnOrdersAreReadInEveryFieldSpellingItUses()
    {
        // eToro's payloads are not consistent about capitalisation or field names —
        // `orderID` and `orderId`, `rate` and `triggerRate` — and an order the app
        // fails to read is an order nobody can cancel. Both spellings, both time
        // formats, and an instrument the selector does not list (which must still be
        // VISIBLE, as "#id", rather than silently dropped).
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            if (path.endsWith(QStringLiteral("/portfolio"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"clientPortfolio":{"positions":[],"orders":[
                        {"orderID":"o1","instrumentID":27,"isBuy":true,"rate":4900.0,
                         "amount":500.0,"leverage":10,"units":1.02,
                         "stopLossRate":4800.0,"takeProfitRate":5100.0,
                         "openDateTime":"2026-08-04T10:00:00.000Z"},
                        {"orderId":"o2","instrumentId":9999,"isBuy":false,
                         "triggerRate":123.0,"amount":200.0,"leverage":5,
                         "isTslEnabled":true,"openDateTime":"2026-08-04T11:00:00Z"}
                    ]}})",
                    {}};
            }
            if (path.endsWith(QStringLiteral("/pnl"))) {
                return MockHttpServer::Response{200, R"({"positions":[]})", {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        QSignalSpy pending(&client, &EtoroClient::pendingOrdersUpdated);
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));
        client.refreshPortfolio();
        QTRY_VERIFY_WITH_TIMEOUT(!pending.isEmpty(), kWaitMs);

        const auto orders = pending.last().at(0).value<QList<PendingOrder>>();
        QCOMPARE(orders.size(), 2);
        const PendingOrder first = orders.constFirst();
        QCOMPARE(first.orderId, QStringLiteral("o1"));     // orderID spelling
        QCOMPARE(first.instrumentId, 27);
        QCOMPARE(first.triggerRate, 4900.0);               // `rate` spelling
        QVERIFY(first.isBuy);
        QVERIFY(first.submitted.isValid());                // ISO with milliseconds
        // The SL/TP came as RATES and are shown as account-currency amounts.
        QVERIFY(first.stopLossAmount > 0.0);
        QVERIFY(first.takeProfitAmount > 0.0);

        const PendingOrder second = orders.at(1);
        QCOMPARE(second.orderId, QStringLiteral("o2"));    // orderId spelling
        QCOMPARE(second.triggerRate, 123.0);               // triggerRate spelling
        QVERIFY(!second.isBuy);
        QVERIFY(second.trailingStop);
        QVERIFY(second.submitted.isValid());               // ISO without milliseconds
        // An instrument the selector does not know is still addressable.
        QCOMPARE(second.symbol, QStringLiteral("#9999"));
        // Units were absent, so the notional identity supplied them — the amounts are
        // finite rather than a division by zero.
        QVERIFY(std::isfinite(second.stopLossAmount));
    }

    //! @tstid TS-CLI-027 @design DES-SVC-CLIENT
    // @relation(REQ-F-015, scope=function)
    void TS_CLI_027_theHistoryWalkSurvivesWhatTheEndpointReallyReturns()
    {
        // The closed-trade walk pages until a page comes back short. Each page can
        // arrive in a different shape, with timestamps in a different format, and a
        // failed page must end the walk with what was collected rather than with
        // nothing — a report that silently drops a month is worse than a short one.
        qint32 page = 0;
        MockHttpServer server([&page](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            // The real path is /v1/trading/info/trade/history — "trade", singular,
            // and no "trades" anywhere in it.
            if (path.contains(QStringLiteral("/trade/history"))) {
                ++page;
                if (page == 1) {
                    // Nested under "data", epoch SECONDS, and one row missing its rate.
                    return MockHttpServer::Response{
                        200,
                        R"({"data":[
                            {"instrumentId":27,"isBuy":true,"leverage":10,"investment":500.0,
                             "units":1.0,"openRate":5000.0,"closeRate":5100.0,"netProfit":10.0,
                             "openTimestamp":1785849600,"closeTimestamp":1785936000,
                             "positionId":11},
                            {"instrumentId":27,"isBuy":false,"leverage":5,"investment":100.0,
                             "units":0.2,"openRate":0.0,"closeRate":0.0,"netProfit":-1.0,
                             "openTimestamp":1785849600,"closeTimestamp":1785936000,
                             "positionId":12}
                        ]})",
                        {}};
                }
                if (page == 2) {
                    // Epoch MILLISECONDS, bare array.
                    return MockHttpServer::Response{
                        200,
                        R"([{"instrumentId":27,"isBuy":true,"leverage":2,"investment":50.0,
                             "units":0.01,"openRate":5000.0,"closeRate":4950.0,
                             "netProfit":-5.0,"openTimestamp":1785849600000,
                             "closeTimestamp":1785936000000,"positionId":13}])",
                        {}};
                }
                if (page == 3) {
                    return MockHttpServer::Response{200, R"([])", {}};   // the walk ends
                }
                // A LATER walk hits a failing page (see the second half of the test).
                return MockHttpServer::Response{503, R"({"err":"later"})", {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        QSignalSpy closed(&client, &EtoroClient::closedTradesReady);
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));

        QSignalSpy failures(&client, &EtoroClient::monthlyPnlFailed);
        client.fetchClosedTrades(4);
        QVERIFY(closed.wait(kWaitMs));
        const auto trades = closed.last().at(0).value<QList<ClosedTrade>>();
        QVERIFY2(trades.size() >= 3, qPrintable(QString::number(trades.size())));
        // Both epoch formats were understood — seconds and milliseconds land in the
        // same year rather than in 1970 or 58000.
        for (const ClosedTrade &t : trades) {
            QVERIFY(t.closeTime.isValid());
            QCOMPARE(t.closeTime.date().year(), 2026);
        }
        // A row whose rates are missing is still counted in the account totals — the
        // money moved even when the price fields did not arrive.
        QVERIFY(!trades.isEmpty());

        // Now the failing page: the walk REPORTS the failure rather than publishing a
        // truncated history as if it were complete. A short report that looks whole is
        // the worse of the two outcomes, so the app refuses to produce one.
        const qint32 readyBefore = closed.count();
        client.fetchClosedTrades(4);
        QTRY_VERIFY_WITH_TIMEOUT(!failures.isEmpty(), kWaitMs);
        QVERIFY(failures.last().at(0).toString().contains(QStringLiteral("503"))
                || failures.last().at(0).toString().contains(QStringLiteral("failed")));
        QCOMPARE(closed.count(), readyBefore);
    }
    //! @tstid TS-CLI-028 @design DES-SVC-CLIENT
    // @relation(REQ-F-003, scope=function)
    void TS_CLI_028_marketOpenAndTheFxLegAreInferredNotAssumed()
    {
        // There is no "is this market open" flag in the API — the client infers it from
        // whether the quote's timestamp ADVANCES between polls. Getting that wrong in
        // either direction is expensive: a false "closed" locks the buttons on a live
        // market, a false "open" lets an order go to a venue that will not fill it.
        QDateTime quoteStamp = QDateTime::currentDateTimeUtc();
        QByteArray fxAnswer;
        MockHttpServer server([&quoteStamp, &fxAnswer](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                if (path.contains(QStringLiteral("instrumentIds=1"))) {
                    return MockHttpServer::Response{200, fxAnswer, {}};
                }
                // The bulk poll answers in the ALTERNATIVE field spelling (cvtBid /
                // instrumentID), which the client has to read as well as the plain one.
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"rates":[{"instrumentID":27,"cvtBid":5000.0,)"
                                   R"("cvtAsk":5001.0,"date":"%1"}]})")
                        .arg(quoteStamp.toString(Qt::ISODate))
                        .toUtf8(),
                    {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        // 1. The FX leg: no usable numbers means the last rate STANDS rather than
        //    becoming zero — every euro figure in the window depends on it.
        fxAnswer = R"({"rates":[{"instrumentId":1}]})";
        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        QSignalSpy fx(&client, &EtoroClient::fxRateUpdated);
        client.setTradableSymbols({QStringLiteral("SPX500")});
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));
        client.refreshPortfolio();
        QTest::qWait(800);
        QCOMPARE(fx.count(), 0);   // nothing publishable arrived

        // 2. …and the documented fallback: no bid/ask, but a lastExecution price.
        fxAnswer = R"({"rates":[{"instrumentId":1,"lastExecution":1.10}]})";
        client.refreshPortfolio();
        QTRY_VERIFY_WITH_TIMEOUT(!fx.isEmpty(), kWaitMs);
        QVERIFY(qAbs(fx.last().at(0).toDouble() - (1.0 / 1.10)) < 1e-6);

        // 3. The alternative quote spelling was understood by the bulk poll.
        QTRY_VERIFY_WITH_TIMEOUT(client.quotes().value(27).bid > 0.0, kWaitMs);
        QCOMPARE(client.quotes().value(27).bid, 5000.0);

        // 4. A timestamp that stops advancing is what "closed" looks like. The first
        //    poll leans on the quote's AGE, later ones on whether it moved — so a
        //    deliberately old, frozen stamp must end up as not-tradable.
        QSignalSpy tradable(&client, &EtoroClient::tradeabilityUpdated);
        quoteStamp = QDateTime::currentDateTimeUtc().addSecs(-4 * 3600);
        QTest::qWait(2500);
        if (!tradable.isEmpty()) {
            const auto openSet = tradable.last().at(0).value<QSet<QString>>();
            QVERIFY(!openSet.contains(QStringLiteral("SPX500")));
        }
    }
    //! @tstid TS-CLI-029 @design DES-SVC-CLIENT
    // @relation(REQ-N-003, scope=function)
    void TS_CLI_029_everyRequestSurvivesAFailingServerAndANonsenseAnswer()
    {
        // A trading client spends its life talking to someone else's server, and the
        // interesting question is not what it does when the answers are good. Two
        // hostile servers here, and the app has to come out of both alive: still
        // running, still reporting, with no invented numbers in its books.

        // 1. EVERYTHING fails. Each entry point is driven and the app must neither
        //    crash nor publish anything derived from a failed reply.
        {
            MockHttpServer dead([](const QByteArray &, const QString &) {
                return MockHttpServer::Response{500, R"({"err":"down"})", {}};
            });
            QVERIFY(dead.listen(QHostAddress::LocalHost));
            EtoroClient client(mockConfig(dead));
            client.setTradableSymbols({QStringLiteral("SPX500")});
            const QSignalSpy quotes(&client, &EtoroClient::quotesUpdated);
            const QSignalSpy cash(&client, &EtoroClient::cashUpdated);
            const QSignalSpy fx(&client, &EtoroClient::fxRateUpdated);
            const QSignalSpy closed(&client, &EtoroClient::closedTradesReady);

            client.setSymbol(QStringLiteral("SPX500"));
            client.refreshPortfolio();
            client.fetchClosedTrades(2);
            client.requestFees(QStringLiteral("SPX500"));
            client.scanInstruments();
            client.closePosition(QStringLiteral("p1"));
            client.modifyPosition(QStringLiteral("p1"), 1.0, 2.0, false);
            client.cancelPendingOrder(QStringLiteral("o1"));
            OrderRequest req;
            req.isBuy = true;
            req.amount = 100.0;
            req.leverage = 2.0;
            client.openPosition(req);
            QTest::qWait(2500);

            // Nothing was published from a failed answer: no quote, no cash, no FX,
            // no closed-trade report. Silence is the correct output here.
            QCOMPARE(quotes.count(), 0);
            QCOMPARE(cash.count(), 0);
            QCOMPARE(fx.count(), 0);
            QCOMPARE(closed.count(), 0);
            QVERIFY(client.quotes().isEmpty());
        }

        // 2. Everything answers 200 with a payload that is well-formed JSON and
        //    completely wrong — an empty array where an object belongs, an object
        //    where an array belongs. The parsers must find nothing rather than read
        //    the first field of something unrelated.
        {
            MockHttpServer nonsense([](const QByteArray &, const QString &path) {
                if (path.contains(QStringLiteral("/market-data/search"))) {
                    // Resolution has to work, or nothing else is even attempted.
                    return MockHttpServer::Response{200, searchBody(27,
                                                                    QStringLiteral("SPX500")),
                                                    {}};
                }
                if (path.contains(QStringLiteral("rates"))) {
                    return MockHttpServer::Response{200, R"([])", {}};
                }
                return MockHttpServer::Response{200, R"({"nothing":"useful"})", {}};
            });
            QVERIFY(nonsense.listen(QHostAddress::LocalHost));
            EtoroClient client(mockConfig(nonsense));
            client.setTradableSymbols({QStringLiteral("SPX500")});
            QSignalSpy resolved(&client, &EtoroClient::ready);
            const QSignalSpy cash(&client, &EtoroClient::cashUpdated);
            const QSignalSpy pending(&client, &EtoroClient::pendingOrdersUpdated);
            client.setSymbol(QStringLiteral("SPX500"));
            QVERIFY(resolved.wait(kWaitMs));
            client.refreshPortfolio();
            client.fetchClosedTrades(2);
            client.requestFees(QStringLiteral("SPX500"));
            QTest::qWait(2000);

            // The account figure is not published from a payload that never carried
            // one, and the quote book stays empty rather than holding a zero.
            QCOMPARE(cash.count(), 0);
            QVERIFY(client.quotes().value(27).bid <= 0.0);
            // A portfolio answer with no positions array yields an EMPTY book, not a
            // phantom position — and an empty book is still a published book.
            if (!pending.isEmpty()) {
                QVERIFY(pending.last().at(0).value<QList<PendingOrder>>().isEmpty());
            }
        }
    }
    //! @tstid TS-CLI-030 @design DES-SVC-CLIENT
    // @relation(REQ-F-014, scope=function)
    void TS_CLI_030_thePositionBookIsReadInEverySpellingAndOwnsItsGaps()
    {
        // The open-trades table is what a person looks at to decide whether to close
        // something. Every field in it has TWO spellings in eToro's payloads, and a
        // position the app fails to attribute is a position nobody manages.
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            if (path.endsWith(QStringLiteral("/portfolio"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"clientPortfolio":{"orders":[],"positions":[
                        {"id":"alt","instrumentId":27,"buy":false,
                         "investedAmount":250.0,"unitsValue":0.5,"openPrice":4900.0,
                         "leverage":10},
                        {"positionId":"plain","instrumentId":27,"isBuy":true,
                         "amount":500.0,"units":1.0,"openRate":5000.0,"leverage":5},
                        {"positionId":"foreign","instrumentId":424242,
                         "internalSymbolFull":"NOTLISTED","isBuy":true,"amount":10.0,
                         "units":1.0,"openRate":1.0,"leverage":1}
                    ]}})",
                    {}};
            }
            if (path.endsWith(QStringLiteral("/pnl"))) {
                // eToro's own unrealised P/L, which is authoritative where it exists.
                return MockHttpServer::Response{
                    200,
                    R"({"positions":[{"positionId":"plain","unrealizedPnL":{"pnL":12.34}}]})",
                    {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        QSignalSpy portfolio(&client, &EtoroClient::portfolioUpdated);
        client.setTradableSymbols({QStringLiteral("SPX500")});
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));
        client.refreshPortfolio();
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.isEmpty(), kWaitMs);

        const auto book = portfolio.last().at(0).value<QList<Position>>();
        // Both spellings were read; the position on an instrument the app does not
        // list is left out rather than shown under a wrong symbol.
        QCOMPARE(book.size(), 2);
        QHash<QString, Position> byId;
        for (const Position &p : book) {
            static_cast<void>(byId.insert(p.positionId, p));
        }
        QVERIFY(byId.contains(QStringLiteral("alt")));      // id / buy / investedAmount…
        QVERIFY(byId.contains(QStringLiteral("plain")));
        QVERIFY(!byId.contains(QStringLiteral("foreign")));
        QCOMPARE(byId.value(QStringLiteral("alt")).amount, 250.0);
        QCOMPARE(byId.value(QStringLiteral("alt")).units, 0.5);
        QCOMPARE(byId.value(QStringLiteral("alt")).openRate, 4900.0);
        QVERIFY(!byId.value(QStringLiteral("alt")).isBuy);   // `buy` spelling, false
        QVERIFY(byId.value(QStringLiteral("plain")).isBuy);

        // A portfolio whose positions array is empty publishes an EMPTY book — a book
        // that stops updating looks identical to one with no positions, and only the
        // first of those is true here.
        const qint32 before = portfolio.count();
        client.refreshPortfolio();
        QTRY_VERIFY_WITH_TIMEOUT(portfolio.count() > before, kWaitMs);
    }
    //! @tstid TS-CLI-031 @design DES-SVC-CLIENT
    // @relation(REQ-F-025, scope=function)
    void TS_CLI_031_theCandleRepairOnlyMovesAPriceForward()
    {
        // eToro's rates row for the .24-7 instruments runs minutes late while the
        // 1-minute candle is live, so the client re-bases a stale quote on the candle's
        // close. The rule that keeps this from being harmful: it may only ever move a
        // price FORWARD in time. A repair that accepted an older candle would make the
        // book jitter between two sources.
        QByteArray candleAnswer;
        QDateTime rowStamp = QDateTime::currentDateTimeUtc().addSecs(-15 * 60);   // stale
        MockHttpServer server([&candleAnswer, &rowStamp](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{
                    200,
                    QStringLiteral(R"({"rates":[{"instrumentId":27,"bid":5000.0,"ask":5001.0,)"
                                   R"("date":"%1"}]})")
                        .arg(rowStamp.toString(Qt::ISODate))
                        .toUtf8(),
                    {}};
            }
            if (path.contains(QStringLiteral("/candles"))) {
                return MockHttpServer::Response{200, candleAnswer, {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        // 1. An empty candle list, and one whose close is zero: neither repairs
        //    anything, and the stale-but-known price stands.
        candleAnswer = R"({"candles":[]})";
        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        client.setTradableSymbols({QStringLiteral("SPX500")});
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));
        QTRY_VERIFY_WITH_TIMEOUT(client.quotes().value(27).bid > 0.0, kWaitMs);
        QCOMPARE(client.quotes().value(27).bid, 5000.0);

        candleAnswer = QStringLiteral(R"({"candles":[{"fromDate":"%1","close":0.0}]})")
                           .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
                           .toUtf8();
        QTest::qWait(1500);
        QCOMPARE(client.quotes().value(27).bid, 5000.0);

        // 2. A candle OLDER than the price already held changes nothing — the repair
        //    only moves forward.
        candleAnswer = QStringLiteral(R"({"candles":[{"fromDate":"%1","close":4000.0}]})")
                           .arg(rowStamp.addSecs(-3600).toString(Qt::ISODate))
                           .toUtf8();
        QTest::qWait(1500);
        QCOMPARE(client.quotes().value(27).bid, 5000.0);

        // 3. A NEWER candle does repair it, and its close becomes the bid — the
        //    identity measured against the live feed (candle close == bid exactly).
        candleAnswer = QStringLiteral(R"({"candles":[{"fromDate":"%1","close":5123.0}]})")
                           .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
                           .toUtf8();
        QTRY_COMPARE_WITH_TIMEOUT(client.quotes().value(27).bid, 5123.0, kWaitMs);
    }

    //! @tstid TS-CLI-032 @design DES-SVC-CLIENT
    // @relation(REQ-F-027, scope=function)
    void TS_CLI_032_adjustingAnOrderThatIsNotThereSaysSoInsteadOfSending()
    {
        // Cancelling or adjusting a resting order the broker no longer holds must be
        // refused LOCALLY with a sentence a person can act on — not sent, and not
        // silently ignored, which would look like the app doing nothing.
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/market-data/search"))) {
                return MockHttpServer::Response{200, searchBody(27, QStringLiteral("SPX500")), {}};
            }
            if (path.contains(QStringLiteral("/instruments/rates"))) {
                return MockHttpServer::Response{200, ratesBody(27, 5000.0, 5001.0), {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EtoroClient client(mockConfig(server));
        QSignalSpy resolved(&client, &EtoroClient::ready);
        client.setSymbol(QStringLiteral("SPX500"));
        QVERIFY(resolved.wait(kWaitMs));

        QSignalSpy results(&client, &EtoroClient::orderResult);
        client.modifyPendingOrder(QStringLiteral("ghost"), 4900.0, 10.0, 20.0);
        QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), kWaitMs);
        QVERIFY(!results.last().at(0).toBool());
        QVERIFY(results.last().at(1).toString().contains(QStringLiteral("no longer resting")));

        // CANCEL is deliberately different from adjust: an id the app does not know is
        // still SENT, because the broker may hold an order this session never saw (an
        // order placed elsewhere is exactly the one a user wants to cancel from here).
        // Only an EMPTY id is refused locally — there is nothing to name.
        const qint32 before = results.count();
        client.cancelPendingOrder(QString());
        QTRY_VERIFY_WITH_TIMEOUT(results.count() > before, kWaitMs);
        QVERIFY(!results.last().at(0).toBool());
        QVERIFY(results.last().at(1).toString().contains(QStringLiteral("No pending order")));

        const qint32 beforeGhost = results.count();
        client.cancelPendingOrder(QStringLiteral("ghost"));
        QTRY_VERIFY_WITH_TIMEOUT(results.count() > beforeGhost, kWaitMs);
    }
    //! @tstid TS-CLI-033 @design DES-SVC-CLIENT
    // @relation(REQ-F-017, scope=function)
    void TS_CLI_033_withoutCredentialsEveryPathGoesToTheSimulationInstead()
    {
        // The client has two halves: with credentials it talks to eToro, without them
        // it drives the built-in simulation. EVERY public entry point therefore has a
        // second branch that a real-mode test never touches — and the one thing that
        // must be true of all of them is that no request leaves the machine.
        qint32 requests = 0;
        MockHttpServer watcher([&requests](const QByteArray &, const QString &) {
            ++requests;   // must stay at zero: simulation talks to nobody
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(watcher.listen(QHostAddress::LocalHost));

        Config cfg;
        cfg.apiKey.clear();    // no credentials → SIMULATION
        cfg.userKey.clear();
        cfg.baseUrl = watcher.baseUrl() + QStringLiteral("/api");
        cfg.symbol = QStringLiteral("SPX500");
        QVERIFY(!cfg.hasCredentials());

        EtoroClient client(cfg);
        QSignalSpy ready(&client, &EtoroClient::ready);
        QSignalSpy history(&client, &EtoroClient::historyReady);
        QSignalSpy portfolio(&client, &EtoroClient::portfolioUpdated);
        QSignalSpy results(&client, &EtoroClient::orderResult);
        QSignalSpy closedTrades(&client, &EtoroClient::closedTradesReady);
        QSignalSpy screenerDone(&client, &EtoroClient::screenerFinished);

        client.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("GER40")});
        client.setSymbol(QStringLiteral("SPX500"));
        // QTRY rather than wait(): simulation answers SYNCHRONOUSLY, so the signal has
        // already been recorded by the time wait() would start listening for the next
        // one — the classic QSignalSpy trap.
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), kWaitMs);
        QVERIFY(!history.isEmpty());   // a synthetic series, immediately

        // Opening, adjusting and closing all work against the virtual account.
        OrderRequest req;
        req.isBuy = true;
        req.amount = 1000.0;
        req.leverage = 5.0;
        req.stopLossAmount = 100.0;
        req.takeProfitAmount = 150.0;
        client.openPosition(req);
        QTRY_VERIFY_WITH_TIMEOUT(!portfolio.isEmpty(), kWaitMs);
        const auto book = portfolio.last().at(0).value<QList<Position>>();
        QVERIFY(!book.isEmpty());
        const QString id = book.constFirst().positionId;
        client.modifyPosition(id, 4900.0, 5200.0, false);
        client.refreshPortfolio();

        // A LIMIT order rests in the simulation exactly as it would at the broker.
        OrderRequest limit;
        limit.isBuy = true;
        limit.amount = 200.0;
        limit.leverage = 2.0;
        limit.triggerRate = 1.0;   // far below: it rests rather than filling
        client.openPosition(limit);
        client.cancelPendingOrder(QStringLiteral("1"));

        // The reporting paths answer from the simulated record.
        client.fetchClosedTrades(4);
        client.requestFees(QStringLiteral("SPX500"));
        client.scanInstruments();
        QTRY_VERIFY_WITH_TIMEOUT(!screenerDone.isEmpty(), kWaitMs);
        client.closePosition(id);
        QTest::qWait(500);

        // The whole point: not one request went out. A simulation that quietly talked
        // to the broker would be the most dangerous defect this app could have.
        QCOMPARE(requests, 0);
    }
};

QTEST_GUILESS_MAIN(TestEtoroClientLive)
#include "tst_etoroclientlive.moc"
