// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/EtoroClient.h"

#include "domain/PositionMath.h"
#include "services/JsonHttp.h"
#include "services/SimulationEngine.h"

#include <QHash>
#include <QHostAddress>
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
#include <array>
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

// Human-readable reason for a rejected request: the API's ProblemDetails message,
// falling back to the raw body and then to the network error. Validation failures
// (ASP.NET ValidationProblemDetails) carry a generic title and put the per-field
// reasons under "errors": {field: [msg, …]} — those get flattened in, so the message
// names the actual offending field(s) instead of just "One or more validation errors".
QString rejectionReason(const QJsonDocument &doc, const QByteArray &raw, const QString &netError)
{
    QString reason;
    if (doc.isObject()) {
        const QJsonObject o = doc.object();
        reason = pick(o, {QStringLiteral("detail"), QStringLiteral("message"),
                          QStringLiteral("error")})
                     .toString();
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
                parts << (it.key().isEmpty() ? joined
                                             : QStringLiteral("%1: %2").arg(it.key(), joined));
            }
            if (!parts.isEmpty()) {
                const QString detail = parts.join(QStringLiteral(" | "));
                reason = reason.isEmpty() ? detail
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
    return reason;
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

// Short human-readable reason from a failed reply: the transport error when
// there is one, otherwise the leading bytes of the response body. One shared
// helper so every log line truncates and falls back identically.
QString errorText(const QByteArray &raw, const QString &netError)
{
    return netError.isEmpty() ? QString::fromUtf8(raw.left(200)) : netError;
}

// The leverages one eligibility entry offers, sorted ascending and
// de-duplicated. This app trades CFDs, so CFD settlement wins; when the
// instrument has no CFD config, every configured leverage counts. Shared by the
// single-instrument lookup (which offers them all) and the screener (which only
// wants the maximum, i.e. the last element).
QList<qint32> eligibleLeverages(const QJsonObject &eligibility)
{
    QList<qint32> cfd;
    QList<qint32> all;
    const QJsonArray leverageConfigs =
        eligibility.value(QStringLiteral("leverageConfigs")).toArray();
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
    // begin()/end() once each: repeating them on a Qt container repeats the
    // detach check.
    const auto sortBegin = values.begin();
    const auto sortEnd = values.end();
    std::sort(sortBegin, sortEnd);
    const auto uniqueEnd = std::unique(sortBegin, sortEnd);
    static_cast<void>(values.erase(uniqueEnd, sortEnd));
    return values;
}

// The public, unauthenticated trade-config feed the eToro web app uses; it
// carries the per-unit overnight/weekend rollover fees. One request builder so
// the URL and the browser headers cannot drift between its two callers.
// Empty = the real host. Set only by tests, through
// EtoroClient::setTradeConfigBaseForTesting.
QString &tradeConfigBase()
{
    // cppcheck-suppress unassignedVariable  // assigned THROUGH the returned reference
    // by setTradeConfigBaseForTesting; cppcheck does not follow a non-const reference
    // out of a function and so reads the default-constructed static as never written.
    // 1 hit, and the alternative — an explicit empty initialiser — would silence
    // nothing and say less.
    static QString base;
    return base;
}

QNetworkRequest tradeConfigRequest(qint64 instrumentId)
{
    const QString host = tradeConfigBase().isEmpty()
                             ? QStringLiteral("https://api.etorostatic.com")
                             : tradeConfigBase();
    QNetworkRequest req(
        QUrl(QStringLiteral("%1/sapi/trade-real/instruments/%2").arg(host).arg(instrumentId)));
    JsonHttp::setBrowserHeaders(req);
    return req;
}

// The four rollover fees out of a trade-config reply (see tradeConfigRequest).
InstrumentFees feesFromTradeConfig(const QJsonDocument &doc)
{
    const QJsonObject inst = doc.object().value(QStringLiteral("Instrument")).toObject();
    InstrumentFees fees;
    fees.buyOvernight = inst.value(QStringLiteral("BuyOverNightFee")).toDouble();
    fees.sellOvernight = inst.value(QStringLiteral("SellOverNightFee")).toDouble();
    fees.buyWeekend = inst.value(QStringLiteral("BuyEndOfWeekFee")).toDouble();
    fees.sellWeekend = inst.value(QStringLiteral("SellEndOfWeekFee")).toDouble();
    return fees;
}

// Candles out of a history reply, oldest first, dropping any without a usable
// close. eToro nests one level: { candles: [ { instrumentId, candles: [ {ohlc} ] } ] }.
QList<Candle> candlesFrom(const QJsonDocument &doc)
{
    QJsonArray arr = asArray(doc, {QStringLiteral("candles"), QStringLiteral("data")});
    const QJsonValue firstEntry = arr.isEmpty() ? QJsonValue() : arr.first();
    if (firstEntry.isObject()) {
        const QJsonValue inner = pick(firstEntry.toObject(), {QStringLiteral("candles")});
        if (inner.isArray()) {
            arr = inner.toArray();
        }
    }
    QList<Candle> candles;
    candles.reserve(arr.size());
    for (const auto &v : std::as_const(arr)) {
        const QJsonObject o = v.toObject();
        Candle c;
        c.timestamp = timeFrom(pick(o, {QStringLiteral("fromDate"), QStringLiteral("timestamp"),
                                        QStringLiteral("date")}));
        c.open = numFrom(pick(o, {QStringLiteral("open")}));
        c.high = numFrom(pick(o, {QStringLiteral("high")}));
        c.low = numFrom(pick(o, {QStringLiteral("low")}));
        c.close = numFrom(
            pick(o, {QStringLiteral("close"), QStringLiteral("rate"), QStringLiteral("mid")}));
        if (c.close > 0.0) {
            candles << c;
        }
    }
    const auto sortBegin = candles.begin();
    const auto sortEnd = candles.end();
    std::sort(sortBegin, sortEnd,
              [](const Candle &a, const Candle &b) { return a.timestamp < b.timestamp; });
    return candles;
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
    , m_pendingTimer(new QTimer(this))
{
    // Resting limit orders get their own fixed 4 s refresh, independent of the price-poll
    // interval: this list is what tells the user whether an order still waits, triggered
    // or was refused, so its freshness must not depend on a configurable poll setting.
    // emitPendingOrders() starts and stops it with the registry.
    m_pendingTimer->setInterval(4000);
    static_cast<void>(connect(m_pendingTimer, &QTimer::timeout, this,
                              [this] { refreshPendingOrdersReal(); }));

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
                                  // The synthetic feed has no spread and only prices the
                                  // instrument on screen — publish it as that instrument's
                                  // quote so the open-trades table marks simulated trades
                                  // through the same path as real ones. Positions on other
                                  // instruments have no sim price, and the table shows them
                                  // as not-live, which is exactly what they are.
                                  if (m_instrument.isValid() && (price > 0.0)) {
                                      Quote q;
                                      q.bid = price;
                                      q.ask = price;
                                      q.asOf = QDateTime::currentDateTimeUtc();
                                      static_cast<void>(
                                          m_quoteById.insert(m_instrument.instrumentId, q));
                                      emit quotesUpdated();
                                  }
                                  emit priceUpdated(time, price);
                              }));
    static_cast<void>(connect(m_sim, &SimulationEngine::portfolioUpdated,
                              this, &EtoroClient::portfolioUpdated));
    static_cast<void>(connect(m_sim, &SimulationEngine::cashUpdated,
                              this, &EtoroClient::cashUpdated));
    static_cast<void>(connect(m_sim, &SimulationEngine::orderResult,
                              this, &EtoroClient::orderResult));
    static_cast<void>(connect(m_sim, &SimulationEngine::pendingOrdersUpdated,
                              this, &EtoroClient::pendingOrdersUpdated));
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
    // Credential hygiene: the API keys travel as headers on every request, so a
    // cleartext base URL would expose them to anyone on the path. A loopback
    // http URL is fine (the test suite's local mock server); anything else
    // deserves the loudest warning the log has — the config is almost certainly
    // wrong, and these keys can move real money.
    const QUrl base(m_config.baseUrl);
    const bool cleartext = base.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0;
    if (cleartext && !QHostAddress(base.host()).isLoopback()) {
        emit log(QStringLiteral("SECURITY: base URL %1 is not HTTPS — the API keys "
                                "would be sent in cleartext. Check the configuration.")
                     .arg(m_config.baseUrl),
                 true);
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
        // Resting limit orders have their own 4 s timer (see m_pendingTimer) — the price
        // poll cadence is configurable and would otherwise decide how fresh that list is.
    }
}

void EtoroClient::openPosition(const OrderRequest &req)
{
    if (req.amount <= 0.0) {
        emit orderResult(false, QStringLiteral("Amount must be greater than zero."));
        return;
    }
    if (!m_simulated) {
        openPositionReal(req);
    } else if (req.isLimit()) {
        m_sim->placePendingOrder(req);
    } else {
        m_sim->openPosition(req);
    }
}

void EtoroClient::cancelPendingOrder(const QString &orderId)
{
    if (orderId.isEmpty()) {
        emit orderResult(false, QStringLiteral("No pending order selected."));
        return;
    }
    if (m_simulated) {
        m_sim->cancelPendingOrder(orderId);
    } else {
        cancelPendingOrderReal(orderId);
    }
}

void EtoroClient::modifyPendingOrder(const QString &orderId, double triggerRate,
                                     double stopLossAmount, double takeProfitAmount)
{
    if (m_simulated) {
        m_sim->modifyPendingOrder(orderId, triggerRate, stopLossAmount, takeProfitAmount);
        return;
    }
    if (!m_pendingOrders.contains(orderId)) {
        emit orderResult(false, QStringLiteral("Limit order %1 is no longer resting — nothing "
                                              "to adjust.").arg(orderId));
        return;
    }
    // The replacement is placed on the ORDER's own instrument, whatever the app happens to
    // be showing: a limit order is priced off its trigger rate, so it needs no live quote
    // of that instrument (openPositionReal enforces that only limit orders may do this).
    PendingOrder rest = m_pendingOrders.value(orderId);
    if (rest.instrumentId == 0) {
        emit orderResult(false, QStringLiteral("Limit order %1 cannot be adjusted — the app "
                                              "does not know which instrument it is on.")
                                    .arg(orderId));
        return;
    }
    rest.triggerRate = triggerRate;
    rest.stopLossAmount = stopLossAmount;
    rest.takeProfitAmount = takeProfitAmount;

    // No update-order endpoint exists, so: cancel, then re-place with the new values.
    // Cancel FIRST — placing first would leave two live orders for one intended trade
    // if the cancel then failed.
    const QString path = QStringLiteral("/v2/trading/execution%1/orders/%2")
                             .arg(accountSegment(), orderId);
    QNetworkReply *reply = apiDelete(path);
    handleReply(reply, [this, orderId, rest](bool ok, qint32 status, const QJsonDocument &doc,
                                             const QByteArray &raw, const QString &netError) {
        if (!ok) {
            emit orderResult(false,
                             QStringLiteral("Limit order %1 unchanged — eToro would not cancel it "
                                            "for the replacement (HTTP %2): %3. It may have "
                                            "triggered already.")
                                 .arg(orderId)
                                 .arg(status)
                                 .arg(rejectionReason(doc, raw, netError)));
            refreshPendingOrdersReal();
            return;
        }
        m_pendingOrders.remove(orderId);
        emitPendingOrders();
        replacePendingOrderReal(rest);
    });
}

void EtoroClient::replacePendingOrderReal(const PendingOrder &rest)
{
    emit log(QStringLiteral("Limit order cancelled for the adjustment — re-placing %1 %2 at %3 "
                            "with the new SL/TP (it comes back under a new order id).")
                 .arg(rest.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"), rest.symbol)
                 .arg(rest.triggerRate, 0, 'f', trading::priceDecimals(rest.triggerRate)),
             false);
    OrderRequest req;
    req.isBuy = rest.isBuy;
    req.instrumentId = rest.instrumentId;  // the order's own, not whatever is on screen
    req.amount = rest.amount;
    req.leverage = rest.leverage;
    req.stopLossAmount = rest.stopLossAmount;
    req.takeProfitAmount = rest.takeProfitAmount;
    req.trailingStop = rest.trailingStop;
    req.triggerRate = rest.triggerRate;
    openPositionReal(req);  // reports its own success/failure; a failure leaves NO order
}

QList<PendingOrder> EtoroClient::pendingOrders() const
{
    if (m_simulated) {
        return m_sim->pendingOrders();
    }
    // In placement order (the simulation reports the same), so a row does not jump
    // around under the cursor when another order lands or resolves. QHash iteration
    // order is arbitrary, hence the explicit sort.
    QList<PendingOrder> out = m_pendingOrders.values();
    std::sort(out.begin(), out.end(), [](const PendingOrder &a, const PendingOrder &b) {
        return a.submitted < b.submitted;
    });
    return out;
}

void EtoroClient::closePosition(const QString &positionId)
{
    if (positionId.isEmpty()) {
        emit positionClosed(false, QStringLiteral("No position selected."), QString());
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

void EtoroClient::fetchClosedTrades(qint32 weeksBack)
{
    if (m_simulated) {
        m_sim->summarizeMonthly();
        emit closedTradesReady({});  // the simulation keeps no per-trade history
        return;
    }
    // Never stack pagers on the shared-pool history endpoint — but never drop the
    // request either: the details dialog's 13-week fetch used to lose silently
    // against the startup 7-week walk, leaving the dialog showing a shorter window
    // than its own lookback selector. Queue the latest request (last one wins) and
    // run it as soon as the current walk finishes.
    if (m_pnlFetching) {
        m_pnlPendingWeeks = std::clamp(weeksBack, 1, 26);
        emit log(QStringLiteral("Closed-trade P/L walk in progress — queued a "
                                "%1-week refresh behind it…")
                     .arg(m_pnlPendingWeeks),
                 false);
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

QNetworkReply *EtoroClient::apiDelete(const QString &path)
{
    return m_nam->deleteResource(makeRequest(QUrl(m_config.baseUrl + path)));
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
                         .arg(errorText(raw, netError)),
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
                         .arg(errorText(raw, netError)),
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

        const QList<qint32> values = eligibleLeverages(e);
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
    QNetworkReply *reply = m_nam->get(tradeConfigRequest(wantId));
    handleReply(reply, [this, wantId](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (!ok || (m_instrument.instrumentId != wantId)) {
            return;  // transient failure, or the user switched instruments meanwhile
        }
        const InstrumentFees fees = feesFromTradeConfig(doc);
        if (fees.isValid()) {
            static_cast<void>(m_feesById.insert(wantId, fees));  // shared cache (see feesFor/requestFees)
            emit feesUpdated(fees);
        }
    });
}

void EtoroClient::setTradeConfigBaseForTesting(const QString &base)
{
    tradeConfigBase() = base;
}

void EtoroClient::setExtraQuoteInstruments(const QSet<qint64> &instrumentIds)
{
    m_extraQuoteIds = instrumentIds;
}

double EtoroClient::spreadPctFor(const QString &symbol) const
{
    return m_spreadPctById.value(m_idBySymbol.value(symbol, 0), 0.0);
}

QString EtoroClient::instrumentLabel(qint64 instrumentId) const
{
    // The instrument on screen names itself; another one (a re-placed limit order) comes
    // from the id→symbol map, and one outside the app's list reads "#<id>", as closed
    // trades on unlisted instruments already do.
    if ((instrumentId == m_instrument.instrumentId) && !m_instrument.symbol.isEmpty()) {
        return m_instrument.symbol;
    }
    QString listed = m_symbolById.value(instrumentId);  // non-const: returned by move
    if (!listed.isEmpty()) {
        return listed;
    }
    return (instrumentId == m_instrument.instrumentId)
               ? m_config.symbol
               : QStringLiteral("#%1").arg(instrumentId);
}

double EtoroClient::lastRateFor(qint64 instrumentId) const
{
    // The instrument on screen has a per-tick price of its own — fresher than the bulk
    // snapshot — so prefer it. Everything else comes from the last bulk rates poll.
    if ((instrumentId == m_instrument.instrumentId) && (m_lastPrice > 0.0)) {
        return m_lastPrice;
    }
    return m_lastRateById.value(instrumentId, 0.0);
}

InstrumentFees EtoroClient::feesFor(const QString &symbol) const
{
    return m_feesById.value(m_idBySymbol.value(symbol, 0), InstrumentFees{});
}

qint64 EtoroClient::instrumentIdFor(const QString &symbol) const
{
    return m_idBySymbol.value(symbol, 0);
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
    QNetworkReply *reply = m_nam->get(tradeConfigRequest(id));
    handleReply(reply, [this, id, symbol](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                          const QByteArray & /*raw*/, const QString & /*netError*/) {
        static_cast<void>(m_feesInFlight.remove(id));
        if (!ok) {
            return;  // transient — the next plan render will re-request
        }
        const InstrumentFees fees = feesFromTradeConfig(doc);
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
    // e.g. SPX500 all weekend), so we infer it from the rate's `date`: while a market is
    // open eToro keeps re-stamping it, once the session ends the price freezes and the
    // timestamp stops moving (hours over a weekend, ~1h in the daily index break).
    //
    // We compare against the PREVIOUS poll's timestamp, not against the wall clock:
    // the public feed publishes minutes behind real time, so an open market's quote is
    // never "age ≈ 0" and any absolute-age threshold near that is a false "closed" for
    // every instrument at once (see kFirstPollMaxAgeSecs). One bulk rates call (≤100
    // ids) covers every resolved instrument.
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

        // Stand-in for the very first poll, when no previous timestamp exists to compare
        // against: accept a quote younger than this as open. It has to clear eToro's own
        // publishing delay (~6 min on the indices, ~19 min on the .24-7 variants as
        // measured on 2026-07-29) while staying under the multi-hour weekend freeze. Only
        // the first poll leans on it — 60 s later the advancement test has its baseline
        // and corrects any misjudgement (a market frozen mid-break included).
        constexpr qint64 kFirstPollMaxAgeSecs = 1800;
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
                // …and the last mid rate per instrument, so the resting-order list can
                // show what each order's market is doing — including instruments other
                // than the one on screen, which have no per-tick price poll of their own.
                static_cast<void>(m_lastRateById.insert(id, (ask + bid) / 2.0));
            }
            const QDateTime quoteTime = QDateTime::fromString(
                pick(rate, {QStringLiteral("date")}).toString(), Qt::ISODate);
            const QDateTime prevTime = m_lastQuoteTime.value(id);  // invalid on first poll
            bool live = false;
            if (!quoteTime.isValid()) {
                live = true;  // fail open: an unparsable timestamp must not block trading
            } else if (prevTime.isValid()) {
                live = quoteTime > prevTime;  // the feed is still publishing ⇒ session on
            } else {
                live = quoteTime.secsTo(nowUtc) < kFirstPollMaxAgeSecs;  // no baseline yet
            }
            if (quoteTime.isValid()) {
                // Keep the baseline even while judged closed, so the session's first
                // re-stamped quote is recognised as the market opening again.
                static_cast<void>(m_lastQuoteTime.insert(id, quoteTime));
            }
            if (live) {
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
                         .arg(errorText(raw, netError)),
                     true);
            cb({});
            return;
        }
        cb(candlesFrom(doc));
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

qint64 EtoroClient::applyRateRow(const QJsonObject &rate)
{
    const qint64 id = static_cast<qint64>(
        numFrom(pick(rate, {QStringLiteral("instrumentID"), QStringLiteral("instrumentId")})));
    const double bid = numFrom(pick(rate, {QStringLiteral("bid"), QStringLiteral("cvtBid")}));
    if ((id <= 0) || (bid <= 0.0)) {
        return 0;
    }
    const double ask = numFrom(pick(rate, {QStringLiteral("ask"), QStringLiteral("cvtAsk")}));
    Quote q;
    q.bid = bid;
    q.ask = (ask > bid) ? ask : bid;
    // conversionRateBid/Ask turn a quote-currency move into the account currency; they
    // are 1.0 for the USD-quoted instruments and e.g. ~0.127 for HKD-quoted HKG50.
    // This is eToro's own closeConversionRate — using it (rather than the rate as of the
    // position's open) is what makes the P/L of a foreign-quoted instrument exact.
    const double convBid = numFrom(pick(rate, {QStringLiteral("conversionRateBid")}));
    const double convAsk = numFrom(pick(rate, {QStringLiteral("conversionRateAsk")}));
    q.conversionBid = (convBid > 0.0) ? convBid : 1.0;
    q.conversionAsk = (convAsk > 0.0) ? convAsk : 1.0;
    // The row's `date` is the age of the PRICE, which is the whole point of keeping it.
    q.asOf = QDateTime::fromString(pick(rate, {QStringLiteral("date")}).toString(), Qt::ISODate);
    const Quote previous = m_quoteById.value(id);
    // A candle repair that is NEWER than this row must survive it, or a delayed
    // instrument would flip between the live and the delayed price every tick.
    if (previous.fromCandle && previous.asOf.isValid()
        && (!q.asOf.isValid() || (previous.asOf > q.asOf))) {
        Quote kept = previous;
        kept.ask = kept.bid + q.spread();   // the spread stays the live row's
        kept.conversionBid = q.conversionBid;
        kept.conversionAsk = q.conversionAsk;
        static_cast<void>(m_quoteById.insert(id, kept));
        return id;
    }
    static_cast<void>(m_quoteById.insert(id, q));
    return id;
}

void EtoroClient::pollPriceReal()
{
    // The instrument on screen plus every held one, in ONE call. Marking the
    // open-trades rows used to depend on a second bulk call issued per portfolio poll,
    // which left every row but the one on screen as stale as that snapshot.
    // The paper bot's simulated holdings ride along in the same call (REQ-F-029) —
    // the endpoint takes a batch of ids, so more instruments cost no extra request.
    QSet<qint64> want = m_heldInstrumentIds;
    want.unite(m_extraQuoteIds);
    if (m_instrument.isValid()) {
        static_cast<void>(want.insert(m_instrument.instrumentId));
    }
    if (want.isEmpty()) {
        return;
    }
    QStringList ids;
    ids.reserve(want.size());
    for (const qint64 id : want) {
        ids << QString::number(id);
    }
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("instrumentIds"), ids.join(QLatin1Char(',')));
    QNetworkReply *reply = apiGet(QStringLiteral("/v1/market-data/instruments/rates"), q);
    const qint64 wantId = m_instrument.instrumentId;  // guard against an instrument switch
    handleReply(reply, [this, wantId](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString & /*netError*/) {
        if (!ok) {
            return;  // transient; keep the previous quotes
        }
        const QJsonObject shown = applyRatesSnapshot(doc, wantId);
        if ((m_instrument.instrumentId != wantId) || shown.isEmpty()) {
            return;  // stale reply from before a switch, or no row for the shown one
        }
        publishShownPrice(shown, wantId);
    }, /*retriesLeft=*/1);  // ride out a transient 429 on the shared market-data pool
}

QJsonObject EtoroClient::applyRatesSnapshot(const QJsonDocument &doc, qint64 wantId)
{
    // Response may be an array of rate objects or {rates:[...]} / {data:[...]}.
    QJsonArray arr = asArray(doc, {QStringLiteral("rates"), QStringLiteral("data")});
    if (arr.isEmpty() && doc.isObject()) {
        arr.append(doc.object());
    }
    QJsonObject shown;
    for (const auto &v : std::as_const(arr)) {
        const QJsonObject rate = v.toObject();
        if (applyRateRow(rate) == wantId) {
            shown = rate;
        }
    }
    repairStaleQuotes();
    emit quotesUpdated();
    return shown;
}

void EtoroClient::publishShownPrice(const QJsonObject &shownRow, qint64 wantId)
{
    // Keep the bid/ask so orders can price SL/TP off the side the position will
    // actually open at (buy→ask, sell→bid); the mid alone skews the SL by the
    // half-spread, which on a wide-spread CFD is a visible mismatch vs the set amount.
    // Take them from the quote book, so a repaired (candle-derived) price reaches the
    // trade panel and the chart too rather than only the P/L column.
    const Quote quote = m_quoteById.value(wantId);
    if (quote.bid > 0.0) {
        m_lastBid = quote.bid;
    }
    if (quote.ask > 0.0) {
        m_lastAsk = quote.ask;
    }

    // A repaired quote's row carries the DELAYED lastExecution, so its own mid is the
    // only price consistent with the bid/ask just published.
    double price = quote.fromCandle
                       ? ((quote.bid + quote.ask) / 2.0)
                       : numFrom(pick(shownRow, {QStringLiteral("lastExecution"),
                                                 QStringLiteral("currentRate"),
                                                 QStringLiteral("close"), QStringLiteral("mid"),
                                                 QStringLiteral("last")}));
    if ((price <= 0.0) && (quote.bid > 0.0) && (quote.ask > 0.0)) {
        price = (quote.bid + quote.ask) / 2.0;
    }
    if (price > 0.0) {
        m_lastPrice = price;
        emit priceUpdated(QDateTime::currentDateTime(), price);
    }
}

void EtoroClient::repairStaleQuotes()
{
    if (m_simulated) {
        return;
    }
    // At most one candle request per instrument per interval: the repair runs off the
    // 1 s price tick, so without this it would fire a request per tick per position.
    static constexpr qint64 kRepairIntervalMs = 2000;
    // An instrument already being marked from candles is re-read well before its mark
    // could go stale — the alternative (waiting for kQuoteStaleMs) both lets the mark
    // drift for minutes on a fast index and makes the column flicker to "not live"
    // every time the bound is crossed. Only the delayed instruments pay for this.
    static constexpr qint64 kCandleRefreshMs = 5000;
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    QSet<qint64> want = m_heldInstrumentIds;
    if (m_instrument.isValid()) {
        static_cast<void>(want.insert(m_instrument.instrumentId));
    }
    for (const qint64 id : want) {
        const Quote quote = m_quoteById.value(id);
        const qint64 age = quote.ageMs(nowUtc);
        const bool current = (age >= 0)
                             && (age <= (quote.fromCandle ? kCandleRefreshMs
                                                          : trading::kQuoteStaleMs));
        if (current) {
            continue;  // the held price is current — nothing to repair
        }
        if (m_candleRepairInFlight.contains(id)) {
            continue;
        }
        const QDateTime lastTry = m_candleRepairAt.value(id);
        if (lastTry.isValid() && (lastTry.msecsTo(nowUtc) < kRepairIntervalMs)) {
            continue;
        }
        static_cast<void>(m_candleRepairAt.insert(id, nowUtc));
        static_cast<void>(m_candleRepairInFlight.insert(id));
        fetchLatestCandleBid(id);
    }
}

void EtoroClient::fetchLatestCandleBid(qint64 instrumentId)
{
    const QString path =
        QStringLiteral("/v1/market-data/instruments/%1/history/candles/%2/OneMinute/1")
            .arg(instrumentId)
            .arg(m_candleDirection);
    QNetworkReply *reply = apiGet(path, QUrlQuery());
    handleReply(reply, [this, instrumentId](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                           const QByteArray & /*raw*/,
                                           const QString & /*netError*/) {
        static_cast<void>(m_candleRepairInFlight.remove(instrumentId));
        if (!ok) {
            return;  // transient; the next tick tries again
        }
        const QList<Candle> candles = candlesFrom(doc);
        if (candles.isEmpty()) {
            return;
        }
        const Candle &newest = candles.last();  // candlesFrom sorts oldest-first
        if ((newest.close <= 0.0) || !newest.timestamp.isValid()) {
            return;
        }
        // The CURRENT minute's candle is a live partial: its close is the bid as of this
        // request, so it is stamped with the fetch time. A newest candle older than that
        // means the feed itself has stopped (closed market) — keep its own stamp, so the
        // quote reads as old and the table shows the position as not live.
        static constexpr qint64 kOneMinuteMs = 60000;  // named: no precedence question
        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        const qint64 candleAge = newest.timestamp.msecsTo(nowUtc);
        const bool livePartial = (candleAge >= 0) && (candleAge < kOneMinuteMs);
        const QDateTime stamp = livePartial ? nowUtc : newest.timestamp;
        Quote q = m_quoteById.value(instrumentId);
        if (q.asOf.isValid() && (stamp <= q.asOf)) {
            return;  // nothing newer than the price already held
        }
        const double spread = q.spread();
        q.bid = newest.close;   // measured identity: the 1-minute close IS the bid
        q.ask = newest.close + spread;
        q.asOf = stamp;
        q.fromCandle = true;
        static_cast<void>(m_quoteById.insert(instrumentId, q));
        emit quotesUpdated();
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

void EtoroClient::refreshPortfolio()
{
    if (m_simulated) {
        // The simulation is the authority for its own book; ask it to re-publish.
        m_sim->refreshPortfolio();
        return;
    }
    refreshPortfolioReal();
}

void EtoroClient::refreshPortfolioReal()
{
    // WHICH positions are open is decided by /portfolio, not by /pnl.
    //
    // /pnl carries eToro's own unrealised P/L per position (unrealizedPnL.pnL,
    // already net of the closing spread and fees), which is why it is still used
    // below — but it serves a CACHED snapshot (observed ~1.5 h old mid-session).
    // Taking the position SET from that cache meant a trade closed anywhere else
    // — in eToro's own web/app UI, or automatically by its stop-loss/take-profit
    // — kept showing as an open trade here until the cache happened to refresh.
    // /portfolio is the live view, so it is the authority for membership and
    // /pnl only decorates the positions it says are still open.
    //
    // NB: /portfolio takes the segment as accountSegment() gives it ("" for real,
    // "/demo" otherwise); /pnl instead needs an explicit real|demo path element.
    const QString path = QStringLiteral("/v1/trading/info%1/portfolio").arg(accountSegment());
    QNetworkReply *reply = apiGet(path, QUrlQuery());
    handleReply(reply, [this](bool ok, qint32 status, const QJsonDocument &doc,
                              const QByteArray &raw, const QString &netError) {
        if (!ok) {
            emit log(QStringLiteral("Portfolio fetch failed (HTTP %1): %2")
                         .arg(status)
                         .arg(errorText(raw, netError)),
                     true);
            // Deliberately no emit: keep the previous snapshot rather than blanking
            // the table on a transient error. The next poll retries.
            return;
        }
        // The same payload lists the account's PENDING orders, and that list is the
        // broker's own — the only way to see limit orders placed in an earlier session
        // or in eToro's own UI (there is no "list my orders" endpoint).
        mergeBrokerPendingOrders(doc);
        overlayPnlOntoLivePositions(parsePositionsPayload(doc));
    }, /*retriesLeft=*/2);  // ride out a transient 429/5xx rather than logging an error
}

void EtoroClient::mergeBrokerPendingOrders(const QJsonDocument &doc)
{
    // clientPortfolio.orders[]: orderID, instrumentID, isBuy, rate (the trigger rate),
    // amount, leverage, units, stopLossRate / takeProfitRate (RATES, unlike the amounts
    // the app's own registry carries) and isTslEnabled.
    const QJsonObject clientPortfolio =
        pick(doc.object(), {QStringLiteral("clientPortfolio")}).toObject();
    const QJsonValue ordersValue = pick(clientPortfolio, {QStringLiteral("orders")});
    if (!ordersValue.isArray()) {
        return;  // shape not as documented — keep what we know rather than blanking it
    }

    QHash<QString, PendingOrder> fromBroker;
    const QJsonArray orders = ordersValue.toArray();
    for (const auto &value : orders) {
        const QJsonObject o = value.toObject();
        const PendingOrder order = pendingOrderFrom(o);
        if (!order.orderId.isEmpty()) {
            static_cast<void>(fromBroker.insert(order.orderId, order));
        }
    }

    // The broker's list decides what exists. Two exceptions, both about lag: an order
    // submitted seconds ago may not be in this (polled) snapshot yet, and one this app
    // knows the status of keeps that wording. kGraceMs is the window in which a locally
    // known order survives being absent — measured from when it was submitted.
    static constexpr qint64 kGraceMs = 20000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (auto it = m_pendingOrders.cbegin(); it != m_pendingOrders.cend();) {
        const bool known = fromBroker.contains(it.key());
        const bool young = it->submitted.isValid()
                           && ((now - it->submitted.toMSecsSinceEpoch()) < kGraceMs);
        if (known || young) {
            ++it;
            continue;
        }
        it = m_pendingOrders.erase(it);  // gone at the broker: triggered, cancelled or refused
        changed = true;
    }
    for (auto it = fromBroker.cbegin(); it != fromBroker.cend(); ++it) {
        const auto mine = m_pendingOrders.constFind(it.key());
        PendingOrder merged = it.value();
        if (mine != m_pendingOrders.constEnd()) {
            merged.status = mine->status;  // the lookup's wording beats a generic default
            if (merged.symbol.startsWith(QLatin1Char('#'))) {
                merged.symbol = mine->symbol;  // keep a resolved symbol over "#<id>"
            }
            if (*mine == merged) {
                continue;
            }
        }
        m_pendingOrders.insert(it.key(), merged);
        changed = true;
    }
    if (changed) {
        emitPendingOrders();
    }
}

PendingOrder EtoroClient::pendingOrderFrom(const QJsonObject &o) const
{
    PendingOrder order;
    order.orderId = pick(o, {QStringLiteral("orderID"), QStringLiteral("orderId")})
                        .toVariant().toString();
    order.instrumentId = static_cast<qint64>(
        numFrom(pick(o, {QStringLiteral("instrumentID"), QStringLiteral("instrumentId")})));
    // Instruments outside the app's selector still have to be VISIBLE — an order the app
    // hides is an order nobody cancels — so they read as "#<id>", like closed trades.
    order.symbol = m_symbolById.value(order.instrumentId,
                                      QStringLiteral("#%1").arg(order.instrumentId));
    order.isBuy = pick(o, {QStringLiteral("isBuy")}).toBool();
    order.triggerRate = numFrom(pick(o, {QStringLiteral("rate"), QStringLiteral("triggerRate")}));
    order.amount = numFrom(pick(o, {QStringLiteral("amount")}));
    order.leverage = numFrom(pick(o, {QStringLiteral("leverage")}));
    order.trailingStop = pick(o, {QStringLiteral("isTslEnabled")}).toBool();
    order.status = QStringLiteral("Waiting for rate");
    const QString opened = pick(o, {QStringLiteral("openDateTime")}).toString();
    order.submitted = QDateTime::fromString(opened, Qt::ISODateWithMs);
    if (!order.submitted.isValid()) {
        order.submitted = QDateTime::fromString(opened, Qt::ISODate);
    }

    // eToro states the order's SL/TP as RATES; the panel and the editor work in
    // account-currency AMOUNTS, which is rate distance × units. Units are given, but fall
    // back to the notional identity (amount × leverage / rate) when they are not.
    double units = numFrom(pick(o, {QStringLiteral("units")}));
    if ((units <= 0.0) && (order.triggerRate > 0.0)) {
        units = (order.amount * order.leverage) / order.triggerRate;
    }
    const double slRate = numFrom(pick(o, {QStringLiteral("stopLossRate")}));
    const double tpRate = numFrom(pick(o, {QStringLiteral("takeProfitRate")}));
    if (units > 0.0) {
        // A "no stop" order carries a sentinel rate (0, or 1e-05 for a long) rather than
        // a real one; anything on the wrong side of the trigger is such a sentinel.
        const bool slSet = order.isBuy ? ((slRate > 0.0) && (slRate < order.triggerRate))
                                       : (slRate > order.triggerRate);
        const bool tpSet = order.isBuy ? (tpRate > order.triggerRate)
                                       : ((tpRate > 0.0) && (tpRate < order.triggerRate));
        if (slSet) {
            order.stopLossAmount = std::abs(order.triggerRate - slRate) * units;
        }
        if (tpSet) {
            order.takeProfitAmount = std::abs(tpRate - order.triggerRate) * units;
        }
    }
    return order;
}

// Fetch the cached /pnl snapshot and copy eToro's own P/L onto the live set,
// matched by positionId. Positions missing from the snapshot (just opened, cache
// not caught up) keep profitFromApi=false and get the derived estimate in
// finalizePortfolioPl(); positions only IN the snapshot (already closed) are
// ignored, because `live` is what exists.
void EtoroClient::overlayPnlOntoLivePositions(const QList<Position> &live)
{
    const QString path = QStringLiteral("/v1/trading/info/%1/pnl")
                             .arg(m_config.isLive() ? QStringLiteral("real")
                                                    : QStringLiteral("demo"));
    QNetworkReply *reply = apiGet(path, QUrlQuery());
    handleReply(reply, [this, live = live](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                          const QByteArray & /*raw*/,
                                          const QString & /*netError*/) mutable {
        if (ok) {
            QHash<QString, Position> pnlById;
            for (const Position &p : parsePositionsPayload(doc)) {
                if (!p.positionId.isEmpty()) {
                    static_cast<void>(pnlById.insert(p.positionId, p));
                }
            }
            for (Position &p : live) {
                const auto it = pnlById.constFind(p.positionId);
                if (it != pnlById.constEnd() && it->profitFromApi) {
                    p.profit = it->profit;
                    p.profitFromApi = true;
                    p.apiCloseRate = it->apiCloseRate;
                }
            }
        }
        // Even if /pnl failed, the live set is still correct — publish it with
        // locally derived P/L rather than showing nothing.
        finalizePortfolioPl(live);
    }, /*retriesLeft=*/2);
}

QList<Position> EtoroClient::parsePositionsPayload(const QJsonDocument &doc) const
{
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
            // The rate this P/L was marked at (long → bid, short → ask): the
            // exact anchor for re-pricing the figure against later live ticks.
            p.apiCloseRate = numFrom(pick(upnl, {QStringLiteral("closeRate")}));
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
    return positions;
}

void EtoroClient::finalizePortfolioPl(const QList<Position> &positions)
{
    // Remember each open position's own instrument, so closePositionReal() can send
    // the right InstrumentId even for a trade on an instrument other than the one
    // currently shown in the header.
    m_instrumentByPosition.clear();
    m_heldInstrumentIds.clear();
    for (const Position &p : positions) {
        if (!p.positionId.isEmpty() && (p.instrumentId > 0)) {
            static_cast<void>(m_instrumentByPosition.insert(p.positionId, p.instrumentId));
        }
        if (p.instrumentId > 0) {
            // Feeds the per-tick bulk quote poll, so every held instrument — not just
            // the one on screen — has a quote of the current tick to mark against.
            static_cast<void>(m_heldInstrumentIds.insert(p.instrumentId));
        }
    }

    // (Order-open confirmation is handled authoritatively by confirmOrderReal() via
    // the order-lookup endpoint, not by watching this snapshot's position count —
    // that lagged and produced false "opened no position" reports.)

    // Mark every position from the quote book, which the 1 s bulk poll keeps current
    // for exactly these instruments. This used to issue its own rates call and mark
    // only at portfolio-poll time; both the extra call and the coarser cadence are gone.
    QList<Position> priced = positions;
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    for (Position &p : priced) {
        const Quote quote = m_quoteById.value(p.instrumentId);
        if ((p.openRate <= 0.0) || !quote.isValid()) {
            continue;
        }
        // Value per point in the account currency — the same domain identity the
        // positions table and the gauge use (notional/openRate; raw units are in the
        // *quote* currency and mis-scale non-account-currency instruments like HKG50).
        const double perPoint = trading::accountValuePerPoint(p);

        // Expected spread cost to close now. eToro attributes HALF the spread to
        // opening and half to closing (a long sells at the bid = mid − spread/2), so
        // its close dialog shows spread/2 × value-per-point; charging the full spread
        // here was ≈2× eToro's figure (same bug fixed for the opening cost).
        if (quote.spread() > 0.0) {
            p.closingCost = (quote.spread() / 2.0) * perPoint;
        }

        // Only mark against a quote that is actually current. A delayed one would move
        // the figure AWAY from eToro's live number, so in that case eToro's own
        // snapshot value is the better answer and is left standing (the table shows it
        // as not-live); the candle repair usually has a fresh mark by the next tick.
        // An unstamped row (age -1) passes: fail open, as the market-open inference does.
        if (quote.ageMs(nowUtc) <= trading::kQuoteStaleMs) {
            p.profit = trading::positionPnl(p, quote);
            // Re-anchor so the per-tick re-price in the table continues from here.
            p.apiCloseRate = quote.closeRate(p.isBuy);
        }
    }
    emit portfolioUpdated(priced);
    // A position just opened on an instrument the poll didn't cover yet has no quote;
    // m_heldInstrumentIds now includes it, so kick the repair rather than wait a tick.
    repairStaleQuotes();
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
                         .arg(errorText(raw, netError)),
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
                         errorText(raw, netError)));
            startPendingClosedTradesWalk();
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

            // Keep the individual trade (all instruments) for the detail list. Its
            // symbol/listed tag is assigned by nameAndSummarizeTrades once the walk
            // completes, not here — the id→symbol map may still be filling.
            ClosedTrade ct;
            ct.instrumentId = iid;
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
        nameAndSummarizeTrades(acc);
        emitMonthlyPnl(acc);
        emit closedTradesReady(acc->trades);
        startPendingClosedTradesWalk();
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
        // Name the trades now — after the rates round-trip, as late as possible —
        // so the concurrent listed-id resolution has had the whole walk to land.
        nameAndSummarizeTrades(acc);
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
        startPendingClosedTradesWalk();
    }, /*retriesLeft=*/1);
}

void EtoroClient::nameAndSummarizeTrades(const QSharedPointer<PnlAccum> &acc)
{
    for (ClosedTrade &t : acc->trades) {
        const QString sym = m_symbolById.value(t.instrumentId);
        t.listed = !sym.isEmpty();
        t.symbol = t.listed ? sym : QStringLiteral("#%1").arg(t.instrumentId);
        // The aggregated summary stays restricted to the app's listed
        // (selectable) instruments; the account can hold many others.
        if (!t.listed) {
            continue;
        }
        InstrumentPnl &r = acc->bySymbol[t.symbol];
        r.symbol = t.symbol;
        r.trades++;
        r.netProfit += t.netProfit;
        r.fees += t.fees;
    }
}

void EtoroClient::startPendingClosedTradesWalk()
{
    if (m_pnlPendingWeeks <= 0) {
        return;
    }
    const qint32 weeks = m_pnlPendingWeeks;
    m_pnlPendingWeeks = 0;
    fetchClosedTrades(weeks);
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
                // The screener column shows the cap only: sorted ascending, so
                // the highest eligible leverage is the last element.
                const QList<qint32> values = eligibleLeverages(e);
                const qint32 mx = values.isEmpty() ? 0 : values.constLast();
                if ((id > 0) && (mx > 0)) {
                    static_cast<void>(st->maxLevById.insert(id, mx));
                }
            }
        } else {
            emit log(QStringLiteral("Screener leverage lookup failed (HTTP %1): %2 — "
                                    "rows will show leverage as n/a.")
                         .arg(status)
                         .arg(errorText(raw, netError)),
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
            const QList<Candle> candles = candlesFrom(doc);
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

namespace {

// Convert the requested stop-loss / take-profit *amounts* (account currency)
// into absolute rates: a position of `units` gains/loses 1 currency unit per
// (1 / units) of price move, so an X loss/profit is X / units away from the
// open rate — below for longs, above for shorts (mirrored for the target).
// (A stop-loss rate is also REQUIRED by the API once leverage > 1.)
// 5-dp round, same as modifyPositionReal: a 2-dp round is fine on indices
// but destroys the stop on forex rates — EURUSD at 1.1373 with a ~0.0014
// stop distance rounded to 1.14 lands the SL nowhere near the set amount.
void addSlTpRates(QJsonObject &body, const OrderRequest &req, double ref, double units)
{
    if (units <= 0.0) {
        return;
    }
    if (req.stopLossAmount > 0.0) {
        const double dist = req.stopLossAmount / units;
        const double sl = req.isBuy ? (ref - dist) : (ref + dist);
        body[QStringLiteral("stopLossRate")] = std::round(sl * 1e5) / 1e5;
        // eToro trails the stop server-side when the type is "trailing".
        body[QStringLiteral("stopLossType")] =
            req.trailingStop ? QStringLiteral("trailing") : QStringLiteral("fixed");
    }
    if (req.takeProfitAmount > 0.0) {
        const double dist = req.takeProfitAmount / units;
        const double tp = req.isBuy ? (ref + dist) : (ref - dist);
        body[QStringLiteral("takeProfitRate")] = std::round(tp * 1e5) / 1e5;
    }
}

} // namespace

// Price the SL/TP (and units) off the side the position actually opens at — a buy
// fills near the ask, a sell near the bid — not the mid. Using the mid leaves a
// half-spread error, so the SL shown on the open trade drifts from the set amount.
//
// A limit order opens at its TRIGGER rate, not at today's price, so that is the
// reference its SL/TP (and its unit count) must be priced off — otherwise a stop
// set 100 away from the current price lands 100 away from a rate the position
// never opened at.
double EtoroClient::orderReferenceRate(const OrderRequest &req) const
{
    if (req.isLimit()) {
        return req.triggerRate;
    }
    const double sideRate = req.isBuy ? m_lastAsk : m_lastBid;
    if (sideRate > 0.0) {
        return sideRate;
    }
    return (m_lastPrice > 0.0) ? m_lastPrice : m_instrument.currentRate;
}

// UnifiedOrderRequest (POST /v2/trading/execution/orders):
//  * identify the instrument by EXACTLY ONE of symbol/instrumentId — sending
//    both is rejected, so we send instrumentId only;
//  * settlementType is REQUIRED for open orders (SPX500 is a leveraged CFD);
//  * leverage / instrumentId are int32 in the schema.
QJsonObject EtoroClient::baseOrderBody(const OrderRequest &req, qint64 instrumentId,
                                       const QString &orderCurrency)
{
    QJsonObject body;
    body[QStringLiteral("action")] = QStringLiteral("open");
    // Opening transactions only: "buy" opens a long, "sellShort" opens a short.
    // ("sell" / "buyToCover" are the *closing* transactions and are rejected here —
    // positions are closed via the market-close endpoint instead.)
    body[QStringLiteral("transaction")] =
        req.isBuy ? QStringLiteral("buy") : QStringLiteral("sellShort");
    body[QStringLiteral("instrumentId")] = static_cast<qint32>(instrumentId);
    body[QStringLiteral("settlementType")] = QStringLiteral("cfd");
    // "mkt" executes now; "mit" (market-if-touched, eToro's "limit order") rests at
    // the broker until the feed publishes triggerRate or better and executes at market
    // then. triggerRate is REQUIRED for mit and must be absent for mkt.
    body[QStringLiteral("orderType")] =
        req.isLimit() ? QStringLiteral("mit") : QStringLiteral("mkt");
    if (req.isLimit()) {
        // 5-dp round like the SL/TP rates (see addSlTpRates): a rate computed from a
        // percentage ("1% above the market") carries binary float noise, and a broker
        // validating against a tick size has no reason to accept 5858.000000000001.
        body[QStringLiteral("triggerRate")] = std::round(req.triggerRate * 1e5) / 1e5;
    }
    body[QStringLiteral("orderCurrency")] = orderCurrency;
    body[QStringLiteral("leverage")] = static_cast<qint32>(req.leverage);
    return body;
}

// eToro caps the units per single order (eligibility's maxUnitsPerOrder, e.g. 20
// for GOLD). An oversized order is accepted here (an orderId is created) but then
// rejected at execution with a terse "PositionUnits ... MaxAllowed" dialog — the
// requested trade never opens. So shrink an over-cap order to the largest amount
// that fits (shaved 0.5% so a price move before execution can't tip it back over)
// and log the reduction; the "order submitted" message then reports the amount
// actually sent. Skipped while the cap (or a live rate) is still unknown; eToro's
// own validation remains the backstop then — which also covers an order on an
// instrument other than the one on screen, whose cap the app has not queried.
EtoroClient::SizedOrder EtoroClient::applyUnitCap(const OrderRequest &req, qint64 instrumentId,
                                                  double ref, const QString &symbolLabel,
                                                  const QString &orderCurrency)
{
    SizedOrder sized;
    sized.amount = req.amount;
    sized.units = (ref > 0.0) ? ((req.amount * req.leverage) / ref) : 0.0;
    const double maxUnits = (instrumentId == m_instrument.instrumentId)
                                ? m_instrument.maxUnitsPerOrder
                                : 0.0;
    if ((maxUnits <= 0.0) || (sized.units <= maxUnits)) {
        sized.ok = true;
        return sized;
    }
    const double maxAmount = std::floor(((maxUnits * ref) / req.leverage) * 0.995);
    if (maxAmount < 1.0) {
        emit orderResult(false,
            QStringLiteral("%1 %2 not sent — even the smallest order would exceed "
                           "eToro's cap of %3 units per order on this instrument.")
                .arg(req.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                     symbolLabel)
                .arg(maxUnits));
        return sized;  // ok stays false
    }
    emit log(QStringLiteral("%1 %2: %3 %4 at x%5 would be ≈ %6 units — over eToro's "
                            "cap of %7 units per order; order size reduced to %8 %4.")
                 .arg(req.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                      symbolLabel)
                 .arg(req.amount, 0, 'f', 2)
                 .arg(orderCurrency.toUpper())
                 .arg(req.leverage)
                 .arg(sized.units, 0, 'f', 2)
                 .arg(maxUnits)
                 .arg(maxAmount, 0, 'f', 2),
             false);
    sized.amount = maxAmount;
    sized.units = (maxAmount * req.leverage) / ref;  // SL/TP distances follow the new size
    sized.ok = true;
    return sized;
}

void EtoroClient::onOrderSubmitReply(const PendingOrder &rest, const QString &orderCurrency,
                                     bool ok, qint32 status, const QJsonDocument &doc,
                                     const QByteArray &raw, const QString &netError)
{
    const bool isLimit = rest.triggerRate > 0.0;
    if (!ok) {
        emit orderResult(false, QStringLiteral("%1 rejected (HTTP %2): %3")
                                    .arg(isLimit ? QStringLiteral("Limit order")
                                                 : QStringLiteral("Order"))
                                    .arg(status)
                                    .arg(rejectionReason(doc, raw, netError)));
        return;
    }
    // A 200 only means the order was SUBMITTED (an orderId was created) — a
    // market order can still be rejected at execution and open no position.
    // Report it as submitted, then confirm the real outcome via the
    // order-lookup endpoint (confirmOrderReal), not the lagging portfolio.
    const QJsonObject submitted = doc.object();
    const qint64 orderId =
        static_cast<qint64>(numFrom(pick(submitted, {QStringLiteral("orderId")})));

    if (isLimit) {
        registerRestingOrder(rest, orderId);
        return;
    }

    emit orderResult(true,
                     QStringLiteral("%1 order submitted (id %5): %2 %3 %4 — confirming…")
                         .arg(rest.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                         .arg(rest.amount)
                         .arg(orderCurrency.toUpper(), rest.symbol)
                         .arg(orderId));
    if (orderId > 0) {
        // Give execution a moment to register before the first lookup.
        const bool isBuy = rest.isBuy;
        const QString symbolLabel = rest.symbol;
        QTimer::singleShot(1500, this, [this, orderId, isBuy, symbolLabel] {
            confirmOrderReal(orderId, isBuy, symbolLabel, 0);
        });
    }
    refreshPortfolioReal();
    refreshBalanceReal();
}

void EtoroClient::openPositionReal(const OrderRequest &req)
{
    // Which instrument this order is for. Normally the one being traded; a LIMIT order may
    // name another one (re-placing a resting order after an edit), which is safe because a
    // limit order is priced off its own trigger rate and needs no live quote. A MARKET
    // order must not: it would be priced from the shown instrument's bid/ask.
    const qint64 instrumentId =
        (req.instrumentId != 0) ? req.instrumentId : m_instrument.instrumentId;
    if (!req.isLimit() && (instrumentId != m_instrument.instrumentId)) {
        emit orderResult(false, QStringLiteral("A market order can only be placed on the "
                                              "instrument currently being traded."));
        return;
    }

    // The order amount must be in the account currency; prefer the real currency
    // learned from the API over a (possibly stale/mismatched) config value.
    const QString orderCurrency =
        (m_accountCurrency.isEmpty() ? m_config.orderCurrency : m_accountCurrency).toLower();
    const QString symbolLabel = instrumentLabel(instrumentId);
    const double ref = orderReferenceRate(req);

    const SizedOrder sized = applyUnitCap(req, instrumentId, ref, symbolLabel, orderCurrency);
    if (!sized.ok) {
        return;  // over the per-order unit cap even at the minimum size (reported)
    }

    QJsonObject body = baseOrderBody(req, instrumentId, orderCurrency);
    body[QStringLiteral("amount")] = sized.amount;
    addSlTpRates(body, req, ref, sized.units);

    // Everything the pending registry needs is known before the POST goes out; only
    // the broker's orderId is missing, so the reply handler captures the registry
    // entry and the display currency instead of a dozen loose values. Built after
    // the unit-cap shrink above, so `amount` is what was actually sent.
    PendingOrder rest;
    rest.instrumentId = instrumentId;
    rest.symbol = symbolLabel;
    rest.isBuy = req.isBuy;
    rest.triggerRate = req.triggerRate;
    rest.amount = sized.amount;
    rest.leverage = req.leverage;
    rest.stopLossAmount = req.stopLossAmount;
    rest.takeProfitAmount = req.takeProfitAmount;
    rest.trailingStop = req.trailingStop;

    const QString path =
        QStringLiteral("/v2/trading/execution%1/orders").arg(accountSegment());
    QNetworkReply *reply = apiPost(path, body);
    handleReply(reply, [this, rest, orderCurrency](bool ok, qint32 status,
                                                   const QJsonDocument &doc,
                                                   const QByteArray &raw,
                                                   const QString &netError) {
        onOrderSubmitReply(rest, orderCurrency, ok, status, doc, raw, netError);
    });
}

void EtoroClient::registerRestingOrder(const PendingOrder &rest, qint64 orderId)
{
    // A resting order is SUPPOSED to stay pending — possibly for days — so it goes into
    // the pending registry and is status-polled alongside the portfolio, instead of
    // being chased by confirmOrderReal's seconds-long loop. Nothing opened yet either,
    // so portfolio and balance need no refresh here.
    PendingOrder placed = rest;
    placed.orderId = QString::number(orderId);
    placed.status = QStringLiteral("Submitted");
    placed.submitted = QDateTime::currentDateTime();
    if (orderId > 0) {
        m_pendingOrders.insert(placed.orderId, placed);
        emitPendingOrders();  // shows at once, and starts the 4 s status cycle
        // HTTP 200 only means eToro CREATED the order; a validation rejection lands a
        // moment later. One check ahead of the 4 s cycle so a dead order is never
        // presented as resting for even one full cycle.
        const QString id = placed.orderId;
        QTimer::singleShot(1500, this, [this, id] {
            if (m_pendingOrders.contains(id)) {
                lookupPendingOrderReal(id);
            }
        });
    }
    emit orderResult(true, QStringLiteral("Limit %1 %2 accepted by eToro (order id %3): %4 at "
                                          "x%5, resting until the rate reaches %6. eToro executes "
                                          "it even with this app closed.")
                               .arg(placed.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                                    placed.symbol)
                               .arg(orderId)
                               .arg(placed.amount, 0, 'f', 2)
                               .arg(placed.leverage)
                               .arg(placed.triggerRate, 0, 'f',
                                    trading::priceDecimals(placed.triggerRate)));
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

void EtoroClient::cancelPendingOrderReal(const QString &orderId)
{
    // DELETE /v2/trading/execution{seg}/orders/{orderId} — "cancels an order before it
    // is executed". Non-GETs are never auto-retried (REQ-N-003): a retried cancel could
    // race an execution, and the pending refresh reports the true state anyway.
    const QString path = QStringLiteral("/v2/trading/execution%1/orders/%2")
                             .arg(accountSegment(), orderId);
    QNetworkReply *reply = apiDelete(path);
    handleReply(reply, [this, orderId](bool ok, qint32 status, const QJsonDocument &doc,
                                       const QByteArray &raw, const QString &netError) {
        if (ok) {
            const PendingOrder gone = m_pendingOrders.take(orderId);
            emit orderResult(true, QStringLiteral("Limit order %1 cancelled%2.")
                                       .arg(orderId,
                                            gone.symbol.isEmpty()
                                                ? QString()
                                                : QStringLiteral(" (%1 %2)")
                                                      .arg(gone.isBuy ? QStringLiteral("BUY")
                                                                      : QStringLiteral("SELL"),
                                                           gone.symbol)));
            emitPendingOrders();
            return;
        }
        // A cancel can legitimately fail because the order just executed; the status
        // refresh below then reports what really happened, so this stays a plain error.
        emit orderResult(false, QStringLiteral("Could not cancel limit order %1 (HTTP %2): %3")
                                    .arg(orderId)
                                    .arg(status)
                                    .arg(rejectionReason(doc, raw, netError)));
        refreshPendingOrdersReal();
    });
}

void EtoroClient::refreshPendingOrdersReal()
{
    if (m_pendingOrders.isEmpty()) {
        return;
    }
    // At most two orders per 4 s tick, continuing where the last tick stopped: the
    // lookup's rate pool is shared with the closed-trade endpoints, and refreshing every
    // order on every tick would spend the whole budget once a few orders rest.
    static constexpr qint32 kOrdersPerTick = 2;
    QStringList ids = m_pendingOrders.keys();
    std::sort(ids.begin(), ids.end());  // QHash order is arbitrary; the cursor needs stability
    if (m_pendingCursor >= static_cast<qint32>(ids.size())) {
        m_pendingCursor = 0;
    }
    const qint32 count = std::min<qint32>(kOrdersPerTick, static_cast<qint32>(ids.size()));
    for (qint32 i = 0; i < count; ++i) {
        lookupPendingOrderReal(ids.at((m_pendingCursor + i) % ids.size()));
    }
    m_pendingCursor = static_cast<qint32>((m_pendingCursor + count) % ids.size());
}

void EtoroClient::emitPendingOrders()
{
    emit pendingOrdersUpdated(pendingOrders());
    if (m_simulated || (m_pendingTimer == nullptr)) {
        return;  // the simulation is authoritative and emits on every change itself
    }
    // Run the 4 s cycle only while something rests: an empty registry has nothing to
    // poll, and a timer left running would burn rate budget on nothing.
    if (m_pendingOrders.isEmpty()) {
        m_pendingTimer->stop();
        m_pendingCursor = 0;
    } else if (!m_pendingTimer->isActive()) {
        m_pendingTimer->start();
    }
}

void EtoroClient::lookupPendingOrderReal(const QString &orderId)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("orderId"), orderId);
    const QString path =
        QStringLiteral("/v2/trading/info%1/orders:lookup").arg(accountSegment());
    QNetworkReply *reply = apiGet(path, query);
    handleReply(reply, [this, orderId](bool ok, qint32 /*status*/, const QJsonDocument &doc,
                                       const QByteArray & /*raw*/, const QString & /*netError*/) {
        // A transient failure is not evidence about the order — leave it resting
        // and re-read it on the next refresh.
        if (ok) {
            applyPendingOrderStatus(orderId, doc);
        }
    }, /*retriesLeft=*/1);
}

void EtoroClient::applyPendingOrderStatus(const QString &orderId, const QJsonDocument &doc)
{
    if (!m_pendingOrders.contains(orderId)) {
        return;  // cancelled between the request and its reply
    }
    const QJsonObject st = pick(doc.object(), {QStringLiteral("status")}).toObject();
    const qint32 statusId = static_cast<qint32>(numFrom(pick(st, {QStringLiteral("id")})));
    const QString statusName = pick(st, {QStringLiteral("name")}).toString();
    const PendingOrder rest = m_pendingOrders.value(orderId);
    const QString side = rest.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");

    // Status ids (GetOrderInfoStatus): 3 Filled / 5 PartiallyFilled = it triggered and
    // opened; 4 Rejected, 7 Canceled, 8 Expired, 9/10 the partially-filled variants =
    // it is gone; 1/2/6/11/12 = still resting.
    static const QSet<qint32> kFilled{3, 5};
    static const QSet<qint32> kGone{4, 7, 8, 9, 10};
    if (kFilled.contains(statusId)) {
        m_pendingOrders.remove(orderId);
        emit orderResult(true, QStringLiteral("Limit %1 %2 triggered at %3 — order %4 filled, "
                                              "the position is now open.")
                                   .arg(side, rest.symbol)
                                   .arg(rest.triggerRate, 0, 'f',
                                        trading::priceDecimals(rest.triggerRate))
                                   .arg(orderId));
        emitPendingOrders();
        refreshPortfolioReal();
        refreshBalanceReal();
        return;
    }
    if (kGone.contains(statusId)) {
        m_pendingOrders.remove(orderId);
        emitPendingOrders();
        // 7 Canceled / 9 CanceledPartiallyFilled is normally the user's own cancel and is
        // already reported by cancelPendingOrder; a REJECTION or expiry is news, and
        // eToro's own reason for it is the one thing that explains why the order the app
        // showed as resting will never open — so it goes out as an error, with that text.
        const QString why = pick(st, {QStringLiteral("errorMessage")}).toString();
        const qint32 errorCode =
            static_cast<qint32>(numFrom(pick(st, {QStringLiteral("errorCode")})));
        if ((statusId == 7) || (statusId == 9)) {
            emit log(QStringLiteral("Limit %1 %2 (order %3) is no longer resting — %4.")
                         .arg(side, rest.symbol, orderId,
                              statusName.isEmpty() ? QStringLiteral("cancelled") : statusName),
                     false);
            return;
        }
        emit orderResult(false,
                         QStringLiteral("eToro did NOT accept the limit %1 %2 at %3 (order %4): "
                                        "%5%6. Nothing is resting for it any more.")
                             .arg(side, rest.symbol)
                             .arg(rest.triggerRate, 0, 'f',
                                  trading::priceDecimals(rest.triggerRate))
                             .arg(orderId,
                                  why.isEmpty() ? (statusName.isEmpty()
                                                       ? QStringLiteral("rejected")
                                                       : statusName)
                                                : why)
                             .arg((errorCode != 0) ? QStringLiteral(" [code %1]").arg(errorCode)
                                                   : QString()));
        return;
    }
    // Still resting: keep the broker's own wording visible in the table.
    if (!statusName.isEmpty() && (statusName != rest.status)) {
        PendingOrder updated = rest;
        updated.status = statusName;
        m_pendingOrders.insert(orderId, updated);
        emitPendingOrders();
    }
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
    handleReply(reply, [this, positionId](bool ok, qint32 status, const QJsonDocument &doc,
                                          const QByteArray &raw, const QString &netError) {
        if (ok) {
            emit positionClosed(true, QStringLiteral("Position %1 closed.").arg(positionId),
                                positionId);
            refreshPortfolioReal();
            refreshBalanceReal();
        } else {
            // The BROKER's reason first: "position already closed" is what the reader
            // needs, and Qt's own "server replied: <phrase>" is what they used to get,
            // because a transport string is always present on an HTTP error.
            emit positionClosed(false,
                                QStringLiteral("Close failed (HTTP %1): %2")
                                    .arg(status)
                                    .arg(rejectionReason(doc, raw, netError)),
                                positionId);
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
