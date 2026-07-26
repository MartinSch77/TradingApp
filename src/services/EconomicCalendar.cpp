#include "services/EconomicCalendar.h"

#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <limits>

namespace {

// TradingView's public economic-calendar endpoint (no API key; used by their
// embeddable widget). Supports a from/to range and country filter.
constexpr auto kEndpoint = "https://economic-calendar.tradingview.com/events";

// How many upcoming trading days (Mon–Fri) to include.
constexpr qint32 kTradingDays = 3;

// A forecast/previous field may arrive as a string ("0.3%") or a number (580).
QString scalarToString(const QJsonValue &v)
{
    if (v.isString()) {
        return v.toString();
    }
    if (v.isDouble()) {
        return QString::number(v.toDouble(), 'g', 6);
    }
    return {};
}

// Map each tradable instrument to the macro regions whose calendar events tend to move
// it (TradingView country codes, comma-separated). Unknown instruments fall back to the
// US — the dominant global macro driver — so the panel is never empty by default.
QString countriesForSymbol(const QString &symbol)
{
    static const QHash<QString, QString> kMap = {
        // US indices and US-listed thematic baskets
        {QStringLiteral("SPX500"), QStringLiteral("US")},
        {QStringLiteral("SP.24-7"), QStringLiteral("US")},
        {QStringLiteral("NSDQ100"), QStringLiteral("US")},
        {QStringLiteral("NSDQ100.24-7"), QStringLiteral("US")},
        {QStringLiteral("DJ30"), QStringLiteral("US")},
        {QStringLiteral("RTY"), QStringLiteral("US")},
        {QStringLiteral("Semiconductors"), QStringLiteral("US")},
        {QStringLiteral("AI.Leaders"), QStringLiteral("US")},
        {QStringLiteral("Cybersecurity"), QStringLiteral("US")},
        {QStringLiteral("Quantum"), QStringLiteral("US")},
        {QStringLiteral("GoldMiners"), QStringLiteral("US")},
        {QStringLiteral("Nuclear"), QStringLiteral("US")},
        {QStringLiteral("Crypto10"), QStringLiteral("US")},
        // USD basket and FX
        {QStringLiteral("USDOLLAR"), QStringLiteral("US,EU")},
        {QStringLiteral("EURUSD"), QStringLiteral("EU,US")},
        // Regional indices
        {QStringLiteral("GER40"), QStringLiteral("DE,EU")},
        {QStringLiteral("EUSTX50"), QStringLiteral("EU,DE,FR")},
        {QStringLiteral("Switzerland20"), QStringLiteral("CH,EU")},
        {QStringLiteral("HKG50"), QStringLiteral("HK,CN")},
        {QStringLiteral("CHINA50"), QStringLiteral("CN")},
        {QStringLiteral("Canada60"), QStringLiteral("CA,US")},
        {QStringLiteral("Sweden30"), QStringLiteral("SE,EU")},
        {QStringLiteral("Colombia"), QStringLiteral("CO,US")},
        // Commodities (USD-priced, Fed / US-macro driven)
        {QStringLiteral("Gold.24-7"), QStringLiteral("US")},
        {QStringLiteral("OIL.24-7"), QStringLiteral("US")},
        {QStringLiteral("RUBBER"), QStringLiteral("CN,US")},
    };
    return kMap.value(symbol, QStringLiteral("US"));
}

} // namespace

EconomicCalendar::EconomicCalendar(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_timer(new QTimer(this))
    , m_nextEventTimer(new QTimer(this))
{
    m_timer->setInterval(30 * 60 * 1000);  // refresh every 30 minutes
    static_cast<void>(connect(m_timer, &QTimer::timeout, this, &EconomicCalendar::refresh));

    m_nextEventTimer->setSingleShot(true);
    static_cast<void>(
        connect(m_nextEventTimer, &QTimer::timeout, this, &EconomicCalendar::refresh));
}

void EconomicCalendar::start()
{
    m_started = true;
    refresh();
    m_timer->start();
}

void EconomicCalendar::setInstrument(const QString &symbol)
{
    const QString countries = countriesForSymbol(symbol);
    if (countries == m_countries) {
        return;  // same regions — nothing to re-fetch
    }
    m_countries = countries;
    if (m_started) {
        refresh();  // re-scope the calendar to the new instrument's regions right away
    }
}

void EconomicCalendar::refresh()
{
    // Build the set of the next kTradingDays weekdays (local time), starting today.
    const QDateTime nowLocal = QDateTime::currentDateTime();
    QList<QDate> tradingDays;
    for (QDate d = nowLocal.date(); tradingDays.size() < kTradingDays; d = d.addDays(1)) {
        if (d.dayOfWeek() <= 5) {  // Mon..Fri
            tradingDays.append(d);
        }
    }
    // Query window: start of today → local midnight after the last trading day,
    // in UTC. Starting at midnight includes events that already happened today.
    const QTime midnight(0, 0);
    const QDate today = nowLocal.date();
    const QDateTime fromUtc = QDateTime(today, midnight, QTimeZone::LocalTime).toUTC();
    const QDate dayAfterLast = tradingDays.last().addDays(1);
    const QDateTime toUtc = QDateTime(dayAfterLast, midnight, QTimeZone::LocalTime).toUTC();

    QUrl url{QString::fromLatin1(kEndpoint)};
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("from"), fromUtc.toString(Qt::ISODateWithMs));
    query.addQueryItem(QStringLiteral("to"), toUtc.toString(Qt::ISODateWithMs));
    query.addQueryItem(QStringLiteral("countries"), m_countries);
    url.setQuery(query);

    QNetworkRequest req{url};
    const QByteArray acceptJson{"application/json"};
    const QByteArray origin{"https://www.tradingview.com"};
    const QByteArray referer{"https://www.tradingview.com/"};
    req.setRawHeader("Accept", acceptJson);
    req.setRawHeader("Origin", origin);
    req.setRawHeader("Referer", referer);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("TradingApp/1.0"));

    QNetworkReply *reply = m_nam->get(req);
    static_cast<void>(connect(reply, &QNetworkReply::finished, this, [this, reply, tradingDays]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit log(QStringLiteral("Economic calendar fetch failed: %1").arg(reply->errorString()),
                     true);
            return;
        }
        const QJsonArray arr =
            QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("result")).toArray();

        QList<EconomicEvent> events;
        for (const auto &v : arr) {
            const QJsonObject o = v.toObject();
            EconomicEvent e;
            e.title = o.value(QStringLiteral("title")).toString();
            e.country = o.value(QStringLiteral("country")).toString();
            e.forecast = scalarToString(o.value(QStringLiteral("forecast")));
            e.previous = scalarToString(o.value(QStringLiteral("previous")));
            const QString date = o.value(QStringLiteral("date")).toString();
            e.when = QDateTime::fromString(date, Qt::ISODateWithMs);
            if (!e.when.isValid()) {
                e.when = QDateTime::fromString(date, Qt::ISODate);
            }

            const qint32 importance = o.value(QStringLiteral("importance")).toInt(-1);
            e.impact = (importance >= 1)
                           ? QStringLiteral("High")
                           : ((importance == 0) ? QStringLiteral("Medium") : QStringLiteral("Low"));

            // The query is already scoped to the instrument's regions; just keep
            // events inside the chosen trading days (today included, weekends in
            // range dropped).
            if (!e.when.isValid()) {
                continue;
            }
            if (!tradingDays.contains(e.when.toLocalTime().date())) {
                continue;
            }
            events.append(e);
        }
        const auto sortBegin = events.begin();
        const auto sortEnd = events.end();
        std::sort(sortBegin, sortEnd,
                  [](const EconomicEvent &a, const EconomicEvent &b) { return a.when < b.when; });
        emit eventsUpdated(events);
        scheduleNextEventRefresh(events);
    }));
}

void EconomicCalendar::scheduleNextEventRefresh(const QList<EconomicEvent> &events)
{
    m_nextEventTimer->stop();

    const QDateTime now = QDateTime::currentDateTime();
    for (const EconomicEvent &e : events) {
        if (e.when <= now) {
            continue;  // already past — keep scanning for the soonest still ahead
        }

        // Two event-aligned refreshes for the soonest upcoming event:
        //   * 30 min BEFORE it, so the events panel is freshly updated going in;
        //   * ~1 s AFTER it, so it rolls into the past and the released actual is picked up.
        // Schedule whichever comes next; each refresh re-runs this and advances to the
        // next moment (the periodic m_timer stays a backstop and re-evaluates too).
        const QDateTime preRefresh = e.when.addSecs(-30LL * 60);
        const qint64 ms = (now < preRefresh) ? now.msecsTo(preRefresh)
                                             : (now.msecsTo(e.when) + 1000);
        const qint64 clamped =
            std::clamp<qint64>(ms, 1000, std::numeric_limits<qint32>::max());
        m_nextEventTimer->start(static_cast<qint32>(clamped));
        return;
    }
}
