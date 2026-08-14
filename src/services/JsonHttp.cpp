// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/JsonHttp.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace {

// A desktop-browser User-Agent, needed for the public TradingView/Yahoo endpoints
// (they 403 requests without one — see the read-API notes).
constexpr auto kBrowserUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/125.0 Safari/537.36";

// A failure worth re-issuing an idempotent GET for. Covers 429 rate limits and
// 5xx server hiccups (e.g. the intermittent 500 from the aggregate-portfolio
// endpoint), which typically clear within a second or two.
// Transport-level stalls never carry an HTTP status (it stays 0): the 30s
// transferTimeout abort (OperationCanceledError — nothing else aborts replies
// here), a socket timeout, or the server dropping the connection mid-flight.
// They are as transient as a 5xx, so retry those too instead of letting one
// hung trade/history page kill a whole paged walk.
bool isRetryable(const QNetworkReply *reply, qint32 status)
{
    const QNetworkReply::NetworkError err = reply->error();
    const bool transportRetryable =
        (status == 0)
        && ((err == QNetworkReply::OperationCanceledError)
            || (err == QNetworkReply::TimeoutError)
            || (err == QNetworkReply::RemoteHostClosedError)
            || (err == QNetworkReply::TemporaryNetworkFailureError));
    return transportRetryable || (status == 429) || (status == 500) || (status == 502)
           || (status == 503) || (status == 504);
}

// Seconds to wait before the retry: honour a server-supplied delay (429s, and
// some 503s, carry one); else a short default backoff — a bit longer for a
// rate limit than a server hiccup. Capped, since the rate windows are <= 60 s.
qint32 retryDelaySecs(QNetworkReply *reply, qint32 status)
{
    qint32 waitSecs = reply->rawHeader("Retry-After").toInt();
    if (waitSecs <= 0) {
        waitSecs = reply->rawHeader("RateLimit-Reset").toInt();
    }
    if (waitSecs <= 0) {
        waitSecs = (status == 429) ? 2 : 1;
    }
    return std::min(waitSecs, 65);
}

} // namespace

JsonHttp::JsonHttp(QNetworkAccessManager *nam, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
{
}

void JsonHttp::setBrowserHeaders(QNetworkRequest &req)
{
    const QByteArray userAgent(kBrowserUA);
    const QByteArray acceptJson("application/json");
    req.setRawHeader("User-Agent", userAgent);
    req.setRawHeader("Accept", acceptJson);
}

void JsonHttp::handleReply(QNetworkReply *reply, Handler cb, qint32 retriesLeft)
{
    // Qt::SingleShotConnection (Qt 6): finished() fires at most once per reply, so
    // let Qt disconnect this lambda from `reply` synchronously as part of that one
    // activate() call, rather than leaving the connection "fired but still attached"
    // until reply->deleteLater() below tears it down on a later event-loop turn.
    // Narrows the window in which this connection sits in reply's connection list
    // overlapping with QNetworkAccessManager wiring the SAME reply to its own
    // internal HTTP worker thread — see tools/tsan.supp (issue #20) for the actual
    // race that motivated this; this alone is defence in depth, not a proven fix
    // for a race that lives inside Qt's own (non-instrumented-in-spirit) plumbing.
    static_cast<void>(connect(reply, &QNetworkReply::finished, this,
                               [this, reply, cb = std::move(cb), retriesLeft]() {
        const qint32 status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // Transient failure on an idempotent GET (retries left): wait briefly and
        // re-issue rather than surfacing it (see isRetryable above). Non-GET
        // requests are never auto-retried.
        const bool isGet = reply->operation() == QNetworkAccessManager::GetOperation;
        if (isRetryable(reply, status) && (retriesLeft > 0) && isGet) {
            const qint32 waitSecs = retryDelaySecs(reply, status);
            // Re-issue the same GET with a fresh x-request-id after the delay.
            QNetworkRequest req = reply->request();
            const QByteArray requestId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
            req.setRawHeader("x-request-id", requestId);
            reply->deleteLater();
            QTimer::singleShot((waitSecs * 1000) + 250, this, [this, req, cb, retriesLeft] {
                QNetworkReply *retryReply = m_nam->get(req);
                handleReply(retryReply, cb, retriesLeft - 1);
            });
            return;
        }

        // Only read when the device is open: a transport-level failure (the 30s
        // transferTimeout abort above, host-not-found, connection refused, TLS error)
        // leaves the reply's QIODevice unopened, and readAll() on it logs
        // "QIODevice::read (QNetworkReplyHttpImpl): device not open" to the console.
        // HTTP error responses (4xx/5xx) still carry an open, readable body.
        const QByteArray raw = reply->isOpen() ? reply->readAll() : QByteArray();
        const QString netError = (reply->error() == QNetworkReply::NoError)
                                     ? QString()
                                     : reply->errorString();
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        const bool ok = (reply->error() == QNetworkReply::NoError) && (status >= 200)
                        && (status < 300);
        cb(ok, status, doc, raw, netError);
        reply->deleteLater();
    }, Qt::SingleShotConnection));
}
