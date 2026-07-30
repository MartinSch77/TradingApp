#ifndef TRADINGAPP_DOMAIN_MODELS_H
#define TRADINGAPP_DOMAIN_MODELS_H

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

#include <cmath>

// Plain value types shared by the domain, service and UI layers.

// A tradable instrument on eToro, e.g. SPX500.
struct Instrument {
    qint64 instrumentId = 0;   // eToro numeric instrument id
    QString symbol;            // internalSymbolFull, e.g. "SPX500"
    QString displayName;       // human name, e.g. "S&P 500"
    double currentRate = 0.0;  // last known price, if the search returned one
    // eToro's per-order unit cap from the eligibility endpoint (maxUnitsPerOrder,
    // e.g. 20 units for GOLD); 0 while unknown, which disables the local check.
    double maxUnitsPerOrder = 0.0;

    [[nodiscard]] bool isValid() const { return instrumentId != 0; }
};

// Per-unit rollover fees for an instrument (USD per unit per night; the weekend
// figure is the one-off triple charge applied on the weekend rollover night).
// Negative values are credits paid to the position holder.
struct InstrumentFees {
    double buyOvernight = 0.0;
    double sellOvernight = 0.0;
    double buyWeekend = 0.0;
    double sellWeekend = 0.0;

    // Defined out-of-line (Models.cpp) so exactly one TU instruments it for
    // coverage — see the note there.
    [[nodiscard]] bool isValid() const;
};

// One OHLC candle (used to seed the chart with recent history).
struct Candle {
    QDateTime timestamp;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
};

// An open position in the portfolio.
struct Position {
    QString positionId;
    qint64 instrumentId = 0;
    QString symbol;
    bool isBuy = true;        // true = long (buy), false = short (sell)
    double amount = 0.0;      // invested cash amount
    double units = 0.0;       // number of units held
    double openRate = 0.0;    // price at which the position was opened
    double leverage = 1.0;
    double profit = 0.0;      // current (unrealised) profit/loss in account currency
    bool profitFromApi = false;   // true = profit is eToro's own live P/L (authoritative,
                                  // includes spread + fees), not a locally-derived estimate
    double apiCloseRate = 0.0;    // the rate eToro marked `profit` at (unrealizedPnL.closeRate,
                                  // bid for a long / ask for a short); anchor for the live
                                  // re-price between polls. 0 = not supplied.
    double closingCost = 0.0;     // est. spread cost to close the position now (account ccy)
    double stopLossRate = 0.0;    // auto-close rate for a loss (0 = none)
    double takeProfitRate = 0.0;  // auto-close rate for a profit (0 = none)
    bool trailingStop = false;    // stop-loss trails the price (simulation bookkeeping)
    double trailDistance = 0.0;   // price distance the trailing stop keeps behind the peak
    QDateTime openTime;
};

// Everything one OPENING order needs. Bundled into a value type because the four
// money figures are easy to transpose positionally, and because a limit order
// differs from a market order in exactly one field (triggerRate).
struct OrderRequest {
    bool isBuy = true;              // true = open long, false = open short
    // Instrument to trade; 0 = the one currently being traded. Only a LIMIT order may
    // name a different instrument: it is priced off its own trigger rate, so it needs no
    // live quote of that instrument, whereas a market order would have to be priced from
    // a bid/ask the app only polls for the instrument on screen.
    qint64 instrumentId = 0;
    double amount = 0.0;            // cash to invest, in the order currency
    double leverage = 1.0;
    double stopLossAmount = 0.0;    // loss at which the position closes (0 = none)
    double takeProfitAmount = 0.0;  // profit at which it closes (0 = none)
    bool trailingStop = false;      // stop-loss follows the price in the trade's favour
    // 0 = MARKET order: executes at the current price. > 0 = LIMIT order: the broker
    // holds it until the instrument's rate reaches this value, then executes at market.
    double triggerRate = 0.0;

    [[nodiscard]] bool isLimit() const { return triggerRate > 0.0; }
};

// A resting entry order the BROKER holds until the market reaches a rate — what
// eToro's own UI calls a "limit order" and its API a "market if touched" order
// (orderType "mit" + triggerRate): once the instrument's rate touches
// triggerRate, eToro releases it as a market order. Unlike an app-side price
// watch it survives the app being closed, and it triggers on eToro's own feed
// instead of the app's polled (minutes-delayed) quotes.
struct PendingOrder {
    QString orderId;             // broker order id (string: simulation ids are synthetic)
    qint64 instrumentId = 0;
    QString symbol;
    bool isBuy = true;           // true = long entry (buy), false = short entry (sell)
    double triggerRate = 0.0;    // rate at which the broker releases the order
    double amount = 0.0;         // cash to invest, in the order currency
    double leverage = 1.0;
    double stopLossAmount = 0.0;    // loss at which the opened position closes (0 = none)
    double takeProfitAmount = 0.0;  // profit at which it closes (0 = none)
    bool trailingStop = false;
    QString status;              // broker status wording ("Waiting for market", …)
    QDateTime submitted;

    // Field-wise equality, so a poll that brings back an unchanged order can skip the
    // refresh (and with it the table rebuild) instead of redrawing every few seconds.
    [[nodiscard]] bool operator==(const PendingOrder &other) const = default;
};

// One closed trade from the account history — the API's own figures (netProfit
// is net of spread and fees; `fees` is the rollover total) plus the app's
// spread-cost estimates. The API doesn't report the historical spread, so the
// open/close costs are estimated from the instrument's CURRENT spread as
// half-spread × notional, and flagged as estimates in the UI.
struct ClosedTrade {
    qint64 instrumentId = 0;
    QString symbol;            // listed symbol, or "#<id>" for unlisted instruments
    bool listed = false;       // instrument is in the app's selector
    bool isBuy = true;
    double leverage = 1.0;
    double investment = 0.0;   // cash invested (account currency)
    double units = 0.0;
    double openRate = 0.0;
    double closeRate = 0.0;
    QDateTime openTime;
    QDateTime closeTime;
    double netProfit = 0.0;    // account currency, net of spread + fees
    double fees = 0.0;         // rollover/overnight fees the API reports
    double openCostEst = 0.0;  // est. half-spread paid on opening (account ccy)
    double closeCostEst = 0.0; // est. half-spread paid on closing
    bool costEstValid = false; // false = no live spread available to estimate from
    double spreadPctUsed = 0.0; // spread (% of mid) the estimate priced with
    bool spreadStale = false;   // spread captured while the market was closed —
                                // frozen quotes overstate the tradable spread
};

// One instrument's closed-trade P/L over a period (a row of the monthly summary).
struct InstrumentPnl {
    QString symbol;
    qint32 trades = 0;
    double netProfit = 0.0;  // summed net P/L of closed trades, in account currency
    double fees = 0.0;       // summed rollover fees the API reports
    // Summed estimated open+close spread costs (half-spread × notional per side,
    // priced at the instrument's current spread — see ClosedTrade).
    double estSpreadCosts = 0.0;
};

// Aggregated closed-trade P/L over a period, restricted to the app's listed
// (selectable) instruments, with whole-account totals kept alongside for context.
struct MonthlyPnl {
    QDate fromDate;
    QDate toDate;
    QString currency;                    // account currency the sums are in (e.g. "USD")
    qint32 trades = 0;                   // closed trades on listed instruments
    double netProfit = 0.0;              // their summed net P/L
    double fees = 0.0;
    qint32 accountTrades = 0;            // all closed trades in the window (context)
    double accountNet = 0.0;             // all-instruments net P/L (context)
    QList<InstrumentPnl> perInstrument;  // listed instruments only, sorted by net desc
};

// One instrument's row in the leverage screener: its max allowed leverage plus a
// recent close series, from which the UI computes the same buy/sell ensemble it
// shows for the selected instrument. ok=false when the data couldn't be fetched.
struct ScreenerRow {
    QString symbol;
    qint32 maxLeverage = 0;   // highest CFD leverage multiplier the account may use
    QList<double> closes;     // recent closes (oldest to newest), for the signal
    double lastPrice = 0.0;   // most recent close / live rate
    bool ok = false;          // false = leverage and/or candle data unavailable
};

// One recent news headline for an instrument, from a public news feed. Used to
// enrich the "buy / sell now" recommendations with the information behind them.
struct NewsHeadline {
    QString title;
    QString provider;    // e.g. "Reuters"
    QDateTime published;
};

// TradingView aggregated technical rating for an instrument across several
// timeframes, each in [-1, 1] (Strong Sell ... Strong Buy); NaN = not available.
struct WebRating {
    double m15 = std::nan("");   // 15-minute
    double h1 = std::nan("");    // 1-hour
    double d1 = std::nan("");    // 1-day
    [[nodiscard]] bool valid() const
    {
        return !std::isnan(m15) || !std::isnan(h1) || !std::isnan(d1);
    }
    // Mean of the available timeframes (the multi-timeframe consensus).
    [[nodiscard]] double consensus() const
    {
        double sum = 0.0;
        qint32 n = 0;
        for (const double v : {m15, h1, d1}) {
            if (!std::isnan(v)) {
                sum += v;
                ++n;
            }
        }
        return (n > 0) ? (sum / static_cast<double>(n)) : std::nan("");
    }
};

// The Claude (AI) synthesis over all the decision-window sources: which
// instrument to trade, how, and why. ok=false when no key / the call failed.
struct AiDecision {
    bool ok = false;
    QString symbol;
    QString action;        // "BUY" | "SELL" | "HOLD"
    double confidence = 0.0;   // 0..100
    qint32 leverage = 0;
    QString rationale;
    QString error;         // populated when ok=false
};

// One scheduled macro-economic event from a public calendar feed.
struct EconomicEvent {
    QDateTime when;    // event time
    QString title;     // e.g. "CPI m/m", "Federal Funds Rate", "Unemployment Rate"
    QString country;   // currency/country code, e.g. "USD"
    QString impact;    // "Low" | "Medium" | "High"
    QString forecast;
    QString previous;
};

Q_DECLARE_METATYPE(Instrument)
Q_DECLARE_METATYPE(ClosedTrade)
Q_DECLARE_METATYPE(Candle)
Q_DECLARE_METATYPE(ScreenerRow)
Q_DECLARE_METATYPE(Position)
Q_DECLARE_METATYPE(OrderRequest)
Q_DECLARE_METATYPE(PendingOrder)
Q_DECLARE_METATYPE(InstrumentPnl)
Q_DECLARE_METATYPE(MonthlyPnl)
Q_DECLARE_METATYPE(NewsHeadline)
Q_DECLARE_METATYPE(WebRating)
Q_DECLARE_METATYPE(AiDecision)

#endif // TRADINGAPP_DOMAIN_MODELS_H
