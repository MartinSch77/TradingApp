// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/CftcCotProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>

namespace trading::crowd {

namespace {

// The CFTC market name substring for an app instrument's underlying E-mini future. Empty = a
// symbol the COT report does not cover (only the two index futures are meaningful here).
QString cftcMarket(const QString &instrument)
{
    if (instrument.startsWith(QStringLiteral("SPX500"))
        || instrument.startsWith(QStringLiteral("SP.24-7"))) {
        return QStringLiteral("E-MINI S&P 500");
    }
    if (instrument.startsWith(QStringLiteral("NSDQ100")) || instrument.startsWith(QStringLiteral("NQ"))) {
        return QStringLiteral("NASDAQ MINI");
    }
    return {};
}

// Socrata returns numeric columns as JSON STRINGS ("12345"), so read through the variant.
double numberOf(const QJsonObject &record, const char *key)
{
    const QJsonValue value = record.value(QLatin1StringView(key));
    return value.isString() ? value.toString().toDouble() : value.toDouble();
}

Observation cotObservation(const QString &instrument, const QString &seriesId, double net,
                           const QDateTime &eventTime, const QDateTime &receivedTime)
{
    Observation obs;
    obs.instrument = instrument;
    obs.source = Source::InstitutionalPositioning;
    obs.sourceName = QStringLiteral("CFTC-COT");
    obs.seriesId = seriesId;
    obs.eventTime = eventTime;
    obs.receivedTime = receivedTime;
    obs.value = net;
    obs.unit = QStringLiteral("contracts");
    obs.valid = true;
    return obs;
}

} // namespace

CftcCotProvider::CftcCotProvider(QObject *parent) : CrowdHttpProvider(parent) {}

QString CftcCotProvider::name() const
{
    return QStringLiteral("CFTC-COT");
}

Source CftcCotProvider::category() const
{
    return Source::InstitutionalPositioning;
}

bool CftcCotProvider::isConfigured() const
{
    return true;   // public API, no key
}

void CftcCotProvider::refresh(const QString &instrument, const QDateTime & /*now*/)
{
    const QString market = cftcMarket(instrument);
    if (market.isEmpty()) {
        return;   // the report does not cover this instrument — nothing to fetch
    }
    // The latest report for this market. QUrlQuery encodes the SoQL (the market name carries a
    // space and an "&"), so the query is correct without hand-escaping.
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("$where"),
                       QStringLiteral("market_and_exchange_names like '%%%1%%'").arg(market));
    query.addQueryItem(QStringLiteral("$order"),
                       QStringLiteral("report_date_as_yyyy_mm_dd DESC"));
    query.addQueryItem(QStringLiteral("$limit"), QStringLiteral("1"));
    const QString pathAndQuery =
        QStringLiteral("/resource/gpe5-46if.json?") + query.query(QUrl::FullyEncoded);

    getJson(url(QStringLiteral("https://publicreporting.cftc.gov"), pathAndQuery),
            [this, instrument](bool ok, const QJsonDocument &doc) {
                if (!ok || !doc.isArray() || doc.array().isEmpty()) {
                    Q_EMIT providerError(QStringLiteral("CFTC-COT: no report for the instrument"));
                    return;
                }
                const QJsonObject record = doc.array().at(0).toObject();
                const QString dateText =
                    record.value(QStringLiteral("report_date_as_yyyy_mm_dd")).toString().left(10);
                const QDate reportDate = QDate::fromString(dateText, QStringLiteral("yyyy-MM-dd"));
                if (!reportDate.isValid()) {
                    Q_EMIT providerError(QStringLiteral("CFTC-COT: unparsable report date"));
                    return;
                }
                // ABOUT the Tuesday close; KNOWN only the following Friday release (~3:30pm ET).
                const QDateTime eventTime(reportDate, QTime(20, 0), QTimeZone::UTC);
                const QDateTime receivedTime(reportDate.addDays(3), QTime(20, 30), QTimeZone::UTC);

                ProviderResult result;
                result.observations.append(cotObservation(
                    instrument, QStringLiteral("COT-ASSET-MGR-NET"),
                    numberOf(record, "asset_mgr_positions_long")
                        - numberOf(record, "asset_mgr_positions_short"),
                    eventTime, receivedTime));
                result.observations.append(cotObservation(
                    instrument, QStringLiteral("COT-LEV-FUND-NET"),
                    numberOf(record, "lev_money_positions_long")
                        - numberOf(record, "lev_money_positions_short"),
                    eventTime, receivedTime));
                publish(result);
            });
}

} // namespace trading::crowd
