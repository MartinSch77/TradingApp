// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/FredProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>

namespace trading::crowd {

FredProvider::FredProvider(QObject *parent)
    : CrowdHttpProvider(parent),
      m_apiKey(qEnvironmentVariable("TRADINGAPP_FRED_API_KEY"))   // env only, never source
{
}

QString FredProvider::name() const
{
    return QStringLiteral("FRED");
}

Source FredProvider::category() const
{
    return Source::Volatility;
}

bool FredProvider::isConfigured() const
{
    return !m_apiKey.isEmpty();
}

void FredProvider::setApiKey(const QString &key)
{
    m_apiKey = key;
}

void FredProvider::refresh(const QString &instrument, const QDateTime & /*now*/)
{
    if (!isConfigured()) {
        return;   // no key: the provider is simply absent, the app carries on
    }
    // The latest CBOE VIX close (VIXCLS). The key is a query parameter and is never logged.
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("series_id"), QStringLiteral("VIXCLS"));
    query.addQueryItem(QStringLiteral("api_key"), m_apiKey);
    query.addQueryItem(QStringLiteral("file_type"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("sort_order"), QStringLiteral("desc"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
    const QString pathAndQuery =
        QStringLiteral("/fred/series/observations?") + query.query(QUrl::FullyEncoded);

    getJson(url(QStringLiteral("https://api.stlouisfed.org"), pathAndQuery),
            [this, instrument](bool ok, const QJsonDocument &doc) {
                const QJsonArray rows = doc.object().value(QStringLiteral("observations")).toArray();
                if (!ok || rows.isEmpty()) {
                    Q_EMIT providerError(QStringLiteral("FRED: no VIX observation"));
                    return;
                }
                const QJsonObject row = rows.at(0).toObject();
                const QString valueText = row.value(QStringLiteral("value")).toString();
                bool numeric = false;
                const double value = valueText.toDouble(&numeric);
                const QDate date = QDate::fromString(
                    row.value(QStringLiteral("date")).toString(), QStringLiteral("yyyy-MM-dd"));
                // FRED writes "." for a missing print (a holiday) — that is no data, not a zero.
                if (!numeric || !date.isValid()) {
                    Q_EMIT providerError(QStringLiteral("FRED: unusable VIX value"));
                    return;
                }
                // The VIX close is ABOUT the session and KNOWN at that close (~4pm ET ≈ 20:00 UTC).
                const QDateTime closeTime(date, QTime(20, 0), QTimeZone::UTC);

                Observation obs;
                obs.instrument = instrument;
                obs.source = Source::Volatility;
                obs.sourceName = QStringLiteral("FRED");
                obs.seriesId = QStringLiteral("VIX");
                obs.eventTime = closeTime;
                obs.receivedTime = closeTime;
                obs.value = value;
                obs.unit = QStringLiteral("index");
                obs.valid = true;

                ProviderResult result;
                result.observations.append(obs);
                publish(result);
            });
}

} // namespace trading::crowd
