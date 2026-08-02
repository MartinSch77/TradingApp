// Integration tests for the macro-economic calendar service (DES-SVC-CAL)
// against an in-process mock HTTP server: event parsing (title/impact/when/
// forecast/previous), the trading-day window filter, region scoping through
// the InstrumentCatalog via setInstrument(), and the fetch-error log line.
// The calendar host is redirected to the mock via setEndpointBaseForTesting()
// — no test touches the real network.

#include "MockHttpServer.h"
#include "services/EconomicCalendar.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest/QtTest>

namespace {

// Generous shared bound for spy waits: the mock answers in milliseconds, the
// margin only absorbs CI load.
constexpr qint32 kWaitMs = 15000;

// The next `count` weekdays starting today — replicating the service's window
// so the fixture events land inside it whatever day the test runs on.
QList<QDate> nextTradingDays(qint32 count)
{
    QList<QDate> days;
    for (QDate d = QDate::currentDate(); days.size() < count; d = d.addDays(1)) {
        if (d.dayOfWeek() <= 5) {
            days.append(d);
        }
    }
    return days;
}

// The feed's ISO timestamp for a local wall-clock moment (the feed serves UTC).
QString utcIso(const QDate &day, const QTime &time)
{
    return QDateTime(day, time, QTimeZone::LocalTime).toUTC().toString(Qt::ISODateWithMs);
}

// One calendar event as the feed sends it; pass {} to leave a field absent.
QJsonObject eventObj(const QString &title, const QString &date, const QJsonValue &importance,
                     const QJsonValue &forecast, const QJsonValue &previous)
{
    QJsonObject o{{QStringLiteral("title"), title},
                  {QStringLiteral("country"), QStringLiteral("USD")},
                  {QStringLiteral("date"), date}};
    if (!importance.isNull() && !importance.isUndefined()) {
        o[QStringLiteral("importance")] = importance;
    }
    if (!forecast.isNull() && !forecast.isUndefined()) {
        o[QStringLiteral("forecast")] = forecast;
    }
    if (!previous.isNull() && !previous.isUndefined()) {
        o[QStringLiteral("previous")] = previous;
    }
    return o;
}

// The `countries` query parameter of a recorded request path.
QString countriesOf(const QString &pathAndQuery)
{
    const qsizetype q = pathAndQuery.indexOf(u'?');
    return QUrlQuery(pathAndQuery.mid(q + 1))
        .queryItemValue(QStringLiteral("countries"), QUrl::FullyDecoded);
}

} // namespace

class TestEconomicCalendar : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-CAL-001 @design DES-SVC-CAL
    // @relation(REQ-F-020, scope=function)
    void TS_CAL_001_eventsParsedFilteredAndSorted()
    {
        const QList<QDate> days = nextTradingDays(3);
        const QDate last = days.last();
        // Out of order on purpose (the CPI print first) plus two events the
        // service must drop: an unparsable date and one beyond the window.
        const QJsonArray result{
            eventObj(QStringLiteral("CPI m/m"), utcIso(last, QTime(23, 59)), 1,
                     QStringLiteral("0.3%"), 580),
            eventObj(QStringLiteral("GDP q/q"), utcIso(last, QTime(23, 58)), 0, {}, {}),
            eventObj(QStringLiteral("Rate decision"), utcIso(last, QTime(23, 57)), {}, {}, {}),
            eventObj(QStringLiteral("Bad date"), QStringLiteral("not-a-date"), 1, {}, {}),
            eventObj(QStringLiteral("Beyond window"), utcIso(last.addDays(10), QTime(12, 0)), 1,
                     {}, {}),
        };
        const QByteArray body =
            QJsonDocument(QJsonObject{{QStringLiteral("result"), result}})
                .toJson(QJsonDocument::Compact);
        MockHttpServer server([body](const QByteArray &, const QString &) {
            return MockHttpServer::Response{200, body, {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EconomicCalendar cal;
        cal.setEndpointBaseForTesting(server.baseUrl());
        bool received = false;
        QList<EconomicEvent> events;
        static_cast<void>(connect(&cal, &EconomicCalendar::eventsUpdated, this,
                                  [&received, &events](const QList<EconomicEvent> &e) {
                                      events = e;
                                      received = true;
                                  }));
        cal.start();
        QTRY_VERIFY_WITH_TIMEOUT(received, kWaitMs);

        QCOMPARE(events.size(), 3);  // bad date and out-of-window event dropped
        // Time-sorted ascending, importance mapped missing/0/1 -> Low/Medium/High.
        QCOMPARE(events.at(0).title, QStringLiteral("Rate decision"));
        QCOMPARE(events.at(0).impact, QStringLiteral("Low"));
        QCOMPARE(events.at(1).title, QStringLiteral("GDP q/q"));
        QCOMPARE(events.at(1).impact, QStringLiteral("Medium"));
        QCOMPARE(events.at(2).title, QStringLiteral("CPI m/m"));
        QCOMPARE(events.at(2).impact, QStringLiteral("High"));
        QCOMPARE(events.at(2).country, QStringLiteral("USD"));
        QCOMPARE(events.at(2).forecast, QStringLiteral("0.3%"));   // string passed through
        QCOMPARE(events.at(2).previous, QStringLiteral("580"));    // number stringified
        QCOMPARE(events.at(2).when,
                 QDateTime(last, QTime(23, 59), QTimeZone::LocalTime));
    }

    //! @tstid TS-CAL-002 @design DES-SVC-CAL
    // @relation(REQ-F-020, scope=function)
    void TS_CAL_002_regionScopingFollowsInstrument()
    {
        MockHttpServer server([](const QByteArray &, const QString &) {
            return MockHttpServer::Response{200, R"({"result":[]})", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EconomicCalendar cal;
        cal.setEndpointBaseForTesting(server.baseUrl());
        QSignalSpy updated(&cal, &EconomicCalendar::eventsUpdated);
        cal.setInstrument(QStringLiteral("EURUSD"));  // before start(): scoped, but no fetch yet
        QCOMPARE(server.requests().size(), 0);

        cal.start();
        QVERIFY(updated.wait(kWaitMs));
        QCOMPARE(countriesOf(server.requests().last().path), QStringLiteral("EU,US"));

        cal.setInstrument(QStringLiteral("HKG50"));   // new regions -> immediate re-fetch
        QVERIFY(updated.wait(kWaitMs));
        QCOMPARE(countriesOf(server.requests().last().path), QStringLiteral("HK,CN"));

        const qsizetype before = server.requests().size();
        cal.setInstrument(QStringLiteral("HKG50"));   // same regions -> nothing to re-fetch
        QTest::qWait(200);
        QCOMPARE(server.requests().size(), before);

        cal.setInstrument(QStringLiteral("NOT-IN-CATALOG"));  // unknown -> US fallback
        QVERIFY(updated.wait(kWaitMs));
        QCOMPARE(countriesOf(server.requests().last().path), QStringLiteral("US"));
    }

    //! @tstid TS-CAL-003 @design DES-SVC-CAL
    // @relation(REQ-F-020, scope=function)
    void TS_CAL_003_fetchFailureLogsError()
    {
        MockHttpServer server([](const QByteArray &, const QString &) {
            return MockHttpServer::Response{500, R"({"err":"down"})", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));

        EconomicCalendar cal;
        cal.setEndpointBaseForTesting(server.baseUrl());
        const QSignalSpy updated(&cal, &EconomicCalendar::eventsUpdated);
        QSignalSpy logs(&cal, &EconomicCalendar::log);
        cal.start();
        QVERIFY(logs.wait(kWaitMs));
        QVERIFY(logs.at(0).at(0).toString().startsWith(
            QStringLiteral("Economic calendar fetch failed")));
        QVERIFY(logs.at(0).at(1).toBool());   // reported as an error
        QCOMPARE(updated.count(), 0);         // no event list published from a failed fetch
    }
};

QTEST_GUILESS_MAIN(TestEconomicCalendar)
#include "tst_economiccalendar.moc"
