// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/IgSentimentProvider.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace trading::crowd {

namespace {

// IG's client-sentiment marketId for an app instrument. Empty = not covered. These are the
// documented ids for IG's index CFD markets; they are verified live the first time credentials
// are present — an id IG does not know is answered 404 and surfaces as a NAMED providerError,
// never as silent wrong data.
QString igMarketId(const QString &instrument)
{
    if (instrument.startsWith(QStringLiteral("SPX500"))
        || instrument.startsWith(QStringLiteral("SP.24-7"))) {
        return QStringLiteral("SPTRD");    // IG "US 500"
    }
    if (instrument.startsWith(QStringLiteral("NSDQ100"))
        || instrument.startsWith(QStringLiteral("NQ"))) {
        return QStringLiteral("NASDAQ");   // IG "US Tech 100"
    }
    return {};
}

// The session tokens stay valid ~6 h; re-login a little before that so a refresh never rides on
// a token about to expire mid-flight.
constexpr qint64 kSessionMaxAgeSec = qint64{5} * 60 * 60;

QByteArray replyHeaderValue(const QList<QPair<QByteArray, QByteArray>> &headers,
                            const QByteArray &wanted)
{
    for (const auto &[name, value] : headers) {
        if (name.compare(wanted, Qt::CaseInsensitive) == 0) {
            return value;
        }
    }
    return {};
}

} // namespace

IgSentimentProvider::IgSentimentProvider(QObject *parent)
    : CrowdHttpProvider(parent),
      // Environment only as the standalone default; production wiring hands in Config's values
      // (which layer the git-ignored apiKeyEtoro.json under the same variables). Never source.
      m_apiKey(qEnvironmentVariable("TRADINGAPP_IG_API_KEY")),
      m_identifier(qEnvironmentVariable("TRADINGAPP_IG_IDENTIFIER")),
      m_password(qEnvironmentVariable("TRADINGAPP_IG_PASSWORD")),
      m_demo(qEnvironmentVariableIsSet("TRADINGAPP_IG_DEMO"))
{
}

QString IgSentimentProvider::name() const
{
    return QStringLiteral("IG");
}

Source IgSentimentProvider::category() const
{
    return Source::RetailPositioning;
}

bool IgSentimentProvider::isConfigured() const
{
    return !m_apiKey.isEmpty() && !m_identifier.isEmpty() && !m_password.isEmpty();
}

void IgSentimentProvider::setCredentials(const QString &apiKey, const QString &identifier,
                                         const QString &password)
{
    m_apiKey = apiKey;
    m_identifier = identifier;
    m_password = password;
    m_cst.clear();   // new credentials invalidate any cached session
    m_securityToken.clear();
    m_loginTime = QDateTime();
}

void IgSentimentProvider::setDemoAccount(bool demo)
{
    m_demo = demo;
}

void IgSentimentProvider::refresh(const QString &instrument, const QDateTime &now)
{
    if (!isConfigured()) {
        return;   // no account: the provider is simply absent, the app carries on
    }
    const QString marketId = igMarketId(instrument);
    if (marketId.isEmpty()) {
        return;   // IG sentiment does not cover this instrument — nothing to fetch
    }
    const bool sessionFresh = !m_cst.isEmpty() && m_loginTime.isValid()
                              && (m_loginTime.secsTo(now) < kSessionMaxAgeSec);
    if (sessionFresh) {
        fetchSentiment(instrument, marketId, now);
        return;
    }
    // POST /session (VERSION 2): the identifier/password go in the BODY, the key in a header,
    // and the session tokens come back as RESPONSE headers. Not retried — see postJson.
    const QJsonObject login{{QStringLiteral("identifier"), m_identifier},
                            {QStringLiteral("password"), m_password}};
    const RawHeaders headers{{QByteArrayLiteral("X-IG-API-KEY"), m_apiKey.toUtf8()},
                             {QByteArrayLiteral("VERSION"), QByteArrayLiteral("2")}};
    const QString host = m_demo ? QStringLiteral("https://demo-api.ig.com")
                                : QStringLiteral("https://api.ig.com");
    postJson(url(host, QStringLiteral("/gateway/deal/session")), headers,
             QJsonDocument(login).toJson(QJsonDocument::Compact),
             [this, instrument, marketId, now](bool ok, const QJsonDocument & /*doc*/,
                                               const RawHeaders &replyHeaders) {
                 const QByteArray cst = replyHeaderValue(replyHeaders, QByteArrayLiteral("CST"));
                 const QByteArray xst =
                     replyHeaderValue(replyHeaders, QByteArrayLiteral("X-SECURITY-TOKEN"));
                 if (!ok || cst.isEmpty() || xst.isEmpty()) {
                     // Named, but NEVER carrying a credential — a diagnostic must stay safe to log.
                     Q_EMIT providerError(QStringLiteral("IG: login failed (no session tokens)"));
                     return;
                 }
                 m_cst = cst;
                 m_securityToken = xst;
                 m_loginTime = now;
                 fetchSentiment(instrument, marketId, now);
             });
}

void IgSentimentProvider::fetchSentiment(const QString &instrument, const QString &marketId,
                                         const QDateTime &now)
{
    const RawHeaders headers{{QByteArrayLiteral("X-IG-API-KEY"), m_apiKey.toUtf8()},
                             {QByteArrayLiteral("CST"), m_cst},
                             {QByteArrayLiteral("X-SECURITY-TOKEN"), m_securityToken},
                             {QByteArrayLiteral("VERSION"), QByteArrayLiteral("1")}};
    const QString host = m_demo ? QStringLiteral("https://demo-api.ig.com")
                                : QStringLiteral("https://api.ig.com");
    getJson(url(host, QStringLiteral("/gateway/deal/clientsentiment/") + marketId), headers,
            [this, instrument, now](bool ok, const QJsonDocument &doc) {
                const QJsonValue pctLong =
                    doc.object().value(QStringLiteral("longPositionPercentage"));
                if (!ok || !pctLong.isDouble()) {
                    // A failed or unparsable answer also drops the session: the next refresh
                    // logs in again rather than riding tokens the server may have rejected.
                    m_cst.clear();
                    m_securityToken.clear();
                    m_loginTime = QDateTime();
                    Q_EMIT providerError(
                        QStringLiteral("IG: no usable client-sentiment answer"));
                    return;
                }
                Observation obs;
                obs.instrument = instrument;
                obs.source = Source::RetailPositioning;
                obs.sourceName = QStringLiteral("IG");
                obs.seriesId = QStringLiteral("IG-PCT-LONG");
                // A live snapshot: it is ABOUT now and KNOWN now — there is no publication lag.
                obs.eventTime = now;
                obs.receivedTime = now;
                obs.value = pctLong.toDouble();   // 0..100, % of IG clients positioned long
                obs.unit = QStringLiteral("percent");
                obs.valid = true;

                ProviderResult result;
                result.observations.append(obs);
                publish(result);
            });
}

} // namespace trading::crowd
