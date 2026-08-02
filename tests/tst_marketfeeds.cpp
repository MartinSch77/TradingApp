// Integration tests for the public market feeds (DES-SVC-FEEDS) against an
// in-process mock HTTP server: the VIX reading vs its multi-month baseline,
// the TradingView rating (current instrument and bulk with ticker->symbols
// fan-out), news headlines, the CNN Fear & Greed index, the Yahoo reference
// quote with its exchange timestamp, the intraday close series and the
// throttled feed-error log. Every feed host is redirected to the mock via
// setEndpointBaseForTesting() — no test touches the real network.

#include "MockHttpServer.h"
#include "domain/DecisionEngine.h"
#include "services/MarketFeeds.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <algorithm>

#include <cmath>

namespace {

// Generous shared bound for spy waits: the mock answers in milliseconds, the
// margin only absorbs CI load.
constexpr qint32 kWaitMs = 15000;

// Yahoo v8 chart payload: meta fields + a close series. Raw JSON snippets so
// nulls can appear mid-array, exactly as the live feed sends holidays / empty
// minutes.
QByteArray yahooChartBody(const QByteArray &meta, const QByteArray &closes)
{
    return R"({"chart":{"result":[{"meta":{)" + meta + R"(},)"
           R"("indicators":{"quote":[{"close":[)" + closes + R"(]}]}}]}})";
}

// TradingView scanner payload: one rated ticker with its "d" column values.
QByteArray scanBody(const QByteArray &ticker, const QByteArray &columns)
{
    return R"({"data":[{"s":")" + ticker + R"(","d":[)" + columns + R"(]}]})";
}

// How many recorded requests hit a path containing `fragment`.
qint32 requestCount(const MockHttpServer &server, const QString &fragment)
{
    const QList<MockHttpServer::Recorded> requests = server.requests();
    return static_cast<qint32>(
        std::count_if(requests.cbegin(), requests.cend(),
                      [&fragment](const MockHttpServer::Recorded &r) {
                          return r.path.contains(fragment);
                      }));
}

// How many log-signal emissions start with `prefix` (per-feed error lines).
qint32 logCount(const QSignalSpy &logs, const QString &prefix)
{
    qint32 hits = 0;
    for (qint32 i = 0; i < logs.count(); ++i) {
        if (logs.at(i).at(0).toString().startsWith(prefix)) {
            ++hits;
        }
    }
    return hits;
}

} // namespace

class TestMarketFeeds : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-FEED-001 @design DES-SVC-FEEDS
    // @relation(REQ-F-020, scope=function)
    void TS_FEED_001_vixLevelAgainstBaseline()
    {
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("VIX"))) {
                return MockHttpServer::Response{
                    200,
                    yahooChartBody(R"("regularMarketPrice":25.0)",
                                   "10.0,null,20.0,0.0,30.0,10.0,30.0"),
                    {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        QSignalSpy vix(&feeds, &MarketFeeds::vixUpdated);
        feeds.start(60000);
        QVERIFY(vix.wait(kWaitMs));
        // The null and the non-positive close are dropped before averaging, so
        // the baseline is (10+20+30+10+30)/5 = 20 and 25 reads +25% against it.
        QCOMPARE(vix.at(0).at(0).toDouble(), 25.0);
        QCOMPARE(vix.at(0).at(1).toDouble(), 25.0);
    }

    //! @tstid TS-FEED-002 @design DES-SVC-FEEDS
    // @relation(REQ-F-020, scope=function)
    void TS_FEED_002_externalSignalForCurrentSymbol()
    {
        MockHttpServer server([](const QByteArray &method, const QString &path) {
            if ((method == "POST") && path.contains(QStringLiteral("/global/scan"))) {
                return MockHttpServer::Response{200, scanBody("SP:SPX", "0.35"), {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        feeds.setCurrentSymbol(QStringLiteral("SPX500"));
        QSignalSpy rating(&feeds, &MarketFeeds::externalSignalUpdated);
        feeds.start(60000);
        QVERIFY(rating.wait(kWaitMs));
        QVERIFY(rating.at(0).at(0).toBool());
        QCOMPARE(rating.at(0).at(1).toDouble(), 0.35);
        QCOMPARE(rating.at(0).at(2).toString(), trading::webRatingWord(0.35));
        // The scan POST asked for exactly this instrument's ticker on the 1h column.
        const QJsonObject body =
            QJsonDocument::fromJson(server.lastBodyFor(QStringLiteral("/global/scan"))).object();
        const QJsonArray tickers = body.value(QStringLiteral("symbols"))
                                       .toObject()
                                       .value(QStringLiteral("tickers"))
                                       .toArray();
        QCOMPARE(tickers, QJsonArray{QStringLiteral("SP:SPX")});
        QCOMPARE(body.value(QStringLiteral("columns")).toArray(),
                 QJsonArray{QStringLiteral("Recommend.All|60")});
    }

    //! @tstid TS-FEED-003 @design DES-SVC-FEEDS
    // @relation(REQ-F-020, scope=function)
    void TS_FEED_003_staleReplyForSwitchedInstrumentDropped()
    {
        // The SPX500 scan reply is held back; by the time it arrives the
        // current instrument is EURUSD, so it must be ignored ("the instrument
        // changed under us"). The mock recognises the two requests by the
        // ticker in the just-recorded body.
        MockHttpServer *srv = nullptr;
        MockHttpServer server([&srv](const QByteArray &method, const QString &path) {
            if ((method == "POST") && path.contains(QStringLiteral("/global/scan"))) {
                if (srv->lastBodyFor(QStringLiteral("/global/scan")).contains("SP:SPX")) {
                    return MockHttpServer::Response{200, scanBody("SP:SPX", "0.9"), {}, 400};
                }
                return MockHttpServer::Response{200, scanBody("FX:EURUSD", "0.2"), {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        srv = &server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        feeds.setCurrentSymbol(QStringLiteral("SPX500"));
        QSignalSpy rating(&feeds, &MarketFeeds::externalSignalUpdated);
        feeds.start(60000);                                 // SPX500 fetch, reply held back
        feeds.setCurrentSymbol(QStringLiteral("EURUSD"));   // switch mid-flight, prompt re-fetch
        QVERIFY(rating.wait(kWaitMs));
        QVERIFY(rating.at(0).at(0).toBool());
        QCOMPARE(rating.at(0).at(1).toDouble(), 0.2);       // the EURUSD reading
        QTest::qWait(700);                                  // let the held-back SPX500 reply land
        QCOMPARE(rating.count(), 1);                        // ... and be dropped, not published
    }

    //! @tstid TS-FEED-004 @design DES-SVC-FEEDS
    // @relation(REQ-F-020, scope=function)
    void TS_FEED_004_bulkRatingsFanOutToSharedSymbols()
    {
        MockHttpServer server([](const QByteArray &method, const QString &path) {
            if ((method == "POST") && path.contains(QStringLiteral("/global/scan"))) {
                return MockHttpServer::Response{200, scanBody("SP:SPX", "0.1,0.2,null"), {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        // SPX500 and SP.24-7 share the SP:SPX ticker; RUBBER has no web ticker.
        feeds.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("SP.24-7"),
                                  QStringLiteral("RUBBER")});
        bool received = false;
        QHash<QString, WebRating> ratings;
        static_cast<void>(connect(&feeds, &MarketFeeds::instrumentRatingsUpdated, this,
                                  [&received, &ratings](const QHash<QString, WebRating> &r) {
                                      ratings = r;
                                      received = true;
                                  }));
        feeds.fetchInstrumentRatings();
        QTRY_VERIFY_WITH_TIMEOUT(received, kWaitMs);
        // One rated ticker fans out to both app symbols; RUBBER is omitted.
        QCOMPARE(ratings.size(), 2);
        QVERIFY(ratings.contains(QStringLiteral("SPX500")));
        QVERIFY(ratings.contains(QStringLiteral("SP.24-7")));
        const WebRating r = ratings.value(QStringLiteral("SPX500"));
        QCOMPARE(r.m15, 0.1);
        QCOMPARE(r.h1, 0.2);
        QVERIFY(std::isnan(r.d1));  // null column = timeframe unavailable
        // The request carried the deduplicated ticker once, with all three timeframes.
        const QJsonObject body =
            QJsonDocument::fromJson(server.lastBodyFor(QStringLiteral("/global/scan"))).object();
        QCOMPARE(body.value(QStringLiteral("symbols"))
                     .toObject()
                     .value(QStringLiteral("tickers"))
                     .toArray(),
                 QJsonArray{QStringLiteral("SP:SPX")});
        QCOMPARE(body.value(QStringLiteral("columns")).toArray().size(), 3);
    }

    //! @tstid TS-FEED-005 @design DES-SVC-FEEDS
    // @relation(REQ-F-020, scope=function)
    void TS_FEED_005_newsHeadlinesPerSymbol()
    {
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("/public/view/v1/symbol"))) {
                return MockHttpServer::Response{
                    200,
                    R"({"items":[
                        {"title":"","provider":{"name":"NoTitle"}},
                        {"title":"A","provider":{"name":"Reuters"},"published":1753900000},
                        {"title":"B"},{"title":"C"},{"title":"D"},{"title":"E"},{"title":"F"}
                    ]})",
                    {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        feeds.setTradableSymbols({QStringLiteral("SPX500")});
        bool received = false;
        QString symbol;
        QList<NewsHeadline> headlines;
        static_cast<void>(connect(
            &feeds, &MarketFeeds::instrumentNewsUpdated, this,
            [&received, &symbol, &headlines](const QString &sym, const QList<NewsHeadline> &h) {
                symbol = sym;
                headlines = h;
                received = true;
            }));
        feeds.fetchInstrumentNews();
        QTRY_VERIFY_WITH_TIMEOUT(received, kWaitMs);
        QCOMPARE(symbol, QStringLiteral("SPX500"));
        QCOMPARE(headlines.size(), 5);  // untitled item skipped, capped at five
        QCOMPARE(headlines.first().title, QStringLiteral("A"));
        QCOMPARE(headlines.first().provider, QStringLiteral("Reuters"));
        QCOMPARE(headlines.first().published, QDateTime::fromSecsSinceEpoch(1753900000));
        // The query filter carried the ticker with its ':' unencoded, as the feed expects.
        QVERIFY(server.requests().last().path.contains(QStringLiteral("filter=symbol:SP:SPX")));
    }

    //! @tstid TS-FEED-006 @design DES-SVC-FEEDS
    // @relation(REQ-F-009, scope=function)
    void TS_FEED_006_fearGreedScoreRangeValidated()
    {
        qint32 hits = 0;
        MockHttpServer server([&hits](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("fearandgreed"))) {
                ++hits;
                if (hits == 1) {  // out-of-range score: must be rejected, not published
                    return MockHttpServer::Response{
                        200, R"({"fear_and_greed":{"score":120.0,"rating":"broken"}})", {}};
                }
                return MockHttpServer::Response{
                    200, R"({"fear_and_greed":{"score":72.5,"rating":"greed"}})", {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds rejecting;
        rejecting.setEndpointBaseForTesting(server.baseUrl());
        const QSignalSpy rejected(&rejecting, &MarketFeeds::fearGreedUpdated);
        rejecting.start(60000);
        QTRY_VERIFY_WITH_TIMEOUT(requestCount(server, QStringLiteral("fearandgreed")) >= 1,
                                 kWaitMs);
        QTest::qWait(200);              // let the reply be parsed ...
        QCOMPARE(rejected.count(), 0);  // ... and the 120.0 reading be rejected

        MarketFeeds accepting;
        accepting.setEndpointBaseForTesting(server.baseUrl());
        QSignalSpy accepted(&accepting, &MarketFeeds::fearGreedUpdated);
        accepting.start(60000);
        QVERIFY(accepted.wait(kWaitMs));
        QCOMPARE(accepted.at(0).at(0).toDouble(), 72.5);
        QCOMPARE(accepted.at(0).at(1).toString(), QStringLiteral("greed"));
    }

    //! @tstid TS-FEED-007 @design DES-SVC-FEEDS
    // @relation(REQ-F-019, scope=function)
    void TS_FEED_007_webQuoteWithExchangeTimestamp()
    {
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("GSPC"))) {  // SPX500's Yahoo ticker ^GSPC
                return MockHttpServer::Response{
                    200,
                    yahooChartBody(
                        R"("regularMarketPrice":5432.1,"regularMarketTime":1753900000)", ""),
                    {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        feeds.setCurrentSymbol(QStringLiteral("SPX500"));
        QSignalSpy quote(&feeds, &MarketFeeds::webQuoteUpdated);
        feeds.start(60000);
        QVERIFY(quote.wait(kWaitMs));
        // Independent reference quote for the CURRENT instrument, stamped with
        // the exchange timestamp the panel shows next to the eToro rate.
        QCOMPARE(quote.at(0).at(0).toString(), QStringLiteral("SPX500"));
        QCOMPARE(quote.at(0).at(1).toDouble(), 5432.1);
        QCOMPARE(quote.at(0).at(2).toDateTime(), QDateTime::fromSecsSinceEpoch(1753900000));
        // 1-minute chart of the percent-encoded ticker — byte-identical query.
        QCOMPARE(requestCount(
                     server, QStringLiteral("/v8/finance/chart/%5EGSPC?interval=1m&range=1d")),
                 1);
    }

    //! @tstid TS-FEED-008 @design DES-SVC-FEEDS
    // @relation(REQ-F-022, scope=function)
    void TS_FEED_008_intradayClosesSkipNullMinutes()
    {
        MockHttpServer server([](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("GSPC"))) {
                return MockHttpServer::Response{
                    200, yahooChartBody("", "5000.5,null,5001.5,0.0"), {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        // RUBBER has no Yahoo ticker: it must be skipped without a request.
        feeds.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("RUBBER")});
        bool received = false;
        QString symbol;
        QList<double> closes;
        static_cast<void>(connect(&feeds, &MarketFeeds::intradayCloses, this,
                                  [&received, &symbol, &closes](const QString &sym,
                                                                const QList<double> &c) {
                                      symbol = sym;
                                      closes = c;
                                      received = true;
                                  }));
        feeds.fetchIntradaySeries();
        QTRY_VERIFY_WITH_TIMEOUT(received, kWaitMs);
        QCOMPARE(symbol, QStringLiteral("SPX500"));
        // Empty minutes (null) are skipped; a genuine 0.0 close survives here
        // (positiveOnly filtering is for the VIX baseline only).
        QCOMPARE(closes, (QList<double>{5000.5, 5001.5, 0.0}));
        QCOMPARE(server.requests().size(), 1);  // one mapped instrument, one request
    }

    //! @tstid TS-FEED-009 @design DES-SVC-FEEDS
    // @relation(REQ-F-020, scope=function)
    void TS_FEED_009_feedErrorLogThrottled()
    {
        MockHttpServer server([](const QByteArray &, const QString &) {
            return MockHttpServer::Response{500, R"({"err":"down"})", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        const QSignalSpy logs(&feeds, &MarketFeeds::log);
        feeds.start(150);  // fast cadence: several failing VIX fetches per second
        const QString vixLine = QStringLiteral("VIX feed unavailable");
        QTRY_VERIFY_WITH_TIMEOUT(logCount(logs, vixLine) >= 1, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(requestCount(server, QStringLiteral("VIX")) >= 3, kWaitMs);
        QTest::qWait(300);  // let the later failing replies be processed too
        // All failures fall inside the 10-minute window: one line, not one per poll.
        QCOMPARE(logCount(logs, vixLine), 1);
    }
};

QTEST_GUILESS_MAIN(TestMarketFeeds)
#include "tst_marketfeeds.moc"
