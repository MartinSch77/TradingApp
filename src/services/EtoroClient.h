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

    const Instrument &instrument() const & { return m_instrument; }
    // Latest bid/ask (0 until a real quote arrives, e.g. in simulation mode): a buy
    // opens near the ask, a sell near the bid, so their gap is the spread the trade
    // crosses on opening. Used by the UI to estimate the opening cost.
    double lastBid() const { return m_lastBid; }
    double lastAsk() const { return m_lastAsk; }
    const Config &config() const & { return m_config; }

    // Live spread (percent of mid) for any listed symbol, from the most recent
    // bulk rates snapshot (the periodic tradeability refresh keeps it warm for
    // every resolved instrument). 0 while unknown.
    double spreadPctFor(const QString &symbol) const;
    // Cached per-unit rollover fees for any listed symbol (invalid while unknown).
    InstrumentFees feesFor(const QString &symbol) const;
    // Fetch the rollover fees for a listed symbol if they aren't cached yet
    // (public etorostatic feed — cheap, outside the rate-limited API). The result
    // arrives via instrumentFeesUpdated; a no-op in simulation mode.
    void requestFees(const QString &symbol);

public slots:
    // amount is the cash to invest (order currency); isBuy=false opens a short.
    // stopLossAmount / takeProfitAmount are the loss/profit in account currency at
    // which the position should auto-close (0 = none). trailingStop makes the
    // stop-loss trail the price in the trade's favour (never against it).
    void openPosition(bool isBuy, double amount, double leverage, double stopLossAmount,
                      double takeProfitAmount, bool trailingStop = false);
    void closePosition(const QString &positionId);
    // Change the stop-loss / take-profit *rates* on an open position (0 clears that
    // leg). trailingStop only matters when a stop-loss rate is set.
    void modifyPosition(const QString &positionId, double stopLossRate,
                        double takeProfitRate, bool trailingStop);
    void refreshPortfolio();
    // Summarise closed-trade net P/L over the last 7 weeks, restricted to the app's
    // listed (selectable) instruments; result arrives via monthlyPnlReady.
    void fetchMonthlyPnl();
    // Walk the closed-trade history `weeksBack` weeks back (clamped to 1..26).
    // Emits BOTH the aggregated monthlyPnlReady summary for the window and
    // closedTradesReady with the individual trades, each carrying open/close
    // spread-cost estimates priced from the instruments' current spreads.
    void fetchClosedTrades(qint32 weeksBack);

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
    void portfolioUpdated(const QList<Position> &positions);
    void cashUpdated(double available, const QString &currency);
    // EUR per 1 USD (from the EURUSD instrument), so the USD-based account figures
    // can be displayed in euro. Emitted whenever a fresh rate is fetched.
    void fxRateUpdated(double eurPerUsd);
    void orderResult(bool ok, const QString &message);
    void positionClosed(bool ok, const QString &message);
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
    QNetworkRequest makeRequest(const QUrl &url) const;
    QNetworkReply *apiGet(const QString &path, const QUrlQuery &query);
    QNetworkReply *apiPost(const QString &path, const QJsonObject &body);
    QNetworkReply *apiPatch(const QString &path, const QJsonObject &body);
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
    // symbols whose market is currently open, inferred from quote freshness (a stale
    // `date` means the price has frozen, i.e. the market is closed). Emitted via
    // tradeabilityUpdated. eligibility.allowOpenPosition is unusable here — it is a
    // static account permission that stays true even when the exchange is closed.
    void refreshTradeabilityReal();
    void fetchHistoryReal();
    // Fetch `count` candles at `interval` (ascending), then hand them to cb (empty on
    // failure). fetchHistoryReal merges a coarse month with a fine recent window.
    void fetchCandles(const QString &interval, qint32 count,
                      std::function<void(QList<Candle>)> cb);
    void pollPriceReal();
    // Fetch the EURUSD rate (instrument 1) and emit fxRateUpdated. Real mode only.
    void fetchEurUsd();
    void refreshPortfolioReal();
    // Fill in each open position's P/L from live rates for all held instruments,
    // then emit portfolioUpdated (the payload has no per-position P/L).
    void finalizePortfolioPl(QList<Position> positions);
    void refreshBalanceReal();
    // Fetch one trade-history page and fold it into acc, then recurse to the next
    // page until the API returns an empty page (history is newest-first).
    void fetchTradeHistoryPageReal(const QSharedPointer<PnlAccum> &acc);
    // Walk complete: price the trades' open/close cost estimates from one bulk
    // rates call (current spreads), then emit the summary and the trade list.
    void finishTradeHistory(const QSharedPointer<PnlAccum> &acc);
    void emitMonthlyPnl(const QSharedPointer<PnlAccum> &acc);  // build MonthlyPnl + emit
    // ---- leverage screener -----------------------------------------------
    void scanInstrumentsReal();
    // Fetch the next queued instrument's candles, emit its screenerRow, then recurse
    // to the following one — sequential so the market-data rate budget isn't burst.
    void fetchScanCandle(const QSharedPointer<ScanState> &st);

    void openPositionReal(bool isBuy, double amount, double leverage, double stopLossAmount,
                          double takeProfitAmount, bool trailingStop);
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
    QString accountSegment() const;  // "" for real, "/demo" for demo

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
    QHash<QString, qint64> m_instrumentByPosition;  // open positionId -> its instrumentId,
                                                    // so any trade can be closed regardless
                                                    // of the instrument currently shown

    bool m_scanActive = false;           // a leverage screener run is in progress
    bool m_pnlFetching = false;          // a closed-trade P/L paging walk is in progress
    // Last known live spread per instrument (percent of mid), kept across
    // closed-trade refreshes: rate rows occasionally arrive without a usable
    // bid/ask (frozen weekend quotes), and the cached value keeps the cost
    // estimates stable instead of flickering to "unknown".
    QHash<qint64, double> m_spreadPctById;

    // Candle sort direction (newest-first). The chart is seeded from two resolutions
    // merged in fetchHistoryReal: hourly for ~1 month of context, plus the most recent
    // ~1000 one-minute candles (~17h) so the recent action keeps full 1-minute detail.
    QString m_candleDirection = QStringLiteral("desc");
};

#endif // TRADINGAPP_ETOROCLIENT_H
