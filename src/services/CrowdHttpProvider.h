// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_CROWDHTTPPROVIDER_H
#define TRADINGAPP_SERVICES_CROWDHTTPPROVIDER_H

#include "services/CrowdProvider.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <functional>

class JsonHttp;
class QNetworkAccessManager;

// The shared base for the REAL, network-backed crowd providers (REQ-F-039, Phase 3). It carries
// the one async-HTTP pattern the rest of the app already uses — QNetworkAccessManager plus
// JsonHttp, which retries an idempotent GET on 429/5xx with backoff and honours Retry-After — so
// the CFTC and FRED providers add only their URL and their parse, and neither reimplements the
// networking (nor is a clone of the other).
//
// The interface stays the Phase-1 pull: `fetch()` returns the LAST result the async `refresh()`
// cached, and `refresh()` is the non-blocking network call that updates that cache and emits
// `observationsReady`. So a collector can either react to the signal (store on arrival) or poll
// `fetch()`, and the GUI thread is never blocked. A provider with no credentials reports itself
// unavailable rather than failing.
namespace trading::crowd {

class CrowdHttpProvider : public QObject, public ICrowdProvider
{
    Q_OBJECT

public:
    explicit CrowdHttpProvider(QObject *parent = nullptr);
    // No user destructor: m_nam/m_http are QObject-parented to `this`, and QObject already gives a
    // virtual destructor — declaring one here would only pull in the rule-of-five (copy/move are
    // already deleted via QObject and ICrowdProvider).

    // Point the provider's host at one base URL — the tests' in-process MockHttpServer — instead
    // of the real API. Empty (default) keeps the real host. The test seam, mirroring MarketFeeds.
    void setEndpointBaseForTesting(const QString &base);

    // Kick off the async network fetch for `instrument` as of `now` (UTC). Non-blocking; results
    // arrive via observationsReady (and are cached for fetch()). The default no-ops so an
    // unconfigured provider stays quiet.
    virtual void refresh(const QString &instrument, const QDateTime &now);

    // ICrowdProvider: the LAST cached result (what the most recent refresh produced). Sync.
    [[nodiscard]] ProviderResult fetch(const QString &instrument, const QDateTime &now) override;

signals:
    // Fresh observations from a completed refresh — a collector connects this to the store.
    void observationsReady(const QList<trading::crowd::Observation> &observations);
    // A fetch failed after JsonHttp's retries, or a payload could not be parsed. The app stays
    // usable (the provider is simply absent); this is for surfacing state, not for a crash.
    void providerError(const QString &detail);

protected:
    // Raw request headers ("X-IG-API-KEY: …") for the APIs that authenticate per header rather
    // than per query parameter. A plain list, matching QNetworkReply::rawHeaderPairs().
    using RawHeaders = QList<QPair<QByteArray, QByteArray>>;

    // The full URL for `pathAndQuery` against `realHost`, or against the test base when one is
    // set. Keys are appended by the caller, never logged here.
    [[nodiscard]] QString url(const QString &realHost, const QString &pathAndQuery) const;
    // GET `fullUrl` as JSON, off the GUI thread, with JsonHttp's retry/backoff. `cb(ok, doc)`.
    void getJson(const QString &fullUrl,
                 const std::function<void(bool ok, const QJsonDocument &doc)> &cb);
    // The same GET with extra request headers (session tokens, per-header API keys).
    void getJson(const QString &fullUrl, const RawHeaders &headers,
                 const std::function<void(bool ok, const QJsonDocument &doc)> &cb);
    // POST `body` as JSON and hand back the REPLY headers too — IG's login answers with its
    // session tokens as headers, not in the body. Deliberately NOT retried: a POST is not
    // idempotent, and JsonHttp's auto-retry is for GETs only.
    void postJson(const QString &fullUrl, const RawHeaders &headers, const QByteArray &body,
                  const std::function<void(bool ok, const QJsonDocument &doc,
                                           const RawHeaders &replyHeaders)> &cb);
    // Store the result the derived parse produced, and announce it.
    void publish(const ProviderResult &result);

private:
    QNetworkAccessManager *m_nam;
    JsonHttp *m_http;
    QString m_testBase;      // empty = the real host
    ProviderResult m_cache;  // the last refreshed result, for fetch()
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_CROWDHTTPPROVIDER_H
