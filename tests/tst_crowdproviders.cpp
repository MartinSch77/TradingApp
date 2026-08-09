// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Integration tests for the REAL crowd-sentiment providers (DES-SVC-CROWDLIVE, Phase 3) against
// an in-process mock HTTP server — no test touches the real CFTC or FRED endpoint. They cover the
// parse of a genuine payload shape, the UTC publication LAG (event vs known), the unavailable
// paths (unknown instrument, no key), and the resilience the shared base gives for free: a
// malformed body, a hard server error, and a 429 rate limit that JsonHttp retries through. A last
// check pins that a configured key reaches the wire but never a diagnostic string.

#include "MockHttpServer.h"
#include "domain/CrowdObservation.h"
#include "services/CftcCotProvider.h"
#include "services/FredProvider.h"
#include "services/IgSentimentProvider.h"

#include <QHostAddress>
#include <QSignalSpy>
#include <QTimeZone>
#include <QtTest/QtTest>

using trading::crowd::CftcCotProvider;
using trading::crowd::FredProvider;
using trading::crowd::IgSentimentProvider;
using trading::crowd::Observation;
using trading::crowd::Source;

namespace {

// Generous shared bound: the mock answers in milliseconds; the margin absorbs the retry backoff
// (a 429/500 waits ~1-2 s before JsonHttp re-issues) and CI load.
constexpr qint32 kWaitMs = 15000;

// One CFTC "Traders in Financial Futures" record, Socrata-shaped: numbers arrive as JSON STRINGS
// and the report date carries a midnight time part, exactly as the live API sends them.
QByteArray cotBody()
{
    return R"([{"report_date_as_yyyy_mm_dd":"2026-08-04T00:00:00.000",)"
           R"("market_and_exchange_names":"E-MINI S&P 500 - CHICAGO MERCANTILE EXCHANGE",)"
           R"("asset_mgr_positions_long":"120000","asset_mgr_positions_short":"80000",)"
           R"("lev_money_positions_long":"90000","lev_money_positions_short":"140000"}])";
}

// One FRED observations payload with a single VIX close.
QByteArray fredBody(const QByteArray &value)
{
    return R"({"observations":[{"realtime_start":"2026-08-07","realtime_end":"2026-08-07",)"
           R"("date":"2026-08-06","value":")"
           + value + R"("}]})";
}

// One IG client-sentiment answer, as the live API shapes it.
QByteArray igSentimentBody()
{
    return R"({"longPositionPercentage":62.5,"shortPositionPercentage":37.5,)"
           R"("marketId":"US500"})";
}

// The mock IG: POST /session answers the session tokens as HEADERS (how the real API does it);
// GET /clientsentiment parses. Everything else is 404.
MockHttpServer::Response igHandler(const QByteArray &method, const QString &path)
{
    if ((method == QByteArrayLiteral("POST")) && path.contains(QStringLiteral("/session"))) {
        return {200, "{}",
                {QByteArrayLiteral("CST: test-cst-token"),
                 QByteArrayLiteral("X-SECURITY-TOKEN: test-xst-token")},
                0};
    }
    if (path.contains(QStringLiteral("/clientsentiment/"))) {
        return {200, igSentimentBody(), {}, 0};
    }
    return {404, "{}", {}, 0};
}

} // namespace

class TestCrowdProviders : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker

private slots:
    //! @tstid TS-CROWD-009 @design DES-SVC-CROWDLIVE
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_009_cftcParsesCotWithPublicationLag()
    {
        MockHttpServer server([](const QByteArray &, const QString &path) -> MockHttpServer::Response {
            if (path.contains(QStringLiteral("gpe5-46if.json"))) {
                return {200, cotBody(), {}, 0};
            }
            return {404, "{}", {}, 0};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        CftcCotProvider provider;
        provider.setEndpointBaseForTesting(server.baseUrl());
        QList<Observation> got;
        static_cast<void>(connect(&provider, &CftcCotProvider::observationsReady, this,
                                  [&got](const QList<Observation> &obs) { got = obs; }));

        provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
        QTRY_COMPARE_WITH_TIMEOUT(got.size(), 2, kWaitMs);

        // asset-mgr net = 120000 - 80000 = 40000; leveraged-fund net = 90000 - 140000 = -50000.
        const Observation &assetMgr = got.at(0);
        QCOMPARE(assetMgr.seriesId, QStringLiteral("COT-ASSET-MGR-NET"));
        QCOMPARE(assetMgr.value, 40000.0);
        QCOMPARE(assetMgr.source, Source::InstitutionalPositioning);
        QCOMPARE(assetMgr.sourceName, QStringLiteral("CFTC-COT"));
        QCOMPARE(assetMgr.instrument, QStringLiteral("SPX500"));
        QVERIFY(assetMgr.valid);
        // UTC, and the lag is the point: ABOUT Tuesday 2026-08-04, KNOWN the following Friday.
        QCOMPARE(assetMgr.eventTime, QDateTime(QDate(2026, 8, 4), QTime(20, 0), QTimeZone::UTC));
        QCOMPARE(assetMgr.receivedTime,
                 QDateTime(QDate(2026, 8, 7), QTime(20, 30), QTimeZone::UTC));
        QVERIFY(assetMgr.receivedTime > assetMgr.eventTime);

        QCOMPARE(got.at(1).seriesId, QStringLiteral("COT-LEV-FUND-NET"));
        QCOMPARE(got.at(1).value, -50000.0);

        // fetch() returns the last refreshed result synchronously.
        QCOMPARE(provider.fetch(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc())
                     .observations.size(),
                 2);
    }

    //! @tstid TS-CROWD-010 @design DES-SVC-CROWDLIVE
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_010_cftcResilienceAndUnknownInstrument()
    {
        // (a) An instrument the COT report does not cover fetches NOTHING — no request, no error.
        {
            MockHttpServer server([](const QByteArray &, const QString &) -> MockHttpServer::Response {
                return {200, cotBody(), {}, 0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            CftcCotProvider provider;
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy ready(&provider, &CftcCotProvider::observationsReady);
            provider.refresh(QStringLiteral("GOLD"), QDateTime::currentDateTimeUtc());
            QTest::qWait(200);   // legitimate: asserting the ABSENCE of any network call
            QVERIFY(server.requests().isEmpty());
            QCOMPARE(ready.count(), 0);
        }

        // (b) An empty Socrata array (unknown market) is a named error, not a crash or a zero row.
        {
            MockHttpServer server([](const QByteArray &, const QString &) -> MockHttpServer::Response {
                return {200, "[]", {}, 0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            CftcCotProvider provider;
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy errors(&provider, &CftcCotProvider::providerError);
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, kWaitMs);
        }

        // (c) A hard server error, retried by JsonHttp, ends as a reported error (never a hang).
        {
            MockHttpServer server([](const QByteArray &, const QString &) -> MockHttpServer::Response {
                return {500, "{}", {}, 0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            CftcCotProvider provider;
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy errors(&provider, &CftcCotProvider::providerError);
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, kWaitMs);
            QVERIFY(server.requests().size() >= 2);   // the base retried the idempotent GET
        }

        // (d) A 429 rate limit is transparently retried: the second attempt succeeds and parses.
        {
            int hits = 0;
            MockHttpServer server([&hits](const QByteArray &,
                                          const QString &) -> MockHttpServer::Response {
                if (hits++ == 0) {
                    return {429, "{}", {"Retry-After: 1"}, 0};
                }
                return {200, cotBody(), {}, 0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            CftcCotProvider provider;
            provider.setEndpointBaseForTesting(server.baseUrl());
            QList<Observation> got;
            static_cast<void>(connect(&provider, &CftcCotProvider::observationsReady, this,
                                      [&got](const QList<Observation> &obs) { got = obs; }));
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTRY_COMPARE_WITH_TIMEOUT(got.size(), 2, kWaitMs);
        }
    }

    //! @tstid TS-CROWD-011 @design DES-SVC-CROWDLIVE
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_011_fredParsesVixCloseInUtc()
    {
        MockHttpServer server([](const QByteArray &, const QString &path) -> MockHttpServer::Response {
            if (path.contains(QStringLiteral("/fred/series/observations"))) {
                return {200, fredBody("15.42"), {}, 0};
            }
            return {404, "{}", {}, 0};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        FredProvider provider;
        provider.setApiKey(QStringLiteral("DUMMY-TEST-KEY"));
        QVERIFY(provider.isConfigured());
        provider.setEndpointBaseForTesting(server.baseUrl());
        QList<Observation> got;
        static_cast<void>(connect(&provider, &FredProvider::observationsReady, this,
                                  [&got](const QList<Observation> &obs) { got = obs; }));

        provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
        QTRY_COMPARE_WITH_TIMEOUT(got.size(), 1, kWaitMs);

        const Observation &vix = got.at(0);
        QCOMPARE(vix.seriesId, QStringLiteral("VIX"));
        QCOMPARE(vix.value, 15.42);
        QCOMPARE(vix.source, Source::Volatility);
        QCOMPARE(vix.sourceName, QStringLiteral("FRED"));
        QVERIFY(vix.valid);
        QCOMPARE(vix.eventTime, QDateTime(QDate(2026, 8, 6), QTime(20, 0), QTimeZone::UTC));

        // The key must reach the wire (FRED requires it) — but only as a request parameter.
        bool keyOnWire = false;
        for (const MockHttpServer::Recorded &request : server.requests()) {
            if (request.path.contains(QStringLiteral("DUMMY-TEST-KEY"))) {
                keyOnWire = true;
            }
        }
        QVERIFY(keyOnWire);
    }

    //! @tstid TS-CROWD-012 @design DES-SVC-CROWDLIVE
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_012_fredUnavailableWithoutKeyAndNeverLeaksIt()
    {
        // (a) No key: the provider is UNAVAILABLE and makes no call — the app carries on.
        {
            MockHttpServer server([](const QByteArray &, const QString &) -> MockHttpServer::Response {
                return {200, fredBody("15.42"), {}, 0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            FredProvider provider;
            provider.setApiKey(QString());   // deterministic: ignore any ambient env key
            QVERIFY(!provider.isConfigured());
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy ready(&provider, &FredProvider::observationsReady);
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTest::qWait(200);
            QVERIFY(server.requests().isEmpty());
            QCOMPARE(ready.count(), 0);
        }

        // (b) A missing print ("." on a holiday) is an error, never a zero VIX; and the error text
        // never carries the key.
        {
            MockHttpServer server([](const QByteArray &, const QString &) -> MockHttpServer::Response {
                return {200, fredBody("."), {}, 0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            FredProvider provider;
            provider.setApiKey(QStringLiteral("SECRET-KEY-XYZ"));
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy errors(&provider, &FredProvider::providerError);
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, kWaitMs);
            QVERIFY(!errors.at(0).at(0).toString().contains(QStringLiteral("SECRET-KEY-XYZ")));
        }
    }

    //! @tstid TS-CROWD-013 @design DES-SVC-CROWDLIVE
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_013_igLogsInParsesSentimentAndReusesSession()
    {
        MockHttpServer server(igHandler);
        QVERIFY(server.listen(QHostAddress::LocalHost));

        IgSentimentProvider provider;
        provider.setCredentials(QStringLiteral("TEST-IG-KEY"), QStringLiteral("test-user"),
                                QStringLiteral("test-pass"));
        QVERIFY(provider.isConfigured());
        provider.setEndpointBaseForTesting(server.baseUrl());
        QList<Observation> got;
        static_cast<void>(connect(&provider, &IgSentimentProvider::observationsReady, this,
                                  [&got](const QList<Observation> &obs) { got = obs; }));

        const QDateTime now(QDate(2026, 8, 7), QTime(12, 0), QTimeZone::UTC);
        provider.refresh(QStringLiteral("SPX500"), now);
        QTRY_COMPARE_WITH_TIMEOUT(got.size(), 1, kWaitMs);

        const Observation &retail = got.at(0);
        QCOMPARE(retail.seriesId, QStringLiteral("IG-PCT-LONG"));
        QCOMPARE(retail.value, 62.5);
        QCOMPARE(retail.source, Source::RetailPositioning);
        QCOMPARE(retail.sourceName, QStringLiteral("IG"));
        QCOMPARE(retail.unit, QStringLiteral("percent"));
        QVERIFY(retail.valid);
        // A live snapshot: ABOUT now and KNOWN now, in UTC — no publication lag to model.
        QCOMPARE(retail.eventTime, now);
        QCOMPARE(retail.receivedTime, now);

        // The wire shape: the login POST carried the key as a header and the identifier in the
        // BODY; the sentiment GET carried both session tokens. No credential is ever in a URL.
        // Compared lowercased: HTTP header names are case-insensitive and QNetworkAccessManager
        // title-cases raw headers on the wire ("X-Ig-Api-Key"), which must stay irrelevant.
        const QList<MockHttpServer::Recorded> first = server.requests();
        QCOMPARE(first.size(), 2);
        QCOMPARE(first.at(0).method, QByteArrayLiteral("POST"));
        QVERIFY(first.at(0).headers.toLower().contains(
            QByteArrayLiteral("x-ig-api-key: test-ig-key")));
        QVERIFY(first.at(0).body.contains(QByteArrayLiteral("test-user")));
        QVERIFY(first.at(1).headers.toLower().contains(
            QByteArrayLiteral("cst: test-cst-token")));
        QVERIFY(first.at(1).headers.toLower().contains(
            QByteArrayLiteral("x-security-token: test-xst-token")));
        for (const MockHttpServer::Recorded &request : first) {
            QVERIFY(!request.path.contains(QStringLiteral("test-pass")));
            QVERIFY(!request.path.contains(QStringLiteral("TEST-IG-KEY")));
        }

        // A second refresh REUSES the fresh session: one more sentiment GET, no second login.
        got.clear();
        provider.refresh(QStringLiteral("SPX500"), now.addSecs(60));
        QTRY_COMPARE_WITH_TIMEOUT(got.size(), 1, kWaitMs);
        qint32 logins = 0;
        for (const MockHttpServer::Recorded &request : server.requests()) {
            logins += (request.method == QByteArrayLiteral("POST")) ? 1 : 0;
        }
        QCOMPARE(logins, 1);
        QCOMPARE(server.requests().size(), 3);
    }

    //! @tstid TS-CROWD-014 @design DES-SVC-CROWDLIVE
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_014_igUnavailableWithoutCredentialsAndNeverLeaksThem()
    {
        // (a) Any missing credential: UNAVAILABLE, no network call — the feature is opt-in.
        {
            MockHttpServer server(igHandler);
            QVERIFY(server.listen(QHostAddress::LocalHost));
            IgSentimentProvider provider;
            provider.setCredentials(QStringLiteral("TEST-IG-KEY"), QString(), QString());
            QVERIFY(!provider.isConfigured());
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy ready(&provider, &IgSentimentProvider::observationsReady);
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTest::qWait(200);   // legitimate: asserting the ABSENCE of any network call
            QVERIFY(server.requests().isEmpty());
            QCOMPARE(ready.count(), 0);
        }

        // (b) A failed login is ONE named error and exactly ONE request — the non-idempotent
        // POST is never auto-retried — and the diagnostic carries no credential.
        {
            MockHttpServer server([](const QByteArray &, const QString &) -> MockHttpServer::Response {
                return {500, "{}", {}, 0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            IgSentimentProvider provider;
            provider.setCredentials(QStringLiteral("SECRET-IG-KEY"), QStringLiteral("secret-user"),
                                    QStringLiteral("secret-pass"));
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy errors(&provider, &IgSentimentProvider::providerError);
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, kWaitMs);
            QCOMPARE(server.requests().size(), 1);
            const QString detail = errors.at(0).at(0).toString();
            QVERIFY(!detail.contains(QStringLiteral("SECRET-IG-KEY")));
            QVERIFY(!detail.contains(QStringLiteral("secret-user")));
            QVERIFY(!detail.contains(QStringLiteral("secret-pass")));
        }

        // (c) A login that succeeds but a sentiment answer with no usable percentage is a named
        // error, never a zero observation — and still no credential in the diagnostic.
        {
            MockHttpServer server([](const QByteArray &method,
                                     const QString &path) -> MockHttpServer::Response {
                if ((method == QByteArrayLiteral("POST"))
                    && path.contains(QStringLiteral("/session"))) {
                    return {200, "{}",
                            {QByteArrayLiteral("CST: test-cst-token"),
                             QByteArrayLiteral("X-SECURITY-TOKEN: test-xst-token")},
                            0};
                }
                return {200, R"({"marketId":"US500"})", {}, 0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            IgSentimentProvider provider;
            provider.setCredentials(QStringLiteral("SECRET-IG-KEY"), QStringLiteral("secret-user"),
                                    QStringLiteral("secret-pass"));
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy errors(&provider, &IgSentimentProvider::providerError);
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, kWaitMs);
            const QString detail = errors.at(0).at(0).toString();
            QVERIFY(!detail.contains(QStringLiteral("SECRET-IG-KEY")));
            QVERIFY(!detail.contains(QStringLiteral("secret-pass")));
        }

        // (d) IG's EMPTY record — 0.0/0.0 WITH the fields present — is refused the same way
        // (measured live 2026-08-09: an unknown marketId answers HTTP 200 with exactly this
        // shape). Stored, it would read as the most extreme contrarian signal there is.
        {
            MockHttpServer server([](const QByteArray &method,
                                     const QString &path) -> MockHttpServer::Response {
                if ((method == QByteArrayLiteral("POST"))
                    && path.contains(QStringLiteral("/session"))) {
                    return {200, "{}",
                            {QByteArrayLiteral("CST: test-cst-token"),
                             QByteArrayLiteral("X-SECURITY-TOKEN: test-xst-token")},
                            0};
                }
                return {200,
                        R"({"marketId":"US500","longPositionPercentage":0.0,)"
                        R"("shortPositionPercentage":0.0})",
                        {},
                        0};
            });
            QVERIFY(server.listen(QHostAddress::LocalHost));
            IgSentimentProvider provider;
            provider.setCredentials(QStringLiteral("k"), QStringLiteral("u"),
                                    QStringLiteral("p"));
            provider.setEndpointBaseForTesting(server.baseUrl());
            const QSignalSpy errors(&provider, &IgSentimentProvider::providerError);
            provider.refresh(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc());
            QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, kWaitMs);
            QVERIFY(provider.fetch(QStringLiteral("SPX500"), QDateTime::currentDateTimeUtc())
                        .observations.isEmpty());
        }
    }
};

QTEST_MAIN(TestCrowdProviders)
#include "tst_crowdproviders.moc"
