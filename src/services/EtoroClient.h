// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_ETOROCLIENT_H
#define TRADINGAPP_ETOROCLIENT_H

#include "domain/Models.h"
#include "services/Config.h"
#include "services/JsonHttp.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QStringList>
#include <functional>

struct PnlAccum;   // paging accumulator for the monthly-P/L fetch (defined in the .cpp)
struct ScanState;  // per-run state for the leverage screener (defined in the .cpp)

class SimulationEngine;

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QTimer;
class QJsonDocument;
class QJsonObject;
class QUrl;
class QUrlQuery;

// Talks to the official eToro public API (https://public-api.etoro.com/api).
//
// When credentials are configured it performs real REST calls (against the
// demo or real account depending on Config::mode). When no credentials are
// present it transparently delegates to the self-contained SimulationEngine
// so the UI is always fully functional. Callers use the same API in both
// cases and react to the same signals. Non-broker concerns live elsewhere:
// the public web feeds in MarketFeeds, the AI synthesis in AiAdvisor.
class EtoroClient : public QObject
{
    Q_OBJECT
public:
    explicit EtoroClient(Config config, QObject *parent = nullptr);

    // Resolve the instrument, seed history and begin polling.
    void start();

    // Switch to a different instrument at runtime (re-resolve / re-seed / re-poll).
    void setSymbol(const QString &symbol);

    // The instruments selectable in the app; used to resolve their ids and to
    // restrict the (account-wide) real portfolio to just these instruments.
    void setTradableSymbols(const QStringList &symbols);

    [[nodiscard]] const Instrument &instrument() const & { return m_instrument; }
    // Latest bid/ask (0 until a real quote arrives, e.g. in simulation mode): a buy
    // opens near the ask, a sell near the bid, so their gap is the spread the trade
    // crosses on opening. Used by the UI to estimate the opening cost.
    [[nodiscard]] double lastBid() const { return m_lastBid; }
    [[nodiscard]] double lastAsk() const { return m_lastAsk; }
    [[nodiscard]] const Config &config() const & { return m_config; }

    // Live two-sided quotes keyed by instrumentId, for the instrument on screen AND
    // every instrument the account holds a position in — one bulk call per poll tick
    // keeps them all current. Each carries eToro's own stamp for the price, so a
    // consumer can tell a live mark from a delayed publication (Quote::ageMs).
    [[nodiscard]] const QHash<qint64, Quote> &quotes() const & { return m_quoteById; }

    // Instruments to keep quoting per tick even though the ACCOUNT holds nothing in
    // them: the paper-trading bot's simulated positions (REQ-F-029). They join the
    // one bulk rates call the poll already makes, so a simulated trade is marked
    // from a quote of this tick — exactly like a real one — instead of at the much
    // slower tradeability cadence. Read-only interest: nothing here can place an
    // order, and an empty set restores the previous behaviour.
    void setExtraQuoteInstruments(const QSet<qint64> &instrumentIds);

    // The per-unit rollover fees come from eToro's PUBLIC trade-config host, which is
    // not the API base and therefore not covered by Config::baseUrl. Tests redirect it
    // here — the same hook AiAdvisor, MarketFeeds and EconomicCalendar carry, and for
    // the same reason: a fee path that can only be exercised against the real host is
    // one that is never exercised.
    // static, and that is the honest signature: the redirect is held in one
    // function-local static behind tradeConfigBase(), so it applies to EVERY client in
    // the process, not to the instance it happens to be called on.
    static void setTradeConfigBaseForTesting(const QString &base);

    // Live spread (percent of mid) for any listed symbol, from the most recent
    // bulk rates snapshot (the periodic tradeability refresh keeps it warm for
    // every resolved instrument). 0 while unknown.
    [[nodiscard]] double spreadPctFor(const QString &symbol) const;
    // Cached per-unit rollover fees for any listed symbol (invalid while unknown).
    [[nodiscard]] InstrumentFees feesFor(const QString &symbol) const;
    // Last known rate for ANY instrument by id — the live per-tick price for the one on
    // screen, otherwise the mid of the last bulk rates snapshot. 0 while unknown. Lets the
    // resting-order list show what each order's own market is doing, including instruments
    // that are not the one being traded.
    [[nodiscard]] double lastRateFor(qint64 instrumentId) const;

    // The instrumentId the startup search resolved for an app symbol, 0 while it
    // is still unresolved. The resolutions land independently and asynchronously,
    // so this is also how a test observes that one of them has been APPLIED —
    // seeing the search response go out over the wire says nothing about whether
    // the client has processed it yet (TS-CLI-005).
    [[nodiscard]] qint64 instrumentIdFor(const QString &symbol) const;
    // Fetch the rollover fees for a listed symbol if they aren't cached yet
    // (public etorostatic feed — cheap, outside the rate-limited API). The result
    // arrives via instrumentFeesUpdated; a no-op in simulation mode.
    void requestFees(const QString &symbol);


    // Open a position on the current instrument (see OrderRequest): req.amount is the
    // cash to invest in the order currency, the SL/TP are account-currency amounts at
    // which the position auto-closes (0 = none).
    //
    // req.triggerRate == 0 places a MARKET order (eToro orderType "mkt"): it executes
    // at the current price. req.triggerRate > 0 places what eToro's UI calls a LIMIT
    // order (API orderType "mit", market-if-touched): eToro itself holds the order until
    // the instrument's rate reaches the trigger and only then executes at market — so it
    // fires with this app closed and off the broker's own feed rather than the app's
    // (minutes-delayed) polled quotes. The SL/TP amounts of a limit order are converted
    // to rates off the TRIGGER rate, since that — not today's price — is where the
    // position will open.
    void openPosition(const OrderRequest &req);
    // Cancel a resting limit order before it executes. Reports the outcome through
    // orderResult and re-emits pendingOrdersUpdated.
    void cancelPendingOrder(const QString &orderId);
    // Change a resting limit order's trigger rate and SL/TP amounts. eToro's public API
    // has NO update-order endpoint (PATCH exists for OPEN positions only), so this
    // CANCELS the order and re-places it with the new values: the order comes back with
    // a NEW orderId, and for a moment nothing rests at the broker. Both facts are
    // reported through orderResult. Re-placing reuses the current instrument's data, so
    // an order on a different instrument is refused rather than mis-placed — select that
    // instrument first.
    void modifyPendingOrder(const QString &orderId, double triggerRate,
                            double stopLossAmount, double takeProfitAmount);
    // The limit orders this app placed and that are still resting at the broker, in
    // placement order. The API has no "list my open orders" endpoint, so this is the
    // app's own registry, refreshed from the authoritative per-order lookup.
    [[nodiscard]] QList<PendingOrder> pendingOrders() const;
    void closePosition(const QString &positionId);
    // Change the stop-loss / take-profit *rates* on an open position (0 clears that
    // leg). trailingStop only matters when a stop-loss rate is set.
    void modifyPosition(const QString &positionId, double stopLossRate,
                        double takeProfitRate, bool trailingStop);
    // Walk the closed-trade history `weeksBack` weeks back (clamped to 1..26).
    // Emits BOTH the aggregated monthlyPnlReady summary for the window and
    // closedTradesReady with the individual trades, each carrying open/close
    // spread-cost estimates priced from the instruments' current spreads.
    // While a walk is paging, the latest overlapping request is queued and runs
    // right after it (never dropped, never stacked on the shared rate pool).
    void fetchClosedTrades(qint32 weeksBack);

    // Re-read the open positions now instead of waiting for the next poll, and
    // emit portfolioUpdated. Useful after trading elsewhere (eToro's own UI), and
    // the entry point the open-trades regression test drives.
    void refreshPortfolio();
    // Re-run the market-open inference now (the poll loop does this sparingly on its own
    // tick; tests and a user-facing "re-check" need it on demand). Simulation has no
    // session boundaries, so it is a no-op there.
    void refreshTradeability();

    // Scan every tradable instrument for its max leverage + a recent close series,
    // so the UI can rank them by leverage and compute a buy/sell signal for each.
    // Rows arrive progressively via screenerRow; screenerFinished ends the run. A
    // scan already in progress is ignored (one at a time).
    void scanInstruments();

signals:
    void ready(const Instrument &instrument);
    // Instrument resolution gave up (after retries): the trade panel must not
    // keep trading the previously resolved instrument.
    void resolveFailed(const QString &symbol);
    // Per-unit overnight/weekend rollover fees for the current instrument.
    void feesUpdated(const InstrumentFees &fees);
    // Rollover fees fetched for an arbitrary listed symbol (see requestFees).
    void instrumentFeesUpdated(const QString &symbol, const InstrumentFees &fees);
    void historyReady(const QList<Candle> &candles);
    void priceUpdated(const QDateTime &time, double price);
    // The quote book changed (bulk poll, or a stale row re-based on the candle feed).
    // Carries no payload: consumers read quotes() and re-price in place, which keeps
    // the per-tick open-trades refresh allocation-free (REQ-N-006).
    void quotesUpdated();
    void portfolioUpdated(const QList<Position> &positions);
    void cashUpdated(double available, const QString &currency);
    // EUR per 1 USD (from the EURUSD instrument), so the USD-based account figures
    // can be displayed in euro. Emitted whenever a fresh rate is fetched.
    void fxRateUpdated(double eurPerUsd);
    void orderResult(bool ok, const QString &message);
    // The limit orders still resting at the broker, after every change (placed,
    // cancelled, triggered, or a status refresh). Empty means "none outstanding".
    void pendingOrdersUpdated(const QList<PendingOrder> &orders);
    // positionId travels WITH the result so the window can drop the row at once rather
    // than waiting for a portfolio poll that is measurably behind. Empty only when no
    // position was named at all.
    void positionClosed(bool ok, const QString &message, const QString &positionId);
    void monthlyPnlReady(const MonthlyPnl &summary);
    void monthlyPnlFailed(const QString &error);
    // The individual closed trades behind the latest history walk (newest first,
    // ALL instruments — each row is flagged listed/unlisted). Emitted alongside
    // monthlyPnlReady; empty in simulation mode (no per-trade history there).
    void closedTradesReady(const QList<ClosedTrade> &trades);
    // Leverage-screener results: one row per instrument as its data arrives,
    // periodic progress (done/total), and a one-shot finished at the end.
    void screenerRow(const ScreenerRow &row);
    void screenerProgress(int done, int total);
    void screenerFinished();
    void log(const QString &message, bool isError);
    // Leverage multipliers the current instrument allows (sorted ascending).
    void leverageOptions(const QList<int> &values);
    // App symbols whose market is currently open (inferred from live quote freshness).
    // The UI uses it to disable BUY/SELL when the selected instrument's market is closed.
    // A symbol absent from the set is closed; a never-emitted set means "not yet known"
    // (the UI then allows trading, so it never blocks before the first check lands).
    void tradeabilityUpdated(const QSet<QString> &tradeableSymbols);

private:
    // ---- shared REST plumbing (JsonHttp does the reply/retry handling) ----
    using JsonHandler = JsonHttp::Handler;
    // Build a request to `url` with the shared auth + tracing headers (x-api-key,
    // x-user-key, a fresh x-request-id, Accept: application/json).
    [[nodiscard]] QNetworkRequest makeRequest(const QUrl &url) const;
    QNetworkReply *apiGet(const QString &path, const QUrlQuery &query);
    QNetworkReply *apiPost(const QString &path, const QJsonObject &body);
    QNetworkReply *apiPatch(const QString &path, const QJsonObject &body);
    QNetworkReply *apiDelete(const QString &path);
    // retriesLeft > 0 auto-retries an idempotent GET on a transient failure — HTTP 429
    // or a 5xx server hiccup (500/502/503/504) — waiting out the server's Retry-After /
    // RateLimit-Reset (or a short default backoff) before re-issuing with a fresh
    // x-request-id. Non-GET requests are never auto-retried. Default 0 = no retry.
    void handleReply(QNetworkReply *reply, JsonHandler cb, qint32 retriesLeft = 0);

    // ---- real-mode implementation ----------------------------------------
    void resolveInstrumentReal();
    void retryResolveOrGiveUp(const QString &symbol);
    void fetchFeesReal();
    void resolveListedInstrumentIds();  // learn id<->symbol for every tradable instrument
    void fetchLeverageReal();  // query the instrument's allowed leverage values
    // One bulk market-rates call over every resolved instrument id → the set of app
    // symbols whose market is currently open, inferred from whether the quote's `date`
    // ADVANCED since the previous poll (a frozen `date` means the price no longer
    // updates, i.e. the market is closed). Emitted via tradeabilityUpdated.
    // eligibility.allowOpenPosition is unusable here — it is a static account
    // permission that stays true even when the exchange is closed.
    void refreshTradeabilityReal();
    void fetchHistoryReal();
    // Fetch `count` candles at `interval` (ascending), then hand them to cb (empty on
    // failure). fetchHistoryReal merges a coarse month with a fine recent window.
    void fetchCandles(const QString &interval, qint32 count,
                      std::function<void(QList<Candle>)> cb);
    // ONE bulk rates call per tick over the instrument on screen plus every instrument
    // the account holds a position in: it feeds the quote book (so every open-trades row
    // is marked from a quote of THIS tick, not from the last portfolio snapshot) and the
    // shown instrument's own price/bid/ask for the chart and the trade panel.
    void pollPriceReal();
    // Fold a bulk-rates reply into the quote book (repairing the over-age rows and
    // announcing the change), and return the row of instrument `wantId` — empty when the
    // reply carried none. Split out of the reply handler to keep both off the metrics gate.
    QJsonObject applyRatesSnapshot(const QJsonDocument &doc, qint64 wantId);
    // Publish the shown instrument's own price/bid/ask from the quote book (+ the row's
    // lastExecution, which is only usable while that row is the live one).
    void publishShownPrice(const QJsonObject &shownRow, qint64 wantId);
    // Fold one rates row into the quote book. Returns the instrument id, 0 when the row
    // is unusable. Deliberately does not touch m_lastQuoteTime: the market-open
    // inference owns that baseline and compares consecutive TRADEABILITY polls.
    qint64 applyRateRow(const QJsonObject &rate);
    // eToro publishes the rates row of some instruments minutes behind real time (the
    // .24-7 index variants are the worst: ~6-12 min, growing through the session) while
    // its own UI is live — so a position marked off that row shows a P/L that differs
    // from eToro's by whatever the price did in between. The candle feed for the SAME
    // instrument IS live and its 1-minute close is exactly the bid, so re-base any
    // over-age row on the newest candle, keeping the row's spread for the other side.
    void repairStaleQuotes();
    void fetchLatestCandleBid(qint64 instrumentId);  // one candle -> quote book
    // Fetch the EURUSD rate (instrument 1) and emit fxRateUpdated. Real mode only.
    void fetchEurUsd();
    void refreshPortfolioReal();
    // Parse the open-position array out of a /portfolio or /pnl payload (the two
    // share one shape). Positions on instruments outside the app's list are
    // dropped. Shared so the live set and the P/L snapshot cannot drift apart.
    [[nodiscard]] QList<Position> parsePositionsPayload(const QJsonDocument &doc) const;
    // Fold the account's PENDING orders (clientPortfolio.orders[]) from the same
    // /portfolio payload into the registry. This — not the app's own memory of what it
    // submitted — is what makes limit orders placed in an earlier session or in eToro's
    // own UI visible: the API has no "list my orders" endpoint, but the portfolio
    // breakdown carries them. The broker's list decides what exists, except that an
    // order submitted seconds ago survives being absent from the (polled, lagging)
    // snapshot, and a status this app already looked up is kept.
    void mergeBrokerPendingOrders(const QJsonDocument &doc);
    // One clientPortfolio.orders[] entry as a PendingOrder: the SL/TP arrive as RATES
    // there and are converted back to the account-currency amounts the panel works in.
    [[nodiscard]] PendingOrder pendingOrderFrom(const QJsonObject &o) const;
    // Overlay eToro's own per-position P/L (from the /pnl snapshot) onto the LIVE
    // position set, then finalize. `live` decides which positions exist; /pnl only
    // contributes profit / profitFromApi / apiCloseRate for the ones still open.
    void overlayPnlOntoLivePositions(const QList<Position> &live);
    // Fill in each open position's P/L from live rates for all held instruments,
    // then emit portfolioUpdated (the payload has no per-position P/L).
    void finalizePortfolioPl(const QList<Position> &positions);
    void refreshBalanceReal();
    // Fetch one trade-history page and fold it into acc, then recurse to the next
    // page until the API returns an empty page (history is newest-first).
    void fetchTradeHistoryPageReal(const QSharedPointer<PnlAccum> &acc);
    // Walk complete: price the trades' open/close cost estimates from one bulk
    // rates call (current spreads), then emit the summary and the trade list.
    void finishTradeHistory(const QSharedPointer<PnlAccum> &acc);
    // Name each trade from the id→symbol map and aggregate the listed ones into
    // acc->bySymbol. Runs when the walk COMPLETES: the listed-id resolution races
    // the history pages at startup, and naming per page froze "#<id>" onto trades
    // whose resolution landed a moment later (SPX500-only summary in the field).
    void nameAndSummarizeTrades(const QSharedPointer<PnlAccum> &acc);
    void emitMonthlyPnl(const QSharedPointer<PnlAccum> &acc);  // build MonthlyPnl + emit
    void startPendingClosedTradesWalk();  // run the queued lookback, if any
    // ---- leverage screener -----------------------------------------------
    void scanInstrumentsReal();
    // Fetch the next queued instrument's candles, emit its screenerRow, then recurse
    // to the following one — sequential so the market-data rate budget isn't burst.
    void fetchScanCandle(const QSharedPointer<ScanState> &st);

    void openPositionReal(const OrderRequest &req);
    // The rate a new order's units and SL/TP are priced off: a buy fills near the
    // ask, a sell near the bid (mid / instrument rate as fallbacks) — and a limit
    // order at its own trigger, since that is where the position will open.
    [[nodiscard]] double orderReferenceRate(const OrderRequest &req) const;
    // The UnifiedOrderRequest body common to market and limit orders. The amount
    // and the SL/TP rates are added by openPositionReal, after the unit-cap shrink.
    // static: built purely from its arguments — it must not depend on client state.
    [[nodiscard]] static QJsonObject baseOrderBody(const OrderRequest &req, qint64 instrumentId,
                                                   const QString &orderCurrency);
    // The order size after eToro's per-order unit cap (see openPositionReal):
    // ok=false means even the smallest order exceeds the cap (already reported).
    struct SizedOrder {
        bool ok = false;
        double amount = 0.0;
        double units = 0.0;
    };
    SizedOrder applyUnitCap(const OrderRequest &req, qint64 instrumentId, double ref,
                            const QString &symbolLabel, const QString &orderCurrency);
    // The order POST's reply: report the submission, take a resting limit order
    // into the registry, schedule the order-lookup confirmation of a market order.
    void onOrderSubmitReply(const PendingOrder &rest, const QString &orderCurrency, bool ok,
                            qint32 status, const QJsonDocument &doc, const QByteArray &raw,
                            const QString &netError);
    // Take a just-accepted limit order into the pending registry and report it. Split
    // out of openPositionReal's reply handler so that function stays within its
    // complexity budget (and so the reply lambda captures one value, not a dozen).
    void registerRestingOrder(const PendingOrder &rest, qint64 orderId);
    void cancelPendingOrderReal(const QString &orderId);
    // Second half of modifyPendingOrder: re-place `rest` (already cancelled at the
    // broker) with its new trigger/SL/TP. Separate so the cancel's reply handler stays
    // a two-liner and the "the old order is gone" reporting lives in one place.
    void replacePendingOrderReal(const PendingOrder &rest);
    // Refresh every registered resting order from the authoritative order lookup:
    // report and drop the ones that filled, were rejected, cancelled or expired, and
    // keep the still-waiting ones with the broker's own status wording. Driven from
    // the poll timer (sparingly — the lookup shares a 60/60s rate pool).
    void refreshPendingOrdersReal();
    void lookupPendingOrderReal(const QString &orderId);  // one order's status, folded in below
    // Publish the resting-order list AND match the 4 s refresh timer to it (running only
    // while something is actually resting). Every real-mode change goes through here so
    // the timer can never be left running on an empty registry — or stopped on a full one.
    void emitPendingOrders();
    // Fold ONE order-lookup reply into the pending registry: report and drop a filled
    // order (refreshing portfolio + balance), drop a rejected/cancelled/expired one,
    // and otherwise keep the broker's own status wording on the still-resting entry.
    void applyPendingOrderStatus(const QString &orderId, const QJsonDocument &doc);
    // Confirm a just-submitted order via the authoritative order-lookup endpoint. A
    // 200 from the order POST only means "submitted"; this polls orders:lookup by
    // orderId until the status is terminal — reporting the actual opened position, or
    // a genuine rejection with eToro's reason, and never a false "opened nothing"
    // just because the (lagging) portfolio snapshot hasn't caught up yet. attempt is
    // the poll count so far (0 on the first call); it self-reschedules while pending.
    void confirmOrderReal(qint64 orderId, bool isBuy, const QString &symbolLabel, qint32 attempt);
    void closePositionReal(const QString &positionId);
    void modifyPositionReal(const QString &positionId, double stopLossRate,
                            double takeProfitRate, bool trailingStop);
    [[nodiscard]] QString accountSegment() const;  // "" for real, "/demo" for demo
    // Display name for any instrument id: its resolved symbol where known, else "#<id>".
    [[nodiscard]] QString instrumentLabel(qint64 instrumentId) const;

    // ---- simulation orchestration ------------------------------------------
    // The synthetic feed + virtual account live in SimulationEngine; this emits
    // the mode banner/ready, publishes the opening snapshot and starts polling.
    // resetAccount=true wipes cash/positions (initial start); false keeps them
    // across an instrument switch so open trades on all instruments are retained.
    void startSimulation(bool resetAccount = true);

    void onPollTimeout();

    Config m_config;
    QNetworkAccessManager *m_nam = nullptr;
    JsonHttp *m_http = nullptr;          // shared reply/retry plumbing
    SimulationEngine *m_sim = nullptr;   // no-credentials fallback implementation
    QTimer *m_pollTimer = nullptr;
    Instrument m_instrument;
    bool m_simulated = false;
    double m_lastPrice = 0.0;
    double m_lastBid = 0.0;   // latest bid — a sell opens near here
    double m_lastAsk = 0.0;   // latest ask — a buy opens near here
    double m_eurPerUsd = 0.0; // EUR per 1 USD (0 = not yet fetched)
    QString m_accountCurrency;  // real account currency learned from the API (e.g. "USD")
    qint32 m_pollCount = 0;  // throttles portfolio/balance refresh vs. price polling
    qint32 m_resolveRetries = 0;  // transient search failures get a couple of auto-retries
    qint32 m_fxTick = 0;     // throttles the (slow-moving) EURUSD fetch vs. price polling
    qint32 m_tradeTick = 0;  // throttles the market-open (tradeability) refresh; only
                          // advances once instrument ids are resolved so the first
                          // check fires promptly, then ~every 60 ticks

    QStringList m_tradableSymbols;      // instruments selectable in the app
    QHash<qint64, QString> m_symbolById; // resolved instrumentId -> tradable symbol
    QHash<QString, qint64> m_idBySymbol; // the reverse map, for per-symbol lookups
    QHash<qint64, InstrumentFees> m_feesById;  // rollover fees per instrument (cached)
    QSet<qint64> m_feesInFlight;               // fee fetches already running

    // Live quote per instrument (see quotes()), plus the bookkeeping of the candle
    // repair: which instruments are held (so the bulk poll knows what to ask for),
    // which candle fetches are in flight, and when each id was last repaired — the
    // repair must not turn into a per-tick candle request per position.
    QHash<qint64, Quote> m_quoteById;
    QSet<qint64> m_heldInstrumentIds;
    // Instruments the paper bot holds simulated positions in (see
    // setExtraQuoteInstruments): quoted alongside the held ones, never traded.
    QSet<qint64> m_extraQuoteIds;
    QSet<qint64> m_candleRepairInFlight;
    QHash<qint64, QDateTime> m_candleRepairAt;
    QHash<QString, qint64> m_instrumentByPosition;  // open positionId -> its instrumentId,
                                                    // so any trade can be closed regardless
                                                    // of the instrument currently shown

    // Limit orders this session placed and that have not resolved yet, keyed by the
    // broker's orderId. eToro publishes no "my open orders" endpoint, so the app keeps
    // the registry and re-reads each entry through the per-order lookup. Orders placed
    // in an earlier session (or in eToro's own UI) are therefore not listed here —
    // they still execute at the broker, which is the whole point of resting them there.
    QHash<QString, PendingOrder> m_pendingOrders;
    // Resting orders are re-read on their OWN 4 s timer (not the price-poll cadence), so
    // the list and its statuses are current while the user watches it. It runs only while
    // at least one order rests. orders:lookup lives in a 60-requests/60-s pool shared with
    // the closed-trade endpoints, and 4 s means 15 ticks/min — so a tick refreshes at most
    // two orders and continues at m_pendingCursor next time, keeping the worst case at
    // ~30 requests/min instead of 15 × (number of orders).
    QTimer *m_pendingTimer = nullptr;
    qint32 m_pendingCursor = 0;  // round-robin position in the sorted id list

    bool m_scanActive = false;           // a leverage screener run is in progress
    bool m_pnlFetching = false;          // a closed-trade P/L paging walk is in progress
    qint32 m_pnlPendingWeeks = 0;        // lookback queued behind the running walk (0 = none)
    // Last known live spread per instrument (percent of mid), kept across
    // closed-trade refreshes: rate rows occasionally arrive without a usable
    // bid/ask (frozen weekend quotes), and the cached value keeps the cost
    // estimates stable instead of flickering to "unknown".
    QHash<qint64, double> m_spreadPctById;
    // Last mid rate per instrument from the same bulk snapshot (see lastRateFor).
    QHash<qint64, double> m_lastRateById;

    // Symbols whose last bulk-rates quote was live (market open), from the same
    // poll that feeds tradeabilityUpdated. Lets the closed-trades cost estimator
    // flag spreads captured from frozen (widened) after-hours quotes.
    QSet<QString> m_freshQuoteSymbols;

    // Previous poll's quote timestamp per instrument — the baseline the market-open
    // inference compares against. Absolute age cannot decide it: eToro's public rates
    // feed publishes minutes behind real time (measured ~6 min on the indices,
    // ~19 min on the .24-7 variants), so an open market's quote is never "age ≈ 0".
    // A timestamp that MOVED between two polls means the feed is still publishing,
    // whatever its offset; a frozen one means the session has ended.
    QHash<qint64, QDateTime> m_lastQuoteTime;

    // Candle sort direction (newest-first). The chart is seeded from two resolutions
    // merged in fetchHistoryReal: hourly for ~1 month of context, plus the most recent
    // ~1000 one-minute candles (~17h) so the recent action keeps full 1-minute detail.
    QString m_candleDirection = QStringLiteral("desc");
};

#endif // TRADINGAPP_ETOROCLIENT_H
