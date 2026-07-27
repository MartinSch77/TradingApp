#include "services/EtoroClient.h"

#include "services/JsonHttp.h"
#include "services/SimulationEngine.h"

#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace {

// Case-insensitive key lookup returning the first present key's value.
QJsonValue pick(const QJsonObject &obj, const QStringList &keys)
{
    for (const QString &want : keys) {
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (it.key().compare(want, Qt::CaseInsensitive) == 0) {
                return it.value();
            }
        }
    }
    return {QJsonValue::Undefined};
}

double numFrom(const QJsonValue &v)
{
    if (v.isDouble()) {
        return v.toDouble();
    }
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        return ok ? d : 0.0;
    }
    return 0.0;
}

// eToro sometimes wraps payloads in {"data": ...}. Unwrap arrays/objects.
QJsonArray asArray(const QJsonDocument &doc, const QStringList &arrayKeys)
{
    if (doc.isArray()) {
        return doc.array();
    }
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonValue nested = pick(root, arrayKeys);
        if (nested.isArray()) {
            return nested.toArray();
        }
        // Look one level down inside "data".
        const QJsonValue data = pick(root, {QStringLiteral("data")});
        if (data.isArray()) {
            return data.toArray();
        }
        if (data.isObject()) {
            const QJsonValue inner = pick(data.toObject(), arrayKeys);
            if (inner.isArray()) {
                return inner.toArray();
            }
        }
    }
    return {};
}

QDateTime timeFrom(const QJsonValue &v)
{
    if (v.isDouble()) {
        // Heuristic: seconds vs milliseconds epoch.
        const auto n = static_cast<qint64>(v.toDouble());
        return (n > 1000000000000LL) ? QDateTime::fromMSecsSinceEpoch(n)
                                     : QDateTime::fromSecsSinceEpoch(n);
    }
    if (v.isString()) {
        QDateTime dt = QDateTime::fromString(v.toString(), Qt::ISODate);
        if (dt.isValid()) {
            return dt;
        }
        dt = QDateTime::fromString(v.toString(), Qt::ISODateWithMs);
        if (dt.isValid()) {
            return dt;
        }
    }
    return QDateTime::currentDateTime();
}

} // namespace

// Accumulates closed-trade P/L across the paged trade-history responses. Held in a
// QSharedPointer so it survives the chain of asynchronous page fetches.
struct PnlAccum {
    QDate minDate;
    qint32 page = 1;
    QHash<QString, InstrumentPnl> bySymbol;  // listed instruments only
    qint32 accountTrades = 0;                // every closed trade in the window
    double accountNet = 0.0;
    double accountFees = 0.0;
    QList<ClosedTrade> trades;               // every trade in the window, newest first
    QSet<qint64> instrumentIds;              // distinct instruments traded (for spreads)
};

// Per-run state for the leverage screener. Held in a QSharedPointer so it survives
// the bulk-leverage call and the ensuing chain of sequential candle fetches.
struct ScanItem {
    qint64 id = 0;
    QString symbol;
};
struct ScanState {
    QList<ScanItem> queue;             // instruments still to fetch candles for
    qint32 index = 0;                  // next queue entry to fetch
    qint32 total = 0;                  // queue size at start (for progress)
    QHash<qint64, qint32> maxLevById;  // instrumentId -> max CFD leverage from eligibility
};

EtoroClient::EtoroClient(Config config, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
    , m_nam(new QNetworkAccessManager(this))
    , m_http(new JsonHttp(m_nam, this))
    , m_sim(new SimulationEngine(this))
    , m_pollTimer(new QTimer(this))
{
    // Abort any request that stalls with no data for 30s so its finished() always
    // fires: without this a hung reply (e.g. a Cloudflare-blocked trade/history call)
    // never invokes handleReply's callback, leaving guards like m_pnlFetching stuck
    // true forever. The AI request overrides this with a longer per-request timeout.
    m_nam->setTransferTimeout(std::chrono::seconds{30});
    

    // The simulation engine implements the same operations against a synthetic
    // feed; its signals are forwarded unchanged so the UI sees one client API in
    // both modes. The price forward also keeps m_lastPrice in sync.
    
    static_cast<void>(connect(m_sim, &SimulationEngine::historyReady,
                              this, &EtoroClient::historyReady));
    static_cast<void>(connect(m_sim, &SimulationEngine::priceUpdated, this,
                              [this](const QDateTime &time, double price) {
                                  m_lastPrice = price;
                                  emit priceUpdated(time, price);
                              }));
    static_cast<void>(connect(m_sim, &SimulationEngine::portfolioUpdated,
                              this, &EtoroClient::portfolioUpdated));
    static_cast<void>(connect(m_sim, &SimulationEngine::cashUpdated,
                              this, &EtoroClient::cashUpdated));
    static_cast<void>(connect(m_sim, &SimulationEngine::orderResult,
                              this, &EtoroClient::orderResult));
    static_cast<void>(connect(m_sim, &SimulationEngine::positionClosed,
                              this, &EtoroClient::positionClosed));
    static_cast<void>(connect(m_sim, &SimulationEngine::leverageOptions,
                              this, &EtoroClient::leverageOptions));
    static_cast<void>(connect(m_sim, &SimulationEngine::monthlyPnlReady,
                              this, &EtoroClient::monthlyPnlReady));
    static_cast<void>(connect(m_sim, &SimulationEngine::screenerRow,
                              this, &EtoroClient::screenerRow));
    static_cast<void>(connect(m_sim, &SimulationEngine::screenerProgress,
                              this, &EtoroClient::screenerProgress));
    static_cast<void>(connect(m_sim, &SimulationEngine::screenerFinished, this, [this] {
        m_scanActive = false;
        emit screenerFinished();
    }));
    static_cast<void>(connect(m_sim, &SimulationEngine::log, this, &EtoroClient::log));

    
    m_pollTimer->setInterval(m_config.pollIntervalMs);
    static_cast<void>(connect(m_pollTimer, &QTimer::timeout, this, &EtoroClient::onPollTimeout));
}

void EtoroClient::start()
{
    if (!m_config.hasCredentials()) {
        startSimulation();
        return;
    }
    m_simulated = false;
    emit log(QStringLiteral("Connecting to eToro API in %1 mode…").arg(m_config.modeLabel()), false);
    resolveInstrumentReal();
    resolveListedInstrumentIds();
}

void EtoroClient::setTradableSymbols(const QStringList &symbols)
{
    // Just store; start() (real mode) kicks off id resolution. This is called
    // during UI construction, before start(), so no resolution is needed here.
    m_tradableSymbols = symbols;
}

void EtoroClient::resolveListedInstrumentIds()
{
    // Learn each tradable symbol's instrumentId via the search endpoint (its
    // internalSymbolFull field matches the app's symbols, incl. the .24-7 variants),
    // so the account-wide portfolio can be restricted to these instruments and named.
    const QList<QString> known = m_symbolById.values();
    for (const QString &sym : std::as_const(m_tradableSymbols)) {
        if (known.contains(sym)) {
            continue;
        }
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("internalSymbolFull"), sym);
        q.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("instrumentId,internalSymbolFull"));
        q.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("50"));
        QNetworkReply *reply = apiGet(QStringLiteral("/v1/market-data/search"), q);
        handleReply(reply, [this, sym](bool ok, qint32, const QJsonDocument &doc,
                                        const QByteArray &, const QString &) {
            if (!ok) {
                return;
            }
            const QJsonArray items =
                asArray(doc, {QStringLiteral("items"), QStringLiteral("results")});
            for (const auto &v : items) {
                const QJsonObject o = v.toObject();
                const QString s = pick(o, {QStringLiteral("internalSymbolFull"),
                                           QStringLiteral("symbolFull"),
                                           QStringLiteral("symbol")}).toString();
                const qint64 id = static_cast<qint64>(
                    numFrom(pick(o, {QStringLiteral("instrumentId"), QStringLiteral("id")})));
                if (id <= 0) {
                    continue;
                }
                // The search filters by internalSymbolFull but usually does NOT echo
                // the symbol back, returning the primary instrument first. So accept an
                // exact symbol match when present, otherwise take the first valid id.
                if (s.isEmpty() || (s.compare(sym, Qt::CaseInsensitive) == 0)) {
                    static_cast<void>(m_symbolById.insert(id, sym));
                    static_cast<void>(m_idBySymbol.insert(sym, id));
                    return;
                }
            }
        });
    }
}

void EtoroClient::setSymbol(const QString &symbol)
{
    const QString s = symbol.trimmed();
    if (s.isEmpty()) {
        return;
    }
    // Re-selecting the already-resolved instrument is a no-op — but if the last
    // resolution failed (m_instrument was cleared when the switch started), fall
    // through so the user can retry by picking the same symbol again.
    if ((s.compare(m_config.symbol, Qt::CaseInsensitive) == 0) && m_instrument.isValid()) {
        return;
    }

    m_config.symbol = s;
    m_resolveRetries = 0;

    // Tear down the current instrument's data pipeline and rebuild it for the new
    // one. Portfolio/cash are account-wide, so they carry over untouched in real
    // mode; the simulation restarts from a fresh synthetic feed.
    m_pollTimer->stop();
    m_pollCount = 0;
    m_instrument = Instrument{};
    m_lastPrice = 0.0;
    // Also drop the two-sided quote: the opening-cost display must show "awaiting
    // live bid/ask" rather than costs computed from the previous instrument's spread.
    m_lastBid = 0.0;
    m_lastAsk = 0.0;

    emit log(QStringLiteral("Switching instrument to %1…").arg(s), false);

    if (!m_config.hasCredentials()) {
        startSimulation(/*resetAccount=*/false);  // keep trades on other instruments
    } else {
        m_simulated = false;
        resolveInstrumentReal();
    }
}

void EtoroClient::onPollTimeout()
{
    // EUR/USD for display conversion — moves slowly, ~every 20s. Real mode only: it
    // needs an authenticated rates call, and simulation shows the raw account currency.
    if (!m_simulated) {
        if ((m_fxTick % 20) == 0) {
            fetchEurUsd();
        }
        ++m_fxTick;
    }

    if (m_simulated) {
        m_sim->tick();
    } else {
        pollPriceReal();  // price every tick
        // Portfolio + balance are heavier and share the default rate-limit pool,
        // so refresh them roughly every 3rd tick instead of every tick.
        if ((m_pollCount++ % 3) == 0) {
            refreshPortfolioReal();
            refreshBalanceReal();
        }
        // Which markets are open changes only at session boundaries, so re-check
        // tradeability sparingly (~every 60 ticks) on the eligibility endpoint's own
        // rate pool. The tick advances only once ids are resolved, so the first check
        // fires as soon as they land rather than being skipped for a full window.
        if (!m_symbolById.isEmpty()) {
            if ((m_tradeTick % 60) == 0) {
                refreshTradeabilityReal();
            }
            ++m_tradeTick;
        }
    }
}

void EtoroClient::openPosition(bool isBuy, double amount, double leverage, double stopLossAmount,
                               double takeProfitAmount, bool trailingStop)
{
    if (amount <= 0.0) {
        emit orderResult(false, QStringLiteral("Amount must be greater than zero."));
        return;
    }
    if (m_simulated) {
        m_sim->openPosition(isBuy, amount, leverage, stopLossAmount, takeProfitAmount,
                            trailingStop);
    } else {
        openPositionReal(isBuy, amount, leverage, stopLossAmount, takeProfitAmount, trailingStop);
    }
}

void EtoroClient::closePosition(const QString &positionId)
{
    if (positionId.isEmpty()) {
        emit positionClosed(false, QStringLiteral("No position selected."));
        return;
    }
    if (m_simulated) {
        m_sim->closePosition(positionId);
    } else {
        closePositionReal(positionId);
    }
}

void EtoroClient::modifyPosition(const QString &positionId, double stopLossRate,
                                 double takeProfitRate, bool trailingStop)
{
    if (positionId.isEmpty()) {
        return;
    }
    if (m_simulated) {
        m_sim->modifyPosition(positionId, stopLossRate, takeProfitRate, trailingStop);
    } else {
        modifyPositionReal(positionId, stopLossRate, takeProfitRate, trailingStop);
    }
}

void EtoroClient::fetchMonthlyPnl()
{
    fetchClosedTrades(7);
}

void EtoroClient::fetchClosedTrades(qint32 weeksBack)
{
    if (m_simulated) {
        m_sim->summarizeMonthly();
        emit closedTradesReady({});  // the simulation keeps no per-trade history
        return;
    }
    // Ignore overlapping requests: the walk pages the shared-pool history endpoint,
    // so stacking runs (e.g. repeated Refresh clicks) would only burn the rate budget.
    if (m_pnlFetching) {
        emit log(QStringLiteral("Closed-trade P/L refresh already in progress…"), false);
        return;
    }
    m_pnlFetching = true;
    auto acc = QSharedPointer<PnlAccum>::create();
    const qint32 weeks = std::clamp(weeksBack, 1, 26);
    acc->minDate = QDate::currentDate().addDays(-7LL * weeks);
    fetchTradeHistoryPageReal(acc);
}

// ===========================================================================
// Shared REST plumbing
// ===========================================================================

QNetworkRequest EtoroClient::makeRequest(const QUrl &url) const
{
    QNetworkRequest req(url);
    const QByteArray apiKey = m_config.apiKey.toUtf8();
    const QByteArray userKey = m_config.userKey.toUtf8();
    const QByteArray requestId = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
    req.setRawHeader("x-api-key", apiKey);
    req.setRawHeader("x-user-key", userKey);
    req.setRawHeader("x-request-id", requestId);
    // Some eToro read endpoints (notably trade/history) sit behind Cloudflare, which
    // stalls/403s requests without a browser User-Agent — the request would then never
    // finish and pin m_pnlFetching, so the closed-trade panel hangs on "Loading…".
    JsonHttp::setBrowserHeaders(req);
    return req;
}

QNetworkReply *EtoroClient::apiGet(const QString &path, const QUrlQuery &query)
{
    QUrl url(m_config.baseUrl + path);
    if (!query.isEmpty()) {
        url.setQuery(query);
    }
    return m_nam->get(makeRequest(url));
}

QNetworkReply *EtoroClient::apiPost(const QString &path, const QJsonObject &body)
{
    QNetworkRequest req = makeRequest(QUrl(m_config.baseUrl + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return m_nam->post(req, payload);
}

QNetworkReply *EtoroClient::apiPatch(const QString &path, const QJsonObject &body)
{
    QNetworkRequest req = makeRequest(QUrl(m_config.baseUrl + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return m_nam->sendCustomRequest(req, "PATCH", payload);
}

void EtoroClient::handleReply(QNetworkReply *reply, JsonHandler cb, qint32 retriesLeft)
{
    m_http->handleReply(reply, std::move(cb), retriesLeft);
}

QString EtoroClient::accountSegment() const
{
    return m_config.isLive() ? QString() : QStringLiteral("/demo");
}

// ===========================================================================
// Real mode
// ===========================================================================

void EtoroClient::resolveInstrumentReal()
{
    // NOTE: the search endpoint ignores a free-text "query" param (it returns
    // the full instrument list). Filtering on "internalSymbolFull" is what
    // actually narrows the result to the wanted symbol.
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("internalSymbolFull"), m_config.symbol);
    q.addQueryItem(QStringLiteral("fields"),
                   QStringLiteral("instrumentId,displayname,internalSymbolFull,currentRate"));
    q.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("50"));

    QNetworkReply *reply = apiGet(QStringLiteral("/v1/market-data/search"), q);
    const QString wantSym = m_config.symbol;
    handleReply(reply, [this, wantSym](bool ok, qint32 status, const QJsonDocument &doc,
                                       const QByteArray &raw, const QString &netError) {
        // A stale reply for a previously selected symbol must not overwrite the
        // current selection (the user may switch instruments faster than the
        // search round-trips).
        if (wantSym.compare(m_config.symbol, Qt::CaseInsensitive) != 0) {
            return;
        }
        if (!ok) {
            emit log(QStringLiteral("Instrument search failed (HTTP %1): %2")
                         .arg(status)
                         .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(300)) : netError),
                     true);
            retryResolveOrGiveUp(wantSym);
            return;
        }
        const QJsonArray items =
            asArray(doc, {QStringLiteral("items"), QStringLiteral("results")});
        Instrument found;
        for (const auto &v : items) {
            const QJsonObject o = v.toObject();
            const QString sym = pick(o, {QStringLiteral("internalSymbolFull"),
                                         QStringLiteral("symbolFull"),
                                         QStringLiteral("symbol")}).toString();
            Instrument inst;
            inst.instrumentId = static_cast<qint64>(
                numFrom(pick(o, {QStringLiteral("instrumentId"), QStringLiteral("id")})));
            inst.symbol = sym;
            inst.displayName = pick(o, {QStringLiteral("displayname"),
                                        QStringLiteral("displayName"),
                                        QStringLiteral("internalInstrumentDisplayName")})
                                   .toString();
            inst.currentRate =
                numFrom(pick(o, {QStringLiteral("currentRate"), QStringLiteral("rate")}));
            if (inst.instrumentId <= 0) {
                continue;  // skip the "-100000" placeholder / header row
            }
            if (sym.compare(wantSym, Qt::CaseInsensitive) == 0) {
                found = inst;
                break;  // exact symbol match wins (e.g. SPX500 over SPX500.FUT)
            }
            if (!found.isValid()) {
                found = inst;  // fall back to first real result
            }
        }

        if (!found.isValid()) {
            emit log(QStringLiteral("Could not resolve instrument '%1' from search response.")
                         .arg(wantSym),
                     true);
            retryResolveOrGiveUp(wantSym);
            return;
        }
        if (found.symbol.isEmpty()) {
            found.symbol = wantSym;  // search does not always echo internalSymbolFull
        }
        m_instrument = found;
        static_cast<void>(
            m_symbolById.insert(m_instrument.instrumentId, wantSym));  // always mapped
        static_cast<void>(m_idBySymbol.insert(wantSym, m_instrument.instrumentId));
        if (found.currentRate > 0.0) {
            m_lastPrice = found.currentRate;
        }
        emit log(QStringLiteral("Resolved %1 -> instrumentId %2 (%3)")
                     .arg(wantSym)
                     .arg(m_instrument.instrumentId)
                     .arg(m_instrument.displayName),
                 false);
        emit ready(m_instrument);

        // Show account state right away rather than waiting for the first poll.
        refreshBalanceReal();
        refreshPortfolioReal();
        fetchHistoryReal();
        fetchLeverageReal();
        fetchFeesReal();
        refreshPortfolioReal();
        m_pollTimer->start();
    });
}

void EtoroClient::retryResolveOrGiveUp(const QString &symbol)
{
    if (m_resolveRetries < 2) {
        ++m_resolveRetries;
        emit log(QStringLiteral("Retrying instrument resolution for %1 (attempt %2)…")
                     .arg(symbol)
                     .arg(m_resolveRetries + 1),
                 false);
        QTimer::singleShot(4000, this, [this, symbol]() {
            if ((symbol.compare(m_config.symbol, Qt::CaseInsensitive) == 0)
                && !m_instrument.isValid()) {
                resolveInstrumentReal();
            }
        });
        return;
    }
    emit resolveFailed(symbol);
}

void EtoroClient::fetchLeverageReal()
{
    if (m_instrument.instrumentId <= 0) {
        return;
    }

    // Per-instrument trading configuration for this account; leverageConfigs holds
    // the allowed multipliers by settlement type/direction.
    QJsonObject body;
    body[QStringLiteral("instrumentIds")] =
        QJsonArray{static_cast<qint32>(m_instrument.instrumentId)};
    body[QStringLiteral("currency")] = QStringLiteral("USD");  // API supports USD only

    const QString path = QStringLiteral("/v2/trading/info%1/eligibility").arg(accountSegment());
    QNetworkReply *reply = apiPost(path, body);
    handleReply(reply, [this](bool ok, qint32 status, const QJsonDocument &doc,
                              const QByteArray &raw, const QString &netError) {
        if (!ok) {
            emit log(QStringLiteral("Leverage lookup failed (HTTP %1): %2 — keeping current options.")
                         .arg(status)
                         .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(200)) : netError),
                     false);
            return;
        }
        const QJsonArray elig = doc.object().value(QStringLiteral("eligibilities")).toArray();
        if (elig.isEmpty()) {
            return;
        }
        const QJsonObject e = elig.first().toObject();

        // The same response carries the per-order unit cap eToro enforces at
        // execution (e.g. 20 units for GOLD) — remember it so an oversized order
        // can be rejected before submission with an actionable message. Match the
        // echoed id so a stale reply after a quick instrument switch can't attach
        // the previous instrument's cap to the new one.
        const qint64 respId =
            static_cast<qint64>(numFrom(pick(e, {QStringLiteral("instrumentId")})));
        if (respId == m_instrument.instrumentId) {
            m_instrument.maxUnitsPerOrder =
                numFrom(pick(e, {QStringLiteral("maxUnitsPerOrder")}));
        }

        // Prefer CFD configs (how this app trades); fall back to all if none.
        QList<qint32> cfd;
        QList<qint32> all;
        const QJsonArray leverageConfigs = e.value(QStringLiteral("leverageConfigs")).toArray();
        for (const auto &lcv : leverageConfigs) {
            const QJsonObject lc = lcv.toObject();
            const bool isCfd = lc.value(QStringLiteral("settlementType"))
                                   .toString()
                                   .compare(QLatin1String("cfd"), Qt::CaseInsensitive) == 0;
            const QJsonArray leverageValues = lc.value(QStringLiteral("leverageValues")).toArray();
            for (const auto &lv : leverageValues) {
                const qint32 v = lv.toInt();
                if (v <= 0) {
                    continue;
                }
                all.append(v);
                if (isCfd) {
                    cfd.append(v);
                }
            }
        }
        QList<qint32> values = cfd.isEmpty() ? all : cfd;
        const auto sortBegin = values.begin();
        const auto sortEnd = values.end();
        std::sort(sortBegin, sortEnd);
        const auto uniqueEnd = std::unique(sortBegin, sortEnd);
        static_cast<void>(values.erase(uniqueEnd, sortEnd));
        if (!values.isEmpty()) {
            emit leverageOptions(values);
        }
    });
}

void EtoroClient::fetchFeesReal()
{
    if (m_instrument.instrumentId <= 0) {
        return;
    }

    // Public, unauthenticated trade-config feed used by the eToro web app; carries
    // the per-unit overnight/weekend rollover fees the trade ticket displays.
    // Not part of the rate-limited public API — a browser User-Agent suffices.
    const qint64 wantId = m_instrument.instrumentId;
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://api.etorostatic.com/sapi/trade-real/instruments/%1").arg(wantId)));
    JsonHttp::setBrowserHeaders(req);
    QNetworkReply *reply = m_nam->get(req);
    handleReply(reply, [this, wantId](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (!ok || (m_instrument.instrumentId != wantId)) {
            return;  // transient failure, or the user switched instruments meanwhile
        }
        const QJsonObject inst = doc.object().value(QStringLiteral("Instrument")).toObject();
        InstrumentFees fees;
        fees.buyOvernight = inst.value(QStringLiteral("BuyOverNightFee")).toDouble();
        fees.sellOvernight = inst.value(QStringLiteral("SellOverNightFee")).toDouble();
        fees.buyWeekend = inst.value(QStringLiteral("BuyEndOfWeekFee")).toDouble();
        fees.sellWeekend = inst.value(QStringLiteral("SellEndOfWeekFee")).toDouble();
        if (fees.isValid()) {
            static_cast<void>(m_feesById.insert(wantId, fees));  // shared cache (see feesFor/requestFees)
            emit feesUpdated(fees);
        }
    });
}

double EtoroClient::spreadPctFor(const QString &symbol) const
{
    return m_spreadPctById.value(m_idBySymbol.value(symbol, 0), 0.0);
}

InstrumentFees EtoroClient::feesFor(const QString &symbol) const
{
    return m_feesById.value(m_idBySymbol.value(symbol, 0), InstrumentFees{});
}

void EtoroClient::requestFees(const QString &symbol)
{
    if (m_simulated) {
        return;  // no fee feed in simulation — the plan flags its bill as partial
    }
    const qint64 id = m_idBySymbol.value(symbol, 0);
    if ((id <= 0) || m_feesById.contains(id) || m_feesInFlight.contains(id)) {
        if (m_feesById.contains(id)) {
            emit instrumentFeesUpdated(symbol, m_feesById.value(id));
        }
        return;
    }
    static_cast<void>(m_feesInFlight.insert(id));
    // Same public trade-config feed as fetchFeesReal, keyed to any listed symbol.
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://api.etorostatic.com/sapi/trade-real/instruments/%1").arg(id)));
    JsonHttp::setBrowserHeaders(req);
    QNetworkReply *reply = m_nam->get(req);
    handleReply(reply, [this, id, symbol](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                          const QByteArray & /*raw*/, const QString & /*netError*/) {
        static_cast<void>(m_feesInFlight.remove(id));
        if (!ok) {
            return;  // transient — the next plan render will re-request
        }
        const QJsonObject inst = doc.object().value(QStringLiteral("Instrument")).toObject();
        InstrumentFees fees;
        fees.buyOvernight = inst.value(QStringLiteral("BuyOverNightFee")).toDouble();
        fees.sellOvernight = inst.value(QStringLiteral("SellOverNightFee")).toDouble();
        fees.buyWeekend = inst.value(QStringLiteral("BuyEndOfWeekFee")).toDouble();
        fees.sellWeekend = inst.value(QStringLiteral("SellEndOfWeekFee")).toDouble();
        if (fees.isValid()) {
            static_cast<void>(m_feesById.insert(id, fees));
            emit instrumentFeesUpdated(symbol, fees);
        }
    });
}

void EtoroClient::refreshTradeabilityReal()
{
    // Which markets are open *now*. eToro's public API exposes no live session flag
    // (eligibility.allowOpenPosition is a static account permission — it stays true for
    // e.g. SPX500 all weekend), so we infer it from quote freshness: while a market is
    // open eToro re-stamps the rate's `date` continuously (age ≈ 0s); once it closes the
    // price freezes and `date` goes stale (hours over a weekend, ~1h in the daily index
    // break). A quote fresher than the threshold ⇒ the market is open. One bulk rates
    // call (≤100 ids) covers every resolved instrument.
    if (m_symbolById.isEmpty()) {
        return;
    }

    QStringList ids;
    for (auto it = m_symbolById.constBegin(); it != m_symbolById.constEnd(); ++it) {
        ids << QString::number(it.key());
    }

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("instrumentIds"), ids.join(QLatin1Char(',')));
    QNetworkReply *reply = apiGet(QStringLiteral("/v1/market-data/instruments/rates"), q);
    handleReply(reply, [this](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                              const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (!ok) {
            return;  // transient — keep the last known open/closed set
        }

        // A quote older than this counts as a closed market. Open eToro CFDs re-stamp
        // `date` every few seconds, so this sits far above any intra-session gap yet
        // well below the ~1h daily index break and the multi-hour weekend.
        constexpr qint64 kQuoteFreshSecs = 300;
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

        QJsonArray arr = asArray(doc, {QStringLiteral("rates"), QStringLiteral("data")});
        if (arr.isEmpty() && doc.isObject()) {
            arr.append(doc.object());
        }
        QSet<QString> open;
        for (const auto &v : std::as_const(arr)) {
            const QJsonObject rate = v.toObject();
            const qint64 id = static_cast<qint64>(
                numFrom(pick(rate, {QStringLiteral("instrumentID"), QStringLiteral("instrumentId")})));
            const QString sym = m_symbolById.value(id);
            if (sym.isEmpty()) {
                continue;
            }
            // Same bulk snapshot also keeps the per-instrument spread cache warm,
            // so the decision window can cost a plan for ANY listed instrument.
            const double bid =
                numFrom(pick(rate, {QStringLiteral("bid"), QStringLiteral("cvtBid")}));
            const double ask =
                numFrom(pick(rate, {QStringLiteral("ask"), QStringLiteral("cvtAsk")}));
            if ((bid > 0.0) && (ask > bid)) {
                static_cast<void>(
                    m_spreadPctById.insert(id, ((ask - bid) / ((ask + bid) / 2.0)) * 100.0));
            }
            const QDateTime quoteTime = QDateTime::fromString(
                pick(rate, {QStringLiteral("date")}).toString(), Qt::ISODate);
            // Fail open: a missing/unparsable timestamp must not falsely block trading.
            const bool fresh = !quoteTime.isValid()
                               || (quoteTime.secsTo(nowUtc) < kQuoteFreshSecs);
            if (fresh) {
                static_cast<void>(open.insert(sym));
            }
        }
        m_freshQuoteSymbols = open;
        emit tradeabilityUpdated(open);
    }, /*retriesLeft=*/1);  // ride out a transient 429 on the shared market-data pool
}

void EtoroClient::fetchCandles(const QString &interval, qint32 count,
                               std::function<void(QList<Candle>)> cb)
{
    const QString path = QStringLiteral("/v1/market-data/instruments/%1/history/candles/%2/%3/%4")
                             .arg(m_instrument.instrumentId)
                             .arg(m_candleDirection, interval)
                             .arg(count);
    QNetworkReply *reply = apiGet(path, QUrlQuery());
    handleReply(reply, [this, cb = std::move(cb), interval](bool ok, qint32 status, const QJsonDocument &doc,
                                            const QByteArray &raw, const QString &netError) {
        if (!ok) {
            emit log(QStringLiteral("Candle history (%1) unavailable (HTTP %2): %3")
                         .arg(interval)
                         .arg(status)
                         .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(200)) : netError),
                     true);
            cb({});
            return;
        }
        QJsonArray arr = asArray(doc, {QStringLiteral("candles"), QStringLiteral("data")});
        // eToro nests one level: { candles: [ { instrumentId, candles: [ {ohlc} ] } ] }
        const QJsonValue firstEntry = arr.isEmpty() ? QJsonValue() : arr.first();
        if (firstEntry.isObject()) {
            const QJsonObject firstObj = firstEntry.toObject();
            const QJsonValue inner = pick(firstObj, {QStringLiteral("candles")});
            if (inner.isArray()) {
                arr = inner.toArray();
            }
        }
        QList<Candle> candles;
        candles.reserve(arr.size());
        for (const auto &v : std::as_const(arr)) {
            const QJsonObject o = v.toObject();
            Candle c;
            c.timestamp = timeFrom(pick(o, {QStringLiteral("fromDate"),
                                            QStringLiteral("timestamp"),
                                            QStringLiteral("date")}));
            c.open = numFrom(pick(o, {QStringLiteral("open")}));
            c.high = numFrom(pick(o, {QStringLiteral("high")}));
            c.low = numFrom(pick(o, {QStringLiteral("low")}));
            c.close = numFrom(pick(o, {QStringLiteral("close"), QStringLiteral("rate"),
                                       QStringLiteral("mid")}));
            if (c.close > 0.0) {
                candles << c;
            }
        }
        const auto sortBegin = candles.begin();
        const auto sortEnd = candles.end();
        std::sort(sortBegin, sortEnd,
                  [](const Candle &a, const Candle &b) { return a.timestamp < b.timestamp; });
        cb(candles);
    });
}

void EtoroClient::fetchHistoryReal()
{
    // eToro caps a candle request at 1000 with no date paging, so 1-minute reaches
    // only ~17h. To get BOTH a month of context AND the fastest 1-minute detail, seed
    // from two resolutions: hourly for the older month, then splice the recent ~1000
    // one-minute candles onto the end. Short-lookback signals then land entirely in
    // the 1-minute region.
    const qint64 wantId = m_instrument.instrumentId;  // guard against an instrument switch
    fetchCandles(QStringLiteral("OneHour"), 720, [this, wantId](const QList<Candle> &hourly) {
        if (m_instrument.instrumentId != wantId) {
            return;
        }
        fetchCandles(QStringLiteral("OneMinute"), 1000,
                     [this, wantId, hourly](const QList<Candle> &minute) mutable {
            if (m_instrument.instrumentId != wantId) {
                return;
            }
            QList<Candle> merged;
            if (!minute.isEmpty()) {
                const QDateTime cut = minute.first().timestamp;  // oldest 1-minute candle
                for (const Candle &c : hourly) {
                    if (c.timestamp < cut) {
                        merged << c;   // coarse context older than the fine window
                    }
                }
                merged += minute;      // fine, fast detail up to "now"
            } else {
                merged = hourly;       // fall back to the month if 1-minute failed
            }
            if (!merged.isEmpty()) {
                m_lastPrice = merged.last().close;
                emit historyReady(merged);
            }
        });
    });
}

void EtoroClient::pollPriceReal()
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("instrumentIds"), QString::number(m_instrument.instrumentId));
    QNetworkReply *reply = apiGet(QStringLiteral("/v1/market-data/instruments/rates"), q);
    const qint64 wantId = m_instrument.instrumentId;  // guard against an instrument switch
    handleReply(reply, [this, wantId](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (!ok) {
            return;  // transient; keep the previous price
        }
        if (m_instrument.instrumentId != wantId) {
            return;  // stale reply from before a switch — not this instrument's quote
        }
        // Response may be an array of rate objects or {rates:[...]} / {data:[...]}.
        QJsonObject rate;
        const QJsonArray arr =
            asArray(doc, {QStringLiteral("rates"), QStringLiteral("data")});
        if (!arr.isEmpty()) {
            rate = arr.first().toObject();
        } else if (doc.isObject()) {
            rate = doc.object();
        } else {
            // no usable payload shape — the id check below rejects the empty object
        }
        // The payload echoes the instrument id — reject a mismatched row outright.
        const qint64 gotId = static_cast<qint64>(
            numFrom(pick(rate, {QStringLiteral("instrumentID"), QStringLiteral("instrumentId")})));
        if ((gotId > 0) && (gotId != wantId)) {
            return;
        }
        // Keep the bid/ask so orders can price SL/TP off the side the position will
        // actually open at (buy→ask, sell→bid); the mid alone skews the SL by the
        // half-spread, which on a wide-spread CFD is a visible mismatch vs the set amount.
        const double bid = numFrom(pick(rate, {QStringLiteral("bid"), QStringLiteral("cvtBid")}));
        const double ask = numFrom(pick(rate, {QStringLiteral("ask"), QStringLiteral("cvtAsk")}));
        if (bid > 0.0) {
            m_lastBid = bid;
        }
        if (ask > 0.0) {
            m_lastAsk = ask;
        }

        double price = numFrom(pick(rate, {QStringLiteral("lastExecution"),
                                            QStringLiteral("currentRate"), QStringLiteral("close"),
                                            QStringLiteral("mid"), QStringLiteral("last")}));
        if ((price <= 0.0) && (bid > 0.0) && (ask > 0.0)) {
            price = (bid + ask) / 2.0;
        }
        if (price > 0.0) {
            m_lastPrice = price;
            emit priceUpdated(QDateTime::currentDateTime(), price);
        }
    });
}

void EtoroClient::fetchEurUsd()
{
    // EURUSD is instrument 1; its quote is USD per 1 EUR, so the reciprocal of the
    // mid is EUR per 1 USD — the factor used to show USD account figures in euro.
    // Keep the last value on a transient failure (display just stays a touch stale).
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("instrumentIds"), QStringLiteral("1"));
    QNetworkReply *reply = apiGet(QStringLiteral("/v1/market-data/instruments/rates"), q);
    handleReply(reply, [this](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                              const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (!ok) {
            return;
        }
        QJsonArray arr = asArray(doc, {QStringLiteral("rates"), QStringLiteral("data")});
        if (arr.isEmpty() && doc.isObject()) {
            arr.append(doc.object());
        }
        if (arr.isEmpty()) {
            return;
        }
        const QJsonObject r = arr.first().toObject();
        const double bid = numFrom(pick(r, {QStringLiteral("bid")}));
        const double ask = numFrom(pick(r, {QStringLiteral("ask")}));
        const double usdPerEur = ((bid > 0.0) && (ask > 0.0))
                                     ? ((bid + ask) / 2.0)
                                     : numFrom(pick(r, {QStringLiteral("lastExecution")}));
        if (usdPerEur <= 0.0) {
            return;
        }
        m_eurPerUsd = 1.0 / usdPerEur;
        emit fxRateUpdated(m_eurPerUsd);
    }, /*retriesLeft=*/2);
}

void EtoroClient::refreshPortfolioReal()
{
    // Use the /pnl breakdown (same shape as /portfolio) rather than /portfolio: it
    // carries eToro's own live per-position unrealised P/L (unrealizedPnL.pnL), which
    // already reflects the closing spread and any fees. The old code derived P/L from
    // the mid rate, understating it vs eToro (a long is marked at the bid, not mid).
    // NB: unlike /portfolio (real = no segment), /pnl needs an explicit real|demo
    // segment — /v1/trading/info/real/pnl — so accountSegment() ("" for real) is wrong.
    const QString path = QStringLiteral("/v1/trading/info/%1/pnl")
                             .arg(m_config.isLive() ? QStringLiteral("real")
                                                    : QStringLiteral("demo"));
    QNetworkReply *reply = apiGet(path, QUrlQuery());
    handleReply(reply, [this](bool ok, qint32 status, const QJsonDocument &doc,
                              const QByteArray &raw, const QString &netError) {
        if (!ok) {
            emit log(QStringLiteral("Portfolio fetch failed (HTTP %1): %2")
                         .arg(status)
                         .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(200)) : netError),
                     true);
            return;
        }
        // Open positions are nested under clientPortfolio.positions; fall back to
        // the flatter shapes in case the payload differs.
        const QJsonObject root = doc.object();
        const QJsonObject clientPortfolio =
            pick(root, {QStringLiteral("clientPortfolio")}).toObject();
        QJsonArray arr = pick(clientPortfolio, {QStringLiteral("positions")}).toArray();
        if (arr.isEmpty()) {
            arr = asArray(doc, {QStringLiteral("positions"), QStringLiteral("openPositions"),
                                QStringLiteral("data")});
        }

        QList<Position> positions;
        for (const auto &v : std::as_const(arr)) {
            const QJsonObject o = v.toObject();
            const qint64 instrumentId =
                static_cast<qint64>(numFrom(pick(o, {QStringLiteral("instrumentId")})));
            const bool isCurrent = m_instrument.isValid()
                                   && (instrumentId == m_instrument.instrumentId);

            // Resolve the tradable symbol for this position; skip positions on
            // instruments that aren't in the app's list (the account can hold many).
            QString sym = m_symbolById.value(instrumentId);
            if (sym.isEmpty()) {
                const QString payloadSym = pick(o, {QStringLiteral("internalSymbolFull"),
                                                    QStringLiteral("symbolFull"),
                                                    QStringLiteral("symbol")}).toString();
                const bool listed =
                    m_tradableSymbols.contains(payloadSym, Qt::CaseInsensitive);
                if (!payloadSym.isEmpty() && listed) {
                    sym = payloadSym;
                } else if (isCurrent) {
                    sym = m_config.symbol;
                } else {
                    // unknown instrument — the empty symbol is skipped just below
                }
            }
            if (sym.isEmpty()) {
                continue;  // not one of the app's instruments (or its id not resolved yet)
            }

            Position p;
            p.positionId =
                pick(o, {QStringLiteral("positionId"), QStringLiteral("id")}).toVariant().toString();
            p.instrumentId = instrumentId;
            p.symbol = sym;
            p.isBuy = pick(o, {QStringLiteral("isBuy"), QStringLiteral("buy")}).toBool(true);
            p.amount = numFrom(pick(o, {QStringLiteral("amount"), QStringLiteral("investedAmount")}));
            p.units = numFrom(pick(o, {QStringLiteral("units"), QStringLiteral("unitsValue")}));
            p.openRate = numFrom(pick(o, {QStringLiteral("openRate"), QStringLiteral("openPrice")}));
            p.leverage = numFrom(pick(o, {QStringLiteral("leverage")}));
            p.openTime =
                timeFrom(pick(o, {QStringLiteral("openDateTime"), QStringLiteral("openTime")}));

            // SL/TP: the payload carries rates plus explicit "disabled" flags (a tiny
            // sentinel rate is used when a leg is off), so honour those flags.
            const bool noSl = pick(o, {QStringLiteral("isNoStopLoss")}).toBool(false);
            const bool noTp = pick(o, {QStringLiteral("isNoTakeProfit")}).toBool(false);
            p.stopLossRate = noSl ? 0.0 : numFrom(pick(o, {QStringLiteral("stopLossRate")}));
            p.takeProfitRate = noTp ? 0.0 : numFrom(pick(o, {QStringLiteral("takeProfitRate")}));
            p.trailingStop = pick(o, {QStringLiteral("isTslEnabled")}).toBool(false);

            // Authoritative P/L: eToro's own live figure in the account currency,
            // under unrealizedPnL.pnL (already net of the closing spread and fees).
            // Prefer it; fall back to the flat fields, or to a derived estimate in
            // finalizePortfolioPl() only when no API value is available.
            const QJsonObject upnl = pick(o, {QStringLiteral("unrealizedPnL")}).toObject();
            const QJsonValue pnlVal = pick(upnl, {QStringLiteral("pnL"), QStringLiteral("pnl")});
            if (!pnlVal.isUndefined()) {
                p.profit = numFrom(pnlVal);
                p.profitFromApi = true;
            } else {
                const QJsonValue flat = pick(o, {QStringLiteral("netProfit"),
                                                 QStringLiteral("profit"), QStringLiteral("openPl")});
                if (!flat.isUndefined()) {
                    p.profit = numFrom(flat);
                    p.profitFromApi = true;
                }
            }
            positions << p;
        }
        finalizePortfolioPl(positions);
    }, /*retriesLeft=*/2);  // ride out a transient 429/5xx rather than logging an error
}

void EtoroClient::finalizePortfolioPl(const QList<Position> &positions)
{
    // Remember each open position's own instrument, so closePositionReal() can send
    // the right InstrumentId even for a trade on an instrument other than the one
    // currently shown in the header.
    m_instrumentByPosition.clear();
    for (const Position &p : positions) {
        if (!p.positionId.isEmpty() && (p.instrumentId > 0)) {
            static_cast<void>(m_instrumentByPosition.insert(p.positionId, p.instrumentId));
        }
    }

    // (Order-open confirmation is handled authoritatively by confirmOrderReal() via
    // the order-lookup endpoint, not by watching this snapshot's position count —
    // that lagged and produced false "opened no position" reports.)

    // Collect the distinct held instrument ids and fetch their live rates so P/L
    // is computed for every open trade, not just the one currently on screen.
    QSet<qint64> seen;
    QStringList ids;
    for (const Position &p : positions) {
        if ((p.instrumentId > 0) && !seen.contains(p.instrumentId)) {
            static_cast<void>(seen.insert(p.instrumentId));
            ids << QString::number(p.instrumentId);
        }
    }
    if (ids.isEmpty()) {
        emit portfolioUpdated(positions);
        return;
    }

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("instrumentIds"), ids.join(QLatin1Char(',')));
    QNetworkReply *reply = apiGet(QStringLiteral("/v1/market-data/instruments/rates"), q);
    // Init-capture: deduces a mutable QList<Position> copy (a plain by-copy capture
    // of the const-ref parameter would keep the referenced const type).
    handleReply(reply, [this, positions = positions](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                          const QByteArray & /*raw*/, const QString & /*netError*/) mutable {
        if (ok) {
            QHash<qint64, double> bidById;
            QHash<qint64, double> askById;
            QJsonArray arr = asArray(doc, {QStringLiteral("rates"), QStringLiteral("data")});
            if (arr.isEmpty() && doc.isObject()) {
                arr.append(doc.object());
            }
            for (const auto &v : std::as_const(arr)) {
                const QJsonObject rate = v.toObject();
                const qint64 id =
                    static_cast<qint64>(numFrom(pick(rate, {QStringLiteral("instrumentId")})));
                const double bid = numFrom(pick(rate, {QStringLiteral("bid"), QStringLiteral("cvtBid")}));
                const double ask = numFrom(pick(rate, {QStringLiteral("ask"), QStringLiteral("cvtAsk")}));
                if ((id > 0) && (bid > 0.0)) {
                    static_cast<void>(bidById.insert(id, bid));
                }
                if ((id > 0) && (ask > 0.0)) {
                    static_cast<void>(askById.insert(id, ask));
                }
            }
            for (Position &p : positions) {
                const double bid = bidById.value(p.instrumentId, 0.0);
                const double ask = askById.value(p.instrumentId, 0.0);
                if (p.openRate <= 0.0) {
                    continue;
                }
                // Value per point in the account currency: amount × leverage is the
                // account-currency notional, so notional/openRate moves 1:1 with price
                // — raw units are in the *quote* currency and mis-scale non-account-
                // currency instruments (e.g. HKG50 in HKD). Falls back to units.
                const double notional = p.amount * p.leverage;
                const double perPoint = (notional > 0.0) ? (notional / p.openRate) : p.units;

                // Expected spread cost to close now. eToro attributes HALF the spread
                // to opening and half to closing (a long sells at the bid = mid − spread/2),
                // so its close dialog shows spread/2 × value-per-point; charging the full
                // spread here was ≈2× eToro's figure (same bug fixed for the opening cost).
                if ((bid > 0.0) && (ask > 0.0) && (ask > bid)) {
                    p.closingCost = ((ask - bid) / 2.0) * perPoint;
                }

                // Only estimate P/L when eToro didn't supply its own. Mark against the
                // side the trade closes on (a long sells at the bid, a short buys at the
                // ask), matching how eToro values it, rather than the mid.
                if (!p.profitFromApi) {
                    const double close = p.isBuy ? bid : ask;
                    if (close > 0.0) {
                        const double dir = p.isBuy ? 1.0 : -1.0;
                        p.profit = dir * perPoint * (close - p.openRate);
                    }
                }
            }
        }
        emit portfolioUpdated(positions);  // best-effort: emit even if rates failed
    });
}

void EtoroClient::refreshBalanceReal()
{
    // The dedicated /balances endpoint needs the money.balance:read scope, which
    // trading tokens typically lack (403). The aggregate-portfolio snapshot is
    // authorized by the trading scope and exposes the same figure as
    // accountTotals.accountAvailableCash ("cash available for new trades").
    const QString path =
        QStringLiteral("/v1/trading/info%1/aggregate-portfolio").arg(accountSegment());
    QNetworkReply *reply = apiGet(path, QUrlQuery());
    handleReply(reply, [this](bool ok, qint32 status, const QJsonDocument &doc,
                              const QByteArray &raw, const QString &netError) {
        if (!ok) {
            emit log(QStringLiteral("Balance fetch failed (HTTP %1): %2")
                         .arg(status)
                         .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(200)) : netError),
                     true);
            return;
        }
        const QJsonObject root = doc.object();
        const QJsonObject totals = pick(root, {QStringLiteral("accountTotals")}).toObject();
        const QJsonValue cash = pick(totals, {QStringLiteral("accountAvailableCash"),
                                              QStringLiteral("accountBalance")});
        if (cash.isUndefined() || cash.isNull()) {
            return;
        }
        const QString accountCurrency = pick(root, {QStringLiteral("accountCurrency")}).toString();
        // Remember the real account currency so orders are denominated in it rather
        // than a mismatched config value (a USD account rejects an "eur" order).
        if (!accountCurrency.isEmpty()) {
            m_accountCurrency = accountCurrency;
        }
        const QString currency = accountCurrency.isEmpty() ? m_config.orderCurrency : accountCurrency;
        emit cashUpdated(numFrom(cash), currency);
    }, /*retriesLeft=*/2);  // ride out a transient 429/5xx rather than logging an error
}

void EtoroClient::fetchTradeHistoryPageReal(const QSharedPointer<PnlAccum> &acc)
{
    constexpr qint32 kPageSize = 1000;
    // History is newest-first and honours minDate; page until an empty page arrives.
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("minDate"), acc->minDate.toString(Qt::ISODate));
    q.addQueryItem(QStringLiteral("page"), QString::number(acc->page));
    q.addQueryItem(QStringLiteral("pageSize"), QString::number(kPageSize));

    QNetworkReply *reply = apiGet(QStringLiteral("/v1/trading/info/trade/history"), q);
    // Retry on 429: the history endpoint shares the small default rate pool, so a
    // transient rate limit shouldn't kill the whole summary — wait it out and retry.
    handleReply(reply, [this, acc](bool ok, qint32 status, const QJsonDocument &doc,
                                    const QByteArray &raw, const QString &netError) {
        if (!ok) {
            m_pnlFetching = false;
            // status 0 = transport failure (timeout/connection drop) that survived the
            // retries below — name it instead of showing a meaningless "HTTP 0".
            const QString cause =
                (status == 0) ? QStringLiteral("network error") : QStringLiteral("HTTP %1").arg(status);
            emit monthlyPnlFailed(
                QStringLiteral("Closed-trade history fetch failed (%1): %2")
                    .arg(cause,
                         netError.isEmpty() ? QString::fromUtf8(raw.left(200)) : netError));
            return;
        }
        const QJsonArray batch =
            doc.isArray() ? doc.array()
                          : asArray(doc, {QStringLiteral("data"), QStringLiteral("trades")});
        for (const auto &v : batch) {
            const QJsonObject o = v.toObject();
            const double net = numFrom(pick(o, {QStringLiteral("netProfit")}));
            const double fee = numFrom(pick(o, {QStringLiteral("fees")}));
            acc->accountTrades++;
            acc->accountNet += net;
            acc->accountFees += fee;

            const qint64 iid =
                static_cast<qint64>(numFrom(pick(o, {QStringLiteral("instrumentId")})));
            const QString sym = m_symbolById.value(iid);

            // Keep the individual trade (all instruments) for the detail list.
            ClosedTrade ct;
            ct.instrumentId = iid;
            ct.listed = !sym.isEmpty();
            ct.symbol = ct.listed ? sym : QStringLiteral("#%1").arg(iid);
            ct.isBuy = pick(o, {QStringLiteral("isBuy")}).toBool(true);
            const double levRaw = numFrom(pick(o, {QStringLiteral("leverage")}));
            ct.leverage = (levRaw > 0.0) ? levRaw : 1.0;
            ct.investment = numFrom(pick(o, {QStringLiteral("investment"),
                                             QStringLiteral("initialInvestment")}));
            ct.units = numFrom(pick(o, {QStringLiteral("units")}));
            ct.openRate = numFrom(pick(o, {QStringLiteral("openRate")}));
            ct.closeRate = numFrom(pick(o, {QStringLiteral("closeRate")}));
            ct.openTime = timeFrom(pick(o, {QStringLiteral("openTimestamp")}));
            ct.closeTime = timeFrom(pick(o, {QStringLiteral("closeTimestamp")}));
            ct.netProfit = net;
            ct.fees = fee;
            acc->trades.append(ct);
            if (iid > 0) {
                static_cast<void>(acc->instrumentIds.insert(iid));
            }

            // The aggregated summary stays restricted to the app's listed
            // (selectable) instruments; the account can hold many others.
            if (sym.isEmpty()) {
                continue;
            }
            InstrumentPnl &r = acc->bySymbol[sym];
            r.symbol = sym;
            r.trades++;
            r.netProfit += net;
            r.fees += fee;
        }

        // A non-empty page may have more behind it; an empty page ends the history.
        // Cap the walk defensively so a misbehaving API can't loop forever.
        if (!batch.isEmpty() && (acc->page < 60)) {
            acc->page++;
            fetchTradeHistoryPageReal(acc);
            return;
        }
        finishTradeHistory(acc);
    }, /*retriesLeft=*/3);
}

void EtoroClient::finishTradeHistory(const QSharedPointer<PnlAccum> &acc)
{
    if (acc->trades.isEmpty() || acc->instrumentIds.isEmpty()) {
        emitMonthlyPnl(acc);
        emit closedTradesReady(acc->trades);
        return;
    }
    // The history API doesn't report what the spread was, so the open/close costs
    // are estimated from each instrument's CURRENT spread: eToro attributes half
    // the spread to opening and half to closing, and the account-currency value of
    // a half-spread is investment × leverage × (spread% / 2) — the same FX-free
    // identity the trade panel's opening-cost estimate uses. One bulk rates call
    // (≤100 ids) covers every instrument seen in the window.
    QStringList ids;
    const QSet<qint64> &instrumentIds = acc->instrumentIds;  // single-call range-init
    for (const qint64 id : instrumentIds) {
        ids << QString::number(id);
        if (ids.size() >= 100) {
            break;
        }
    }
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("instrumentIds"), ids.join(QLatin1Char(',')));
    QNetworkReply *reply = apiGet(QStringLiteral("/v1/market-data/instruments/rates"), q);
    handleReply(reply, [this, acc](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                   const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (ok) {
            const QJsonArray arr =
                asArray(doc, {QStringLiteral("rates"), QStringLiteral("data")});
            for (const auto &v : arr) {
                const QJsonObject rate = v.toObject();
                const qint64 id = static_cast<qint64>(numFrom(
                    pick(rate, {QStringLiteral("instrumentID"), QStringLiteral("instrumentId")})));
                const double bid =
                    numFrom(pick(rate, {QStringLiteral("bid"), QStringLiteral("cvtBid")}));
                const double ask =
                    numFrom(pick(rate, {QStringLiteral("ask"), QStringLiteral("cvtAsk")}));
                if ((id > 0) && (bid > 0.0) && (ask > bid)) {
                    static_cast<void>(m_spreadPctById.insert(
                        id, ((ask - bid) / ((ask + bid) / 2.0)) * 100.0));
                }
                // Rows without a usable bid/ask (frozen weekend quotes) keep the
                // previously cached spread, so estimates don't flicker away.
            }
        }
        for (ClosedTrade &t : acc->trades) {
            const double sp = m_spreadPctById.value(t.instrumentId, 0.0);
            if ((sp > 0.0) && (t.investment > 0.0) && (t.leverage > 0.0)) {
                const double half = t.investment * t.leverage * (sp / 100.0) / 2.0;
                t.openCostEst = half;
                t.closeCostEst = half;
                t.costEstValid = true;
                t.spreadPctUsed = sp;
                // Frozen after-hours quotes carry the widened closing spread, so
                // the estimate overstates what the trade really paid — flag it.
                t.spreadStale = !m_freshQuoteSymbols.contains(t.symbol);
                // Roll the estimate up into the per-instrument summary so its
                // Costs column can show open+close+fees.
                if (t.listed) {
                    acc->bySymbol[t.symbol].estSpreadCosts += t.openCostEst + t.closeCostEst;
                }
            }
        }
        emitMonthlyPnl(acc);
        emit closedTradesReady(acc->trades);
    }, /*retriesLeft=*/1);
}

void EtoroClient::emitMonthlyPnl(const QSharedPointer<PnlAccum> &acc)
{
    m_pnlFetching = false;  // walk complete
    MonthlyPnl s;
    s.fromDate = acc->minDate;
    s.toDate = QDate::currentDate();
    s.currency =
        (m_accountCurrency.isEmpty() ? m_config.orderCurrency : m_accountCurrency).toUpper();
    s.accountTrades = acc->accountTrades;
    s.accountNet = acc->accountNet;
    for (auto it = acc->bySymbol.constBegin(); it != acc->bySymbol.constEnd(); ++it) {
        s.perInstrument.append(it.value());
        s.trades += it.value().trades;
        s.netProfit += it.value().netProfit;
        s.fees += it.value().fees;
    }
    const auto sortBegin = s.perInstrument.begin();
    const auto sortEnd = s.perInstrument.end();
    std::sort(sortBegin, sortEnd,
              [](const InstrumentPnl &a, const InstrumentPnl &b) {
                  return a.netProfit > b.netProfit;
              });
    emit monthlyPnlReady(s);
}

// ---- leverage screener -----------------------------------------------------

void EtoroClient::scanInstruments()
{
    if (m_scanActive) {
        emit log(QStringLiteral("Screener already running…"), false);
        return;
    }
    m_scanActive = true;
    if (m_simulated || !m_config.hasCredentials()) {
        m_sim->scanInstruments(m_tradableSymbols);  // synchronous; resets
                                                    // m_scanActive via the forward
    } else {
        scanInstrumentsReal();
    }
}

void EtoroClient::scanInstrumentsReal()
{
    // Work list = the instruments whose ids are already resolved. Ids resolve
    // asynchronously at startup; scan whatever is known now, and nudge resolution
    // of any stragglers so a later rescan is complete.
    auto st = QSharedPointer<ScanState>::create();
    for (auto it = m_symbolById.constBegin(); it != m_symbolById.constEnd(); ++it) {
        st->queue.append(ScanItem{it.key(), it.value()});
    }
    st->total = static_cast<qint32>(st->queue.size());
    if (st->total < m_tradableSymbols.size()) {
        resolveListedInstrumentIds();
    }

    if (st->queue.isEmpty()) {
        emit log(QStringLiteral("Screener: instrument ids not resolved yet — try again shortly."),
                 true);
        m_scanActive = false;
        emit screenerFinished();
        return;
    }
    emit screenerProgress(0, st->total);

    // One bulk eligibility call for every id (the endpoint takes up to 100 and
    // returns each instrument's leverageConfigs) → max CFD leverage per instrument.
    QJsonArray ids;
    const QList<ScanItem> &queue = st->queue;  // const ref: detach-free, single-call range-init
    for (const ScanItem &item : queue) {
        ids.append(static_cast<qint32>(item.id));
    }
    QJsonObject body;
    body[QStringLiteral("instrumentIds")] = ids;
    body[QStringLiteral("currency")] = QStringLiteral("USD");  // API supports USD only

    const QString path = QStringLiteral("/v2/trading/info%1/eligibility").arg(accountSegment());
    QNetworkReply *reply = apiPost(path, body);
    handleReply(reply, [this, st](bool ok, qint32 status, const QJsonDocument &doc,
                                  const QByteArray &raw, const QString &netError) {
        if (ok) {
            const QJsonArray elig = doc.object().value(QStringLiteral("eligibilities")).toArray();
            for (const auto &ev : elig) {
                const QJsonObject e = ev.toObject();
                const qint64 id =
                    static_cast<qint64>(numFrom(pick(e, {QStringLiteral("instrumentId")})));
                // Prefer CFD leverage (how this app trades); fall back to all.
                QList<qint32> cfd;
                QList<qint32> all;
                const QJsonArray leverageConfigs =
                    e.value(QStringLiteral("leverageConfigs")).toArray();
                for (const auto &lcv : leverageConfigs) {
                    const QJsonObject lc = lcv.toObject();
                    const bool isCfd = lc.value(QStringLiteral("settlementType"))
                                           .toString()
                                           .compare(QLatin1String("cfd"), Qt::CaseInsensitive) == 0;
                    const QJsonArray leverageValues =
                        lc.value(QStringLiteral("leverageValues")).toArray();
                    for (const auto &lv : leverageValues) {
                        const qint32 v = lv.toInt();
                        if (v <= 0) {
                            continue;
                        }
                        all.append(v);
                        if (isCfd) {
                            cfd.append(v);
                        }
                    }
                }
                qint32 mx = 0;
                const QList<qint32> &pool = cfd.isEmpty() ? all : cfd;
                for (const qint32 v : pool) {
                    mx = std::max(mx, v);
                }
                if ((id > 0) && (mx > 0)) {
                    static_cast<void>(st->maxLevById.insert(id, mx));
                }
            }
        } else {
            emit log(QStringLiteral("Screener leverage lookup failed (HTTP %1): %2 — "
                                    "rows will show leverage as n/a.")
                         .arg(status)
                         .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(200)) : netError),
                     true);
        }
        // Fetch candles regardless: a row still ranks (leverage only) without them.
        fetchScanCandle(st);
    });
}

void EtoroClient::fetchScanCandle(const QSharedPointer<ScanState> &st)
{
    if (st->index >= st->queue.size()) {
        m_scanActive = false;
        emit screenerFinished();
        return;
    }
    const ScanItem item = st->queue[st->index];

    // Hourly candles over ~2 weeks: enough for every indicator (SMA50, MACD, kNN,
    // Hurst) and a stable swing view, at one request per instrument.
    const QString path = QStringLiteral("/v1/market-data/instruments/%1/history/candles/%2/%3/%4")
                             .arg(item.id)
                             .arg(m_candleDirection, QStringLiteral("OneHour"))
                             .arg(300);
    QNetworkReply *reply = apiGet(path, QUrlQuery());
    handleReply(reply, [this, st, item](bool ok, qint32, const QJsonDocument &doc,
                                        const QByteArray &, const QString &) {
        ScreenerRow row;
        row.symbol = item.symbol;
        row.maxLeverage = st->maxLevById.value(item.id, 0);
        if (ok) {
            QJsonArray arr = asArray(doc, {QStringLiteral("candles"), QStringLiteral("data")});
            // eToro nests one level: { candles: [ { instrumentId, candles: [ … ] } ] }.
            const QJsonValue firstEntry = arr.isEmpty() ? QJsonValue() : arr.first();
            if (firstEntry.isObject()) {
                const QJsonObject firstObj = firstEntry.toObject();
                const QJsonValue inner = pick(firstObj, {QStringLiteral("candles")});
                if (inner.isArray()) {
                    arr = inner.toArray();
                }
            }
            QList<Candle> candles;
            candles.reserve(arr.size());
            for (const auto &v : std::as_const(arr)) {
                const QJsonObject o = v.toObject();
                Candle c;
                c.timestamp = timeFrom(pick(o, {QStringLiteral("fromDate"),
                                                QStringLiteral("timestamp"),
                                                QStringLiteral("date")}));
                c.close = numFrom(pick(o, {QStringLiteral("close"), QStringLiteral("rate"),
                                           QStringLiteral("mid")}));
                if (c.close > 0.0) {
                    candles << c;
                }
            }
            const auto sortBegin = candles.begin();
            const auto sortEnd = candles.end();
            std::sort(sortBegin, sortEnd,
                      [](const Candle &a, const Candle &b) { return a.timestamp < b.timestamp; });
            row.closes.reserve(candles.size());
            for (const Candle &c : std::as_const(candles)) {
                row.closes.append(c.close);
            }
            row.ok = !row.closes.isEmpty();
            if (row.ok) {
                row.lastPrice = row.closes.last();
            }
        }
        emit screenerRow(row);
        ++st->index;
        emit screenerProgress(st->index, st->total);
        fetchScanCandle(st);  // next instrument (sequential paces the rate budget)
    }, /*retriesLeft=*/2);  // ride out a transient 429 on the shared market-data pool
}

void EtoroClient::openPositionReal(bool isBuy, double amount, double leverage,
                                   double stopLossAmount, double takeProfitAmount,
                                   bool trailingStop)
{
    // UnifiedOrderRequest (POST /v2/trading/execution/orders):
    //  * identify the instrument by EXACTLY ONE of symbol/instrumentId — sending
    //    both is rejected, so we send instrumentId only;
    //  * settlementType is REQUIRED for open orders (SPX500 is a leveraged CFD);
    //  * leverage / instrumentId are int32 in the schema.
    QJsonObject body;
    body[QStringLiteral("action")] = QStringLiteral("open");
    // Opening transactions only: "buy" opens a long, "sellShort" opens a short.
    // ("sell" / "buyToCover" are the *closing* transactions and are rejected here —
    // positions are closed via the market-close endpoint instead.)
    body[QStringLiteral("transaction")] = isBuy ? QStringLiteral("buy") : QStringLiteral("sellShort");
    body[QStringLiteral("instrumentId")] = static_cast<qint32>(m_instrument.instrumentId);
    body[QStringLiteral("settlementType")] = QStringLiteral("cfd");
    body[QStringLiteral("orderType")] = QStringLiteral("mkt");
    body[QStringLiteral("amount")] = amount;
    // The order amount must be in the account currency; prefer the real currency
    // learned from the API over a (possibly stale/mismatched) config value.
    const QString orderCurrency =
        (m_accountCurrency.isEmpty() ? m_config.orderCurrency : m_accountCurrency).toLower();
    body[QStringLiteral("orderCurrency")] = orderCurrency;
    body[QStringLiteral("leverage")] = static_cast<qint32>(leverage);

    // Convert the requested stop-loss / take-profit *amounts* (account currency)
    // into absolute rates: a position of `units` gains/loses 1 currency unit per
    // (1 / units) of price move, so an X loss/profit is X / units away from the
    // open rate — below for longs, above for shorts (mirrored for the target).
    // (A stop-loss rate is also REQUIRED by the API once leverage > 1.)
    //
    // Price the SL/TP (and units) off the side the position actually opens at — a buy
    // fills near the ask, a sell near the bid — not the mid. Using the mid leaves a
    // half-spread error, so the SL shown on the open trade drifts from the set amount.
    double ref = isBuy ? m_lastAsk : m_lastBid;
    if (ref <= 0.0) {
        ref = (m_lastPrice > 0.0) ? m_lastPrice : m_instrument.currentRate;
    }
    double units = (ref > 0.0) ? ((amount * leverage) / ref) : 0.0;

    // Name the instrument actually traded (falling back to the configured symbol).
    const QString symbolLabel =
        !m_instrument.symbol.isEmpty() ? m_instrument.symbol : m_config.symbol;

    // eToro caps the units per single order (eligibility's maxUnitsPerOrder, e.g. 20
    // for GOLD). An oversized order is accepted here (an orderId is created) but then
    // rejected at execution with a terse "PositionUnits ... MaxAllowed" dialog — the
    // requested trade never opens. So shrink an over-cap order to the largest amount
    // that fits (shaved 0.5% so a price move before execution can't tip it back over)
    // and log the reduction; the "order submitted" message then reports the amount
    // actually sent. Skipped while the cap (or a live rate) is still unknown; eToro's
    // own validation remains the backstop then.
    const double maxUnits = m_instrument.maxUnitsPerOrder;
    if ((maxUnits > 0.0) && (units > maxUnits)) {
        const double maxAmount = std::floor(((maxUnits * ref) / leverage) * 0.995);
        if (maxAmount < 1.0) {
            emit orderResult(false,
                QStringLiteral("%1 %2 not sent — even the smallest order would exceed "
                               "eToro's cap of %3 units per order on this instrument.")
                    .arg(isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                         symbolLabel)
                    .arg(maxUnits));
            return;
        }
        emit log(QStringLiteral("%1 %2: %3 %4 at x%5 would be ≈ %6 units — over eToro's "
                                "cap of %7 units per order; order size reduced to %8 %4.")
                     .arg(isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                          symbolLabel)
                     .arg(amount, 0, 'f', 2)
                     .arg(orderCurrency.toUpper())
                     .arg(leverage)
                     .arg(units, 0, 'f', 2)
                     .arg(maxUnits)
                     .arg(maxAmount, 0, 'f', 2),
                 false);
        amount = maxAmount;
        body[QStringLiteral("amount")] = amount;
        units = (amount * leverage) / ref;  // SL/TP distances below follow the new size
    }

    if (units > 0.0) {
        if (stopLossAmount > 0.0) {
            const double dist = stopLossAmount / units;
            const double sl = isBuy ? (ref - dist) : (ref + dist);
            body[QStringLiteral("stopLossRate")] = std::round(sl * 100.0) / 100.0;
            // eToro trails the stop server-side when the type is "trailing".
            body[QStringLiteral("stopLossType")] =
                trailingStop ? QStringLiteral("trailing") : QStringLiteral("fixed");
        }
        if (takeProfitAmount > 0.0) {
            const double dist = takeProfitAmount / units;
            const double tp = isBuy ? (ref + dist) : (ref - dist);
            body[QStringLiteral("takeProfitRate")] = std::round(tp * 100.0) / 100.0;
        }
    }

    const QString path =
        QStringLiteral("/v2/trading/execution%1/orders").arg(accountSegment());
    QNetworkReply *reply = apiPost(path, body);
    handleReply(reply, [this, isBuy, amount, symbolLabel, orderCurrency](
                           bool ok, qint32 status, const QJsonDocument &doc,
                           const QByteArray &raw, const QString &netError) {
        if (ok) {
            // A 200 only means the order was SUBMITTED (an orderId was created) — a
            // market order can still be rejected at execution and open no position.
            // Report it as submitted, then confirm the real outcome via the
            // order-lookup endpoint (confirmOrderReal), not the lagging portfolio.
            const QJsonObject submitted = doc.object();
            const qint64 orderId =
                static_cast<qint64>(numFrom(pick(submitted, {QStringLiteral("orderId")})));
            emit orderResult(true,
                             QStringLiteral("%1 order submitted (id %5): %2 %3 %4 — confirming…")
                                 .arg(isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                                 .arg(amount)
                                 .arg(orderCurrency.toUpper(), symbolLabel)
                                 .arg(orderId));
            if (orderId > 0) {
                // Give execution a moment to register before the first lookup.
                QTimer::singleShot(1500, this, [this, orderId, isBuy, symbolLabel] {
                    confirmOrderReal(orderId, isBuy, symbolLabel, 0);
                });
            }
            refreshPortfolioReal();
            refreshBalanceReal();
        } else {
            // Prefer the API's ProblemDetails message; fall back to the raw body / net error.
            QString reason;
            if (doc.isObject()) {
                const QJsonObject o = doc.object();
                reason = pick(o, {QStringLiteral("detail"), QStringLiteral("message"),
                                  QStringLiteral("error")})
                             .toString();
                // Validation failures (ASP.NET ValidationProblemDetails) carry a generic
                // title and put the per-field reasons under "errors": {field: [msg, …]}.
                // Flatten those so the message names the actual offending field(s).
                const QJsonValue errs = pick(o, {QStringLiteral("errors")});
                if (errs.isObject()) {
                    QStringList parts;
                    const QJsonObject fields = errs.toObject();
                    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
                        QStringList msgs;
                        if (it.value().isArray()) {
                            const QJsonArray a = it.value().toArray();
                            for (const auto &m : a) {
                                msgs << m.toString();
                            }
                        } else {
                            msgs << it.value().toVariant().toString();
                        }
                        const QString joined = msgs.join(QStringLiteral("; "));
                        parts << (it.key().isEmpty()
                                      ? joined
                                      : QStringLiteral("%1: %2").arg(it.key(), joined));
                    }
                    if (!parts.isEmpty()) {
                        const QString detail = parts.join(QStringLiteral(" | "));
                        reason = reason.isEmpty()
                                     ? detail
                                     : QStringLiteral("%1 (%2)").arg(reason, detail);
                    }
                }
                if (reason.isEmpty()) {
                    reason = pick(o, {QStringLiteral("title")}).toString();
                }
            }
            if (reason.isEmpty()) {
                reason = QString::fromUtf8(raw.left(300)).trimmed();
            }
            if (reason.isEmpty()) {
                reason = netError;
            }
            emit orderResult(false,
                             QStringLiteral("Order rejected (HTTP %1): %2").arg(status).arg(reason));
        }
    });
}

void EtoroClient::confirmOrderReal(qint64 orderId, bool isBuy, const QString &symbolLabel,
                                   qint32 attempt)
{
    static constexpr qint32 kMaxAttempts = 8;  // ~1.5s + 8×2s ≈ 17s before giving up
    static constexpr qint32 kRetryMs = 2000;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("orderId"), QString::number(orderId));
    const QString path =
        QStringLiteral("/v2/trading/info%1/orders:lookup").arg(accountSegment());

    QNetworkReply *reply = apiGet(path, query);
    handleReply(reply, [this, orderId, isBuy, symbolLabel, attempt](
                           bool ok, qint32 /*status*/, const QJsonDocument &doc,
                           const QByteArray & /*raw*/, const QString & /*netError*/) {
        const QString side = isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");

        // Poll again while the outcome is still unknown; once out of attempts, report
        // it as pending (ok=true → log only, no alarming dialog) — never as rejected.
        const auto keepWaiting = [this, orderId, isBuy, symbolLabel, attempt] {
            if ((attempt + 1) < kMaxAttempts) {
                QTimer::singleShot(kRetryMs, this, [this, orderId, isBuy, symbolLabel, attempt] {
                    confirmOrderReal(orderId, isBuy, symbolLabel, attempt + 1);
                });
            } else {
                emit orderResult(true,
                    QStringLiteral("Order id %1 still confirming — check eToro for the outcome.")
                        .arg(orderId));
            }
        };

        // A transient error / 404 (the order isn't queryable the instant it's created)
        // is not evidence of rejection — keep polling.
        if (!ok) {
            keepWaiting();
            return;
        }

        const QJsonObject o = doc.object();

        // The endpoint lists every position this order opened; any still-open one is
        // the definitive "it worked" signal, regardless of the status wording.
        QString openedPositionId;
        const QJsonArray execs = pick(o, {QStringLiteral("positionExecutions")}).toArray();
        for (const auto &v : execs) {
            const QJsonObject e = v.toObject();
            const QString pid = pick(e, {QStringLiteral("positionId")}).toVariant().toString();
            const QString state = pick(e, {QStringLiteral("state")}).toString();
            if (!pid.isEmpty() && (pid != QStringLiteral("0"))
                && (state.compare(QStringLiteral("closed"), Qt::CaseInsensitive) != 0)) {
                openedPositionId = pid;
                break;
            }
        }

        // Status ids: 3 Filled, 5 PartiallyFilled (opened); 4 Rejected, 7 Canceled,
        // 8 Expired (did not open); 1/2/6/11/12 still pending.
        const QJsonObject st = pick(o, {QStringLiteral("status")}).toObject();
        const qint32 statusId = static_cast<qint32>(numFrom(pick(st, {QStringLiteral("id")})));
        const QString statusName = pick(st, {QStringLiteral("name")}).toString();
        const QString errMsg = pick(st, {QStringLiteral("errorMessage")}).toString();

        if (!openedPositionId.isEmpty() || (statusId == 3) || (statusId == 5)) {
            emit orderResult(true,
                openedPositionId.isEmpty()
                    ? QStringLiteral("%1 %2 confirmed — order id %3 filled.")
                          .arg(side, symbolLabel).arg(orderId)
                    : QStringLiteral("%1 %2 opened — position id %3 (order %4).")
                          .arg(side, symbolLabel, openedPositionId).arg(orderId));
            refreshPortfolioReal();
            refreshBalanceReal();
            return;
        }

        if ((statusId == 4) || (statusId == 7) || (statusId == 8)) {
            const QString why = !errMsg.isEmpty()
                                    ? errMsg
                                    : (statusName.isEmpty() ? QStringLiteral("rejected at execution")
                                                            : statusName);
            emit orderResult(false,
                QStringLiteral("Order id %1 did not open — %2 (check eToro).").arg(orderId).arg(why));
            return;
        }

        keepWaiting();  // received / placed / waiting-for-market / … → check again
    }, /*retriesLeft=*/1);
}

void EtoroClient::closePositionReal(const QString &positionId)
{
    // Close the position on ITS instrument, not whatever is currently shown — the
    // portfolio table can hold trades on many instruments and any ticked one may be
    // closed. Fall back to the shown instrument only if the id isn't cached yet.
    const qint64 instrumentId =
        m_instrumentByPosition.value(positionId, m_instrument.instrumentId);

    QJsonObject body;
    body[QStringLiteral("InstrumentId")] = static_cast<double>(instrumentId);
    body[QStringLiteral("UnitsToDeduct")] = QJsonValue(QJsonValue::Null);  // full close

    const QString path = QStringLiteral("/v1/trading/execution%1/market-close-orders/positions/%2")
                             .arg(accountSegment(), positionId);
    QNetworkReply *reply = apiPost(path, body);
    handleReply(reply, [this, positionId](bool ok, qint32 status, const QJsonDocument & /*doc*/,
                                          const QByteArray &raw, const QString &netError) {
        if (ok) {
            emit positionClosed(true, QStringLiteral("Position %1 closed.").arg(positionId));
            refreshPortfolioReal();
            refreshBalanceReal();
        } else {
            emit positionClosed(false, QStringLiteral("Close failed (HTTP %1): %2")
                                           .arg(status)
                                           .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(300))
                                                                   : netError));
        }
    });
}

void EtoroClient::modifyPositionReal(const QString &positionId, double stopLossRate,
                                     double takeProfitRate, bool trailingStop)
{
    // PATCH /v2/trading{/demo}/positions/{id}: set a new SL/TP rate, or clear the
    // leg entirely when the rate is 0. A 5-dp round keeps forex rates intact while
    // avoiding spurious precision on indices.
    QJsonObject body;
    if (stopLossRate > 0.0) {
        body[QStringLiteral("stopLossRate")] = std::round(stopLossRate * 1e5) / 1e5;
        body[QStringLiteral("stopLossType")] =
            trailingStop ? QStringLiteral("trailing") : QStringLiteral("fixed");
    } else {
        body[QStringLiteral("clearStopLoss")] = true;
    }
    if (takeProfitRate > 0.0) {
        body[QStringLiteral("takeProfitRate")] = std::round(takeProfitRate * 1e5) / 1e5;
    } else {
        body[QStringLiteral("clearTakeProfit")] = true;
    }

    const QString path =
        QStringLiteral("/v2/trading%1/positions/%2").arg(accountSegment(), positionId);
    QNetworkReply *reply = apiPatch(path, body);
    handleReply(reply, [this, positionId](bool ok, qint32 status, const QJsonDocument &doc,
                                          const QByteArray &raw, const QString &netError) {
        if (ok) {
            emit log(QStringLiteral("Trade #%1: stop-loss / take-profit updated.").arg(positionId),
                     false);
            refreshPortfolioReal();
        } else {
            QString reason;
            if (doc.isObject()) {
                const QJsonObject error = doc.object();
                reason = pick(error, {QStringLiteral("detail"), QStringLiteral("title"),
                                      QStringLiteral("message")})
                             .toString();
            }
            if (reason.isEmpty()) {
                reason = QString::fromUtf8(raw.left(200)).trimmed();
            }
            if (reason.isEmpty()) {
                reason = netError;
            }
            emit log(QStringLiteral("SL/TP update for #%1 failed (HTTP %2): %3")
                         .arg(positionId)
                         .arg(status)
                         .arg(reason),
                     true);
        }
    });
}

// ===========================================================================
// Simulation mode
// ===========================================================================

void EtoroClient::startSimulation(bool resetAccount)
{
    m_simulated = true;
    m_instrument = m_sim->prepare(m_config.symbol, m_config.orderCurrency, resetAccount);

    emit log(QStringLiteral("No API credentials found — running in SIMULATION mode with a "
                            "synthetic %1 price feed. Add eToro keys to trade for real "
                            "(see README).")
                 .arg(m_config.symbol),
             false);
    emit ready(m_instrument);

    // The live EUR/USD display rate comes from the eToro rates API, which the
    // simulation never calls — without this, the UI blocks every order with
    // "waiting for the EUR/USD rate". The account is synthetic anyway, so a
    // fixed representative rate keeps the € display and the sizing math
    // consistent (clearly synthetic, like the price feed itself).
    constexpr double kSimEurPerUsd = 0.90;
    m_eurPerUsd = kSimEurPerUsd;
    emit fxRateUpdated(m_eurPerUsd);

    m_sim->emitSnapshot();  // history, price, cash, portfolio, leverage steps
    m_lastPrice = m_sim->lastPrice();
    // Simulation has no market sessions: every instrument is always tradeable.
    emit tradeabilityUpdated(QSet<QString>(m_tradableSymbols.cbegin(), m_tradableSymbols.cend()));
    m_pollTimer->start();
}
