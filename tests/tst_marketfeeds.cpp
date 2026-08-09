// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Integration tests for the public market feeds (DES-SVC-FEEDS) against an
// in-process mock HTTP server: the VIX reading vs its multi-month baseline,
// the TradingView rating (current instrument and bulk with ticker->symbols
// fan-out), news headlines, the CNN Fear & Greed index, the Yahoo reference
// quote with its exchange timestamp, the intraday close series and the
// throttled feed-error log. Every feed host is redirected to the mock via
// setEndpointBaseForTesting() — no test touches the real network.

#include "MockHttpServer.h"
#include "domain/DecisionEngine.h"
#include "domain/IndexConfluence.h"
#include "services/MarketFeeds.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
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

    //! @tstid TS-FEED-015 @design DES-SVC-FEEDS
    // @relation(REQ-F-022, scope=function)
    void TS_FEED_015_aClosedSessionFallsBackToTheLastSessionsCandles()
    {
        // A closed market (a weekend for the .24-7 futures) answers range=1d with a valid but
        // EMPTY 1-minute series, which left their candle chart blank. The fetch must fall back
        // ONCE to a wider range and draw the LAST session's candles. Here 1d is empty and 5d
        // carries one bar.
        const QByteArray ohlc5d =
            R"({"chart":{"result":[{"timestamp":[1000],"indicators":{"quote":[{)"
            R"("open":[10.0],"high":[12.0],"low":[9.0],"close":[11.0]}]}}]}})";
        MockHttpServer server([&ohlc5d](const QByteArray &, const QString &path) {
            if (path.contains(QStringLiteral("range=1d"))) {
                return MockHttpServer::Response{200, yahooChartBody("", ""), {}};   // empty session
            }
            return MockHttpServer::Response{200, ohlc5d, {}};                       // 5d has a bar
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        feeds.setTradableSymbols({QStringLiteral("SP.24-7")});   // Yahoo ES=F
        QSignalSpy candles(&feeds, &MarketFeeds::intradayCandles);
        const QSignalSpy closes(&feeds, &MarketFeeds::intradayCloses);
        feeds.fetchIntradaySeries();

        // The fallback fires: exactly one 1d and one 5d request (no recursion), and the 5d bar
        // is emitted as a candle for the same app symbol.
        QTRY_COMPARE_WITH_TIMEOUT(candles.count(), 1, kWaitMs);
        QCOMPARE(requestCount(server, QStringLiteral("range=1d")), 1);
        QCOMPARE(requestCount(server, QStringLiteral("range=5d")), 1);
        QCOMPARE(candles.at(0).at(0).toString(), QStringLiteral("SP.24-7"));
        // Candles only: the decision composite's close series is NOT fed the wider range.
        QCOMPARE(closes.count(), 0);
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
    //! @tstid TS-FEED-010 @design DES-SVC-FEEDS
    // @relation(REQ-F-035, scope=function)
    void TS_FEED_010_theReferenceSweepFetchesEveryTickerOnce()
    {
        // The sweep behind the independent reads: fifteen tickers, each fetched once,
        // each arriving under its own name. A ticker that fails leaves its read ABSENT
        // rather than zero — which is what makes "unknown never counts as agreement"
        // true at the feed level rather than only in the arithmetic.
        MockHttpServer server([](const QByteArray &, const QString &path) {
            // ^TNX answers rubbish and NVDA fails outright: two different ways for a
            // read to be unavailable.
            if (path.contains(QStringLiteral("TNX"))) {
                return MockHttpServer::Response{200, R"({"chart":{"result":[]}})", {}};
            }
            if (path.contains(QStringLiteral("NVDA"))) {
                return MockHttpServer::Response{503, R"({"err":"nope"})", {}};
            }
            return MockHttpServer::Response{
                200, yahooChartBody(R"("regularMarketPrice":100.0)", "100.0,101.0,102.0"), {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        const QSignalSpy series(&feeds, &MarketFeeds::referenceSeries);
        feeds.fetchReferenceSeries();

        const QStringList wanted = trading::referenceTickers();
        QTRY_VERIFY_WITH_TIMEOUT(series.count() >= wanted.size() - 2, kWaitMs);
        QSet<QString> arrived;
        for (qsizetype i = 0; i < series.count(); ++i) {
            static_cast<void>(arrived.insert(series.at(i).at(0).toString()));
            QVERIFY(!series.at(i).at(1).value<QList<double>>().isEmpty());
        }
        // The two broken ones are absent; every other ticker arrived exactly once.
        QVERIFY(!arrived.contains(QStringLiteral("^TNX")));
        QVERIFY(!arrived.contains(QStringLiteral("NVDA")));
        QVERIFY(arrived.contains(QStringLiteral("^VIX")));
        QCOMPARE(arrived.size(), series.count());
    }

    //! @tstid TS-FEED-011 @design DES-SVC-FEEDS
    // @relation(REQ-F-009, scope=function)
    void TS_FEED_011_aFeedThatCannotBeReadReportsNothingRatherThanZero()
    {
        // Every "the payload is not what we asked for" branch of the feeds, one per
        // response shape. The rule under test is the same everywhere: a feed that
        // cannot be read must leave its reading ABSENT, because a zero would be
        // indistinguishable from a real measurement of zero.
        MockHttpServer server([](const QByteArray &method, const QString &path) {
            if (path.contains(QStringLiteral("VIX"))) {
                // No price and no closes at all: nothing to publish.
                return MockHttpServer::Response{200, yahooChartBody(R"("x":1)", ""), {}};
            }
            if ((method == "POST") && path.contains(QStringLiteral("/global/scan"))) {
                // A scan answer whose data array is empty.
                return MockHttpServer::Response{200, R"({"data":[]})", {}};
            }
            if (path.contains(QStringLiteral("fearandgreed"))) {
                // Score outside 0..100 is refused rather than clamped.
                return MockHttpServer::Response{200, R"({"fear_and_greed":{"score":150}})", {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        const QSignalSpy vix(&feeds, &MarketFeeds::vixUpdated);
        const QSignalSpy fear(&feeds, &MarketFeeds::fearGreedUpdated);
        const QSignalSpy external(&feeds, &MarketFeeds::externalSignalUpdated);
        feeds.setCurrentSymbol(QStringLiteral("SPX500"));
        feeds.start(60000);
        QTest::qWait(600);
        // Nothing publishable arrived from either feed…
        QCOMPARE(vix.count(), 0);
        QCOMPARE(fear.count(), 0);
        // …and the rating reports "no rating for this instrument" rather than 0.0,
        // which would read as a neutral opinion the feed never gave.
        if (external.count() > 0) {
            QVERIFY(!external.at(0).at(0).toBool());
        }

        // Setting the SAME instrument again is a no-op rather than a second fetch.
        feeds.setCurrentSymbol(QStringLiteral("SPX500"));
        const qint32 before = requestCount(server, QStringLiteral("scan"));
        feeds.setCurrentSymbol(QStringLiteral("SPX500"));
        QTest::qWait(200);
        QCOMPARE(requestCount(server, QStringLiteral("scan")), before);
    }
    //! @tstid TS-FEED-012 @design DES-SVC-FEEDS
    // @relation(REQ-F-019, scope=function)
    void TS_FEED_012_everyFeedDropsWhatItCannotUseAndKeepsTheRest()
    {
        // Each feed answers a differently-broken payload here, and the rule is the
        // same in every case: the unusable part is dropped, the usable part still
        // arrives. A feed that discards a whole batch because one row was null is how
        // a screen goes blank for no visible reason.
        MockHttpServer server([](const QByteArray &method, const QString &path) {
            if ((method == "POST") && path.contains(QStringLiteral("/global/scan"))) {
                // A bulk rating answer with three rows: one complete, one whose
                // timeframes are all null (unusable), and one for a ticker nobody
                // asked about (no symbol to attach it to).
                return MockHttpServer::Response{
                    200,
                    R"({"data":[
                        {"s":"SP:SPX","d":[0.3,0.4,0.5]},
                        {"s":"NASDAQ:NDX","d":[null,null,null]},
                        {"s":"NOBODY:ASKED","d":[0.9,0.9,0.9]}
                    ]})",
                    {}};
            }
            if (path.contains(QStringLiteral("fearandgreed"))) {
                // A score of exactly 0 and exactly 100 are both LEGAL readings; the
                // boundary is what a naive `> 0` check gets wrong.
                return MockHttpServer::Response{
                    200, R"({"fear_and_greed":{"score":0,"rating":"extreme fear"}})", {}};
            }
            if (path.contains(QStringLiteral("news")) || path.contains(QStringLiteral("rss"))) {
                return MockHttpServer::Response{200, R"({"items":[]})", {}};
            }
            return MockHttpServer::Response{200, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        QSignalSpy ratings(&feeds, &MarketFeeds::instrumentRatingsUpdated);
        const QSignalSpy fear(&feeds, &MarketFeeds::fearGreedUpdated);
        feeds.setTradableSymbols({QStringLiteral("SPX500"), QStringLiteral("NSDQ100")});
        // Asked for explicitly rather than waiting for the poll timer: the bulk
        // ratings call is not part of start()'s first tick.
        feeds.fetchInstrumentRatings();
        // Fear & Greed is on the poll cycle rather than a public call, so start() is
        // what triggers it; a short interval keeps the test quick.
        feeds.start(150);

        QTRY_VERIFY_WITH_TIMEOUT(!ratings.isEmpty(), kWaitMs);
        const auto bySymbol = ratings.last().at(0).value<QHash<QString, WebRating>>();
        // The complete row arrived, the all-null one did not, and the unrequested
        // ticker attached to nothing.
        QVERIFY(bySymbol.contains(QStringLiteral("SPX500")));
        QVERIFY(!bySymbol.contains(QStringLiteral("NSDQ100")));
        QCOMPARE(bySymbol.size(), 1);

        // A Fear & Greed score of exactly 0 is a real reading — "extreme fear" — and
        // must not be dropped as if it were missing.
        QTRY_VERIFY_WITH_TIMEOUT(!fear.isEmpty(), kWaitMs);
        QCOMPARE(fear.last().at(0).toDouble(), 0.0);
        // The label is CNN's own word, passed through rather than re-derived — the app
        // does not invent a wording for someone else's index.
        QCOMPARE(fear.last().at(1).toString(), QStringLiteral("extreme fear"));
    }
    //! @tstid TS-FEED-013 @design DES-SVC-FEEDS
    // @relation(REQ-F-009, scope=function)
    void TS_FEED_013_everyFeedSurvivesAFailingServerAndANonObjectPayload()
    {
        // Public web feeds are the least reliable input this app has — no contract, no
        // support, and they change without notice. The property that matters is not
        // what they publish when they work but what the app does when they do not:
        // nothing, quietly, keeping the last known reading.

        // 1. Every feed fails.
        {
            MockHttpServer dead([](const QByteArray &, const QString &) {
                return MockHttpServer::Response{503, R"({"err":"down"})", {}};
            });
            QVERIFY(dead.listen(QHostAddress::LocalHost));
            MarketFeeds feeds;
            feeds.setEndpointBaseForTesting(dead.baseUrl());
            const QSignalSpy vix(&feeds, &MarketFeeds::vixUpdated);
            const QSignalSpy fear(&feeds, &MarketFeeds::fearGreedUpdated);
            const QSignalSpy external(&feeds, &MarketFeeds::externalSignalUpdated);
            const QSignalSpy ratings(&feeds, &MarketFeeds::instrumentRatingsUpdated);
            const QSignalSpy series(&feeds, &MarketFeeds::referenceSeries);
            const QSignalSpy intraday(&feeds, &MarketFeeds::intradayCloses);
            feeds.setTradableSymbols({QStringLiteral("SPX500")});
            feeds.setCurrentSymbol(QStringLiteral("SPX500"));
            feeds.fetchInstrumentRatings();
            feeds.fetchReferenceSeries();
            feeds.fetchIntradaySeries();
            feeds.start(150);
            QTest::qWait(1200);

            // Not one reading was published from a failed answer.
            QCOMPARE(vix.count(), 0);
            QCOMPARE(fear.count(), 0);
            QCOMPARE(ratings.count(), 0);
            QCOMPARE(series.count(), 0);
            QCOMPARE(intraday.count(), 0);
            // The rating is the one exception, and it is deliberate: it reports
            // "no rating available" so the panel can say so rather than showing a
            // stale number as if it were current.
            for (qsizetype i = 0; i < external.count(); ++i) {
                QVERIFY(!external.at(i).at(0).toBool());
            }
        }

        // 2. Every feed answers 200 with a bare ARRAY where an object belongs — the
        //    single most common way one of these endpoints changes shape.
        {
            MockHttpServer wrong([](const QByteArray &, const QString &) {
                return MockHttpServer::Response{200, R"([1,2,3])", {}};
            });
            QVERIFY(wrong.listen(QHostAddress::LocalHost));
            MarketFeeds feeds;
            feeds.setEndpointBaseForTesting(wrong.baseUrl());
            const QSignalSpy vix(&feeds, &MarketFeeds::vixUpdated);
            const QSignalSpy fear(&feeds, &MarketFeeds::fearGreedUpdated);
            const QSignalSpy series(&feeds, &MarketFeeds::referenceSeries);
            const QSignalSpy intraday(&feeds, &MarketFeeds::intradayCloses);
            feeds.setTradableSymbols({QStringLiteral("SPX500")});
            feeds.setCurrentSymbol(QStringLiteral("SPX500"));
            feeds.fetchReferenceSeries();
            feeds.fetchIntradaySeries();
            feeds.start(150);
            QTest::qWait(1200);
            QCOMPARE(vix.count(), 0);
            QCOMPARE(fear.count(), 0);
            QCOMPARE(series.count(), 0);
            QCOMPARE(intraday.count(), 0);
        }
    }

    //! @tstid TS-FEED-014 @design DES-SVC-FEEDS
    // @relation(REQ-F-035, scope=function)
    void TS_FEED_014_aBarlessEquityResponseStillYieldsItsSessionChange()
    {
        // The regression for a SILENT, TOTAL loss of the heavyweight reads. Measured
        // 2026-08-07 mid-session (11:47 New York, Thursday, cash market open) with a
        // browser User-Agent: Yahoo answered 200 for AAPL with NO timestamp key and an
        // empty close array, while ^VIX on the identical request returned bars. Because
        // yahooCloses() then produced nothing, referenceSeries never fired for any of the
        // twelve constituents, heavyweightPulse measured 0 of 10, and the window sat on
        // "no constituent prices yet". The bot's decision log recorded it as "4 of 10
        // reads measured (6 unmeasurable)" on every evaluation — the feature was dark and
        // nothing said so.
        //
        // The same barless response still carries a live price and yesterday's close, and
        // their difference IS the session change. So the read is MEASURED, and the rule
        // that an unmeasurable read stays UNKNOWN is not bent.
        MockHttpServer server([](const QByteArray & /*method*/, const QString &path) {
            if (path.contains(QStringLiteral("AAPL"))) {
                // Exactly the shape observed: meta present, close array empty.
                return MockHttpServer::Response{
                    200,
                    yahooChartBody(R"("regularMarketPrice":311.0,"previousClose":309.38,)"
                                   R"("chartPreviousClose":333.43)",
                                   ""),
                    {}};
            }
            if (path.contains(QStringLiteral("MSFT"))) {
                // No usable meta either: this one must stay absent, not be invented.
                return MockHttpServer::Response{200, yahooChartBody(R"("x":1)", ""), {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MarketFeeds feeds;
        feeds.setEndpointBaseForTesting(server.baseUrl());
        const QSignalSpy series(&feeds, &MarketFeeds::referenceSeries);
        feeds.fetchReferenceSeries();
        QTRY_VERIFY_WITH_TIMEOUT(series.count() >= 1, kWaitMs);

        QList<double> aapl;
        bool sawMsft = false;
        for (qsizetype i = 0; i < series.count(); ++i) {
            const QString ticker = series.at(i).at(0).toString();
            if (ticker == QStringLiteral("AAPL")) {
                aapl = series.at(i).at(1).value<QList<double>>();
            }
            if (ticker == QStringLiteral("MSFT")) {
                sawMsft = true;
            }
        }
        // Two points, in order: yesterday's close then the live price. Read with value()
        // rather than at(): it is bounds-safe, so the reads below cannot be an out-of-range
        // access even on the failure path where the series never arrived.
        QCOMPARE(aapl.size(), qsizetype(2));
        QCOMPARE(aapl.value(0), 309.38);
        QCOMPARE(aapl.value(1), 311.0);
        // And that is the session change the reads consume: +0.524%, NOT the −6.7% that
        // chartPreviousClose (333.43) would have produced. Picking the wrong meta field is
        // the trap this pins — it is present in the payload above precisely so a future
        // edit that reaches for it fails here.
        const double pct = ((aapl.value(1) - aapl.value(0)) / aapl.value(0)) * 100.0;
        QVERIFY(pct > 0.0);
        QVERIFY(qAbs(pct - 0.5236) < 0.01);

        // A response with neither bars nor usable meta publishes NOTHING. The fallback
        // fills a measurable gap; it never manufactures a reading.
        QVERIFY(!sawMsft);
    }
};

QTEST_GUILESS_MAIN(TestMarketFeeds)
#include "tst_marketfeeds.moc"
