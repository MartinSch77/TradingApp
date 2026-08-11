// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/CrowdHttpProvider.h"

#include "services/JsonHttp.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace trading::crowd {

CrowdHttpProvider::CrowdHttpProvider(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)), m_http(new JsonHttp(m_nam, this))
{
}

void CrowdHttpProvider::setEndpointBaseForTesting(const QString &base)
{
    m_testBase = base;
}

void CrowdHttpProvider::refresh(const QString & /*instrument*/, const QDateTime & /*now*/) {}

ProviderResult CrowdHttpProvider::fetch(const QString & /*instrument*/, const QDateTime & /*now*/)
{
    return m_cache;
}

QString CrowdHttpProvider::url(const QString &realHost, const QString &pathAndQuery) const
{
    return (m_testBase.isEmpty() ? realHost : m_testBase) + pathAndQuery;
}

void CrowdHttpProvider::getJson(const QString &fullUrl,
                                const std::function<void(bool, const QJsonDocument &)> &cb)
{
    getJson(fullUrl, RawHeaders(), cb);
}

void CrowdHttpProvider::getJson(const QString &fullUrl, const RawHeaders &headers,
                                const std::function<void(bool, const QJsonDocument &)> &cb)
{
    QNetworkRequest request((QUrl(fullUrl)));
    JsonHttp::setBrowserHeaders(request);
    for (const auto &[name, value] : headers) {
        request.setRawHeader(name, value);
    }
    QNetworkReply *reply = m_nam->get(request);
    // retriesLeft = 2: JsonHttp waits out the server's Retry-After / RateLimit-Reset (or a short
    // backoff) on 429/5xx before re-issuing an idempotent GET, off the GUI thread.
    m_http->handleReply(
        reply,
        [cb](bool ok, qint32 /*status*/, const QJsonDocument &doc, const QByteArray & /*raw*/,
             const QString & /*netError*/) { cb(ok && (doc.isObject() || doc.isArray()), doc); },
        /*retriesLeft=*/2);
}

void CrowdHttpProvider::postJson(const QString &fullUrl, const RawHeaders &headers,
                                 const QByteArray &body,
                                 const std::function<void(bool ok, const QJsonDocument &doc,
                                                          const RawHeaders &replyHeaders)> &cb)
{
    QNetworkRequest request((QUrl(fullUrl)));
    JsonHttp::setBrowserHeaders(request);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    for (const auto &[name, value] : headers) {
        request.setRawHeader(name, value);
    }
    QNetworkReply *reply = m_nam->post(request, body);
    // Handled directly, WITHOUT JsonHttp's retry: a POST is not idempotent (a login that "failed"
    // on a timeout may still have created the session), so a failure is reported, never re-sent.
    static_cast<void>(connect(reply, &QNetworkReply::finished, this, [reply, cb] {
        reply->deleteLater();
        const qint32 status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = (reply->error() == QNetworkReply::NoError) && (status >= 200)
                        && (status < 300);
        cb(ok, QJsonDocument::fromJson(reply->readAll()), reply->rawHeaderPairs());
    }));
}

void CrowdHttpProvider::publish(const ProviderResult &result)
{
    m_cache = result;
    Q_EMIT observationsReady(result.observations);
}

} // namespace trading::crowd
