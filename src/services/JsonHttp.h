#ifndef TRADINGAPP_SERVICES_JSONHTTP_H
#define TRADINGAPP_SERVICES_JSONHTTP_H

#include <QObject>

#include <functional>

class QJsonDocument;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

// Shared JSON-over-HTTP plumbing for every service that talks to a REST API:
// owns the reply lifecycle, parses the JSON body, and auto-retries idempotent
// GETs on transient failures. Extracted from EtoroClient so the web feeds and
// the AI advisor reuse the exact same behaviour instead of duplicating it.
class JsonHttp : public QObject
{
    Q_OBJECT
public:
    // Invoked once per (possibly retried) request with the final outcome.
    using Handler = std::function<void(bool ok, qint32 httpStatus,
                                       const QJsonDocument &doc,
                                       const QByteArray &raw,
                                       const QString &netError)>;

    // `nam` issues any retries; it is not owned and must outlive this object.
    explicit JsonHttp(QNetworkAccessManager *nam, QObject *parent = nullptr);

    // retriesLeft > 0 auto-retries an idempotent GET on a transient failure — HTTP 429
    // or a 5xx server hiccup (500/502/503/504) — waiting out the server's Retry-After /
    // RateLimit-Reset (or a short default backoff) before re-issuing with a fresh
    // x-request-id. Non-GET requests are never auto-retried. Default 0 = no retry.
    void handleReply(QNetworkReply *reply, Handler cb, qint32 retriesLeft = 0);

    // Shared headers for public feeds (TradingView, Yahoo, etorostatic) and the
    // Cloudflare-fronted eToro read endpoints: they 403/stall plain clients, so
    // send a browser User-Agent and a JSON accept.
    static void setBrowserHeaders(QNetworkRequest &req);

private:
    QNetworkAccessManager *m_nam = nullptr;
};

#endif // TRADINGAPP_SERVICES_JSONHTTP_H
