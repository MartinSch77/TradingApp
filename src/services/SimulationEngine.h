// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_SIMULATIONENGINE_H
#define TRADINGAPP_SERVICES_SIMULATIONENGINE_H

#include "domain/Models.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>

// The self-contained trading SIMULATION: a synthetic random-walk price feed plus
// a virtual account (cash, open positions, SL/TP/trailing stops, closed-trade
// log). EtoroClient delegates to this engine when no API credentials are
// configured, and forwards its signals unchanged — so the UI sees exactly the
// same interface in both modes. Keeping the strategy in its own class keeps the
// broker client focused on the real eToro REST API.
class SimulationEngine : public QObject
{
    Q_OBJECT
public:
    explicit SimulationEngine(QObject *parent = nullptr);

    // Reset the feed for `symbol` and build ~150 minutes of synthetic history.
    // resetAccount=true wipes cash/positions (initial start); false keeps them
    // across an instrument switch so open trades on all instruments are retained.
    // Returns the simulated instrument (id -1 marks it as synthetic).
    Instrument prepare(const QString &symbol, const QString &orderCurrency, bool resetAccount);

    // Emit the full opening snapshot for the prepared instrument: history, the
    // current price, cash, the (possibly carried-over) portfolio and the
    // instrument's leverage steps — in the order the UI expects them.
    void emitSnapshot();

    // One poll-timer tick: move the price, ratchet trailing stops, auto-close
    // positions whose SL/TP is hit, and re-publish the portfolio.
    void tick();

    [[nodiscard]] double lastPrice() const;  // out-of-line: keeps coverage records unambiguous

    // Opens at the current synthetic price (req.triggerRate is ignored here — the
    // caller routes a limit order to placePendingOrder instead).
    void openPosition(const OrderRequest &req);
    // Place a resting entry order (the simulated counterpart of eToro's limit
    // order): it sits until the synthetic price touches req.triggerRate from the side
    // the market is on now, then opens the position with these SL/TP amounts.
    void placePendingOrder(const OrderRequest &req);
    void cancelPendingOrder(const QString &orderId);
    // Adjust a resting order's trigger rate and SL/TP amounts. Mirrors the real path,
    // which can only cancel and re-place (eToro has no update-order endpoint), so the
    // order comes back under a NEW id here too — the UI then behaves the same in both
    // modes.
    void modifyPendingOrder(const QString &orderId, double triggerRate,
                            double stopLossAmount, double takeProfitAmount);
    [[nodiscard]] QList<PendingOrder> pendingOrders() const;
    void closePosition(const QString &positionId);
    void modifyPosition(const QString &positionId, double stopLossRate,
                        double takeProfitRate, bool trailingStop);
    void refreshPortfolio();   // recompute P/L and re-emit portfolioUpdated
    void summarizeMonthly();   // build MonthlyPnl from the recorded closed trades

    // Leverage-screener scan without a network: synthesise a plausible close
    // series per instrument and take leverage from the per-symbol table, so the
    // screener is fully usable before credentials exist. Emits screenerRow per
    // instrument and screenerFinished at the end (synchronously).
    void scanInstruments(const QStringList &symbols);

signals:
    void historyReady(const QList<Candle> &candles);
    void priceUpdated(const QDateTime &time, double price);
    void portfolioUpdated(const QList<Position> &positions);
    void cashUpdated(double available, const QString &currency);
    void orderResult(bool ok, const QString &message);
    // The resting entry orders after every change (placed / cancelled / triggered).
    void pendingOrdersUpdated(const QList<PendingOrder> &orders);
    // positionId travels WITH the result. The message already names it, but a UI that
    // needed the id would have to parse prose — and prose is not an interface.
    void positionClosed(bool ok, const QString &message, const QString &positionId);
    void leverageOptions(const QList<int> &values);
    void monthlyPnlReady(const MonthlyPnl &summary);
    void screenerRow(const ScreenerRow &row);
    void screenerProgress(qint32 done, qint32 total);
    void screenerFinished();
    void log(const QString &message, bool isError);

public:
    // Reseed the price-walk PRNG. Tests use this to make the walk deterministic —
    // QRandomGenerator::global() is securely seeded and cannot repeat a sequence.
    void seedRng(quint32 seed);

private:
    void recomputePortfolio();      // refresh current-instrument positions' P/L
    // Open every resting order whose trigger the current price has touched.
    void releaseTriggeredOrders();
    double gaussian();              // standard-normal sample for the price walk

    QRandomGenerator m_rng = QRandomGenerator::securelySeeded();

    QString m_symbol;               // instrument the synthetic feed currently tracks
    QString m_orderCurrency;        // currency reported with cash updates
    Instrument m_instrument;        // the simulated instrument (id -1)
    QList<Candle> m_pendingHistory; // built by prepare(), published by emitSnapshot()

    double m_simPrice = 0.0;
    double m_simCash = 100000.0;    // free funds available to open positions
    qint32 m_simSeq = 0;
    QList<Position> m_simPositions;
    // Resting entry orders, released by the same market-if-touched rule the broker
    // documents: the trigger rate "or better" — lower for a buy, higher for a short.
    QList<PendingOrder> m_simPending;
    qint32 m_simOrderSeq = 0;       // ids for the resting orders (separate from positions)
    // Log of trades closed this session, so the monthly-P/L summary also works in
    // simulation (the real path reads closed trades from the eToro history API).
    struct SimClosedTrade {
        QString symbol;
        double netProfit = 0.0;
        QDateTime closeTime;
    };
    QList<SimClosedTrade> m_simClosed;
};

#endif // TRADINGAPP_SERVICES_SIMULATIONENGINE_H
