#include "services/SimulationEngine.h"

#include <QHash>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

constexpr double kPi = std::numbers::pi;

// eToro leverage steps per instrument, used only for SIMULATION — real mode
// fetches the exact values from the eToro eligibility API. Values are the steps
// eToro offers for each asset (they are not just a 1..max range: e.g. the 24/7
// index CFDs skip x10). Unknown symbols fall back to the typical index set.
QList<int> simLeverageValues(const QString &symbol)
{
    static const QHash<QString, QList<int>> table = {
        // Forex
        {QStringLiteral("EURUSD"), {1, 2, 5, 10, 20, 30}},
        // Indices
        {QStringLiteral("SPX500"), {1, 2, 5, 10, 20}},
        {QStringLiteral("SP.24-7"), {1, 2, 5, 20}},
        {QStringLiteral("NSDQ100"), {1, 2, 5, 10, 20}},
        {QStringLiteral("NSDQ100.24-7"), {1, 2, 5, 20}},
        {QStringLiteral("DJ30"), {1, 2, 5, 10, 20}},
        {QStringLiteral("GER40"), {1, 2, 5, 10, 20}},
        {QStringLiteral("EUSTX50"), {1, 2, 5, 10, 20}},
        {QStringLiteral("HKG50"), {1, 2, 5, 10, 20}},
        {QStringLiteral("CHINA50"), {1, 2, 5, 10}},
        {QStringLiteral("RTY"), {1, 2, 5, 10, 20}},
        {QStringLiteral("USDOLLAR"), {1, 2, 5, 10, 20}},
        {QStringLiteral("Switzerland20"), {1, 2, 5, 10, 20}},
        {QStringLiteral("Sweden30"), {1, 2, 5, 10}},
        {QStringLiteral("Canada60"), {1, 2, 5, 10}},
        {QStringLiteral("Colombia"), {1, 2, 5}},
        // Thematic / proprietary indices (lower caps)
        {QStringLiteral("Semiconductors"), {1, 2}},
        {QStringLiteral("AI.Leaders"), {1, 2}},
        {QStringLiteral("Cybersecurity"), {1, 2}},
        {QStringLiteral("Quantum"), {1, 2}},
        {QStringLiteral("GoldMiners"), {1, 2}},
        {QStringLiteral("Crypto10"), {1, 2}},
        {QStringLiteral("Nuclear"), {1, 2}},
        // Commodities
        {QStringLiteral("Gold.24-7"), {1, 2, 5, 20}},
        {QStringLiteral("OIL.24-7"), {1, 2, 5, 10}},
        {QStringLiteral("RUBBER"), {1, 2, 5, 10}},
    };
    const auto it = table.constFind(symbol);
    return (it != table.constEnd()) ? it.value() : QList<int>{1, 2, 5, 10, 20};
}

// Rough, plausible starting price per instrument for the SIMULATION feed, so a
// switched instrument doesn't start at an SPX500-like level. Real mode ignores
// this — it uses the live rate. Unknown symbols fall back to a generic level.
double simBasePrice(const QString &symbol)
{
    static const QHash<QString, double> bases = {
        {QStringLiteral("SPX500"), 5800.0},        {QStringLiteral("SP.24-7"), 5800.0},
        {QStringLiteral("USDOLLAR"), 104.0},
        {QStringLiteral("NSDQ100"), 20500.0},      {QStringLiteral("NSDQ100.24-7"), 20500.0},
        {QStringLiteral("DJ30"), 42000.0},         {QStringLiteral("GER40"), 18500.0},
        {QStringLiteral("HKG50"), 18000.0},        {QStringLiteral("CHINA50"), 12500.0},
        {QStringLiteral("EUSTX50"), 4900.0},       {QStringLiteral("RTY"), 2200.0},
        {QStringLiteral("Switzerland20"), 12000.0},{QStringLiteral("Canada60"), 1300.0},
        {QStringLiteral("Sweden30"), 2500.0},      {QStringLiteral("Semiconductors"), 250.0},
        {QStringLiteral("AI.Leaders"), 150.0},     {QStringLiteral("Cybersecurity"), 60.0},
        {QStringLiteral("Quantum"), 40.0},         {QStringLiteral("GoldMiners"), 35.0},
        {QStringLiteral("Crypto10"), 2500.0},      {QStringLiteral("Nuclear"), 45.0},
        {QStringLiteral("Colombia"), 30.0},        {QStringLiteral("EURUSD"), 1.08},
        {QStringLiteral("RUBBER"), 170.0},         {QStringLiteral("Gold.24-7"), 2400.0},
        {QStringLiteral("OIL.24-7"), 78.0},
    };
    const auto it = bases.constFind(symbol);
    return (it != bases.constEnd()) ? it.value() : 5800.0;
}

} // namespace

SimulationEngine::SimulationEngine(QObject *parent)
    : QObject(parent)
{
}

double SimulationEngine::lastPrice() const
{
    return m_simPrice;
}

double SimulationEngine::gaussian()
{
    double u1 = QRandomGenerator::global()->generateDouble();
    const double u2 = QRandomGenerator::global()->generateDouble();
    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    const double magnitude = std::sqrt(-2.0 * std::log(u1));
    const double angle = std::cos(2.0 * kPi * u2);
    return magnitude * angle;
}

Instrument SimulationEngine::prepare(const QString &symbol, const QString &orderCurrency,
                                     bool resetAccount)
{
    m_symbol = symbol;
    m_orderCurrency = orderCurrency;
    m_simPrice = simBasePrice(symbol);
    if (resetAccount) {
        m_simCash = 100000.0;    // start with a $100k virtual balance, like an eToro demo
        m_simPositions.clear();
        m_simSeq = 0;
    }
    // else: keep cash + open positions so trades on other instruments persist.

    m_instrument = Instrument{};
    m_instrument.instrumentId = -1;  // sentinel: simulated
    m_instrument.symbol = symbol;
    m_instrument.displayName = QStringLiteral("%1 (simulated)").arg(symbol);
    m_instrument.currentRate = m_simPrice;

    // Build ~150 minutes of history ending "now" by walking backwards.
    QList<double> prices;
    double p = m_simPrice;
    for (qint32 i = 0; i < 150; ++i) {
        prices.prepend(p);
        p = p / (1.0 + (gaussian() * 0.0009));
    }
    const QDateTime now = QDateTime::currentDateTime();
    m_pendingHistory.clear();
    m_pendingHistory.reserve(prices.size());
    for (qsizetype i = 0; i < prices.size(); ++i) {
        Candle c;
        c.timestamp = now.addSecs(-60LL * (prices.size() - 1 - i));
        c.open = (i > 0) ? prices[i - 1] : prices[i];
        c.close = prices[i];
        c.high = std::max(c.open, c.close) * (1.0 + (std::abs(gaussian()) * 0.0003));
        c.low = std::min(c.open, c.close) * (1.0 - (std::abs(gaussian()) * 0.0003));
        m_pendingHistory << c;
    }
    m_simPrice = prices.last();
    return m_instrument;
}

void SimulationEngine::emitSnapshot()
{
    emit historyReady(m_pendingHistory);
    emit priceUpdated(QDateTime::currentDateTime(), m_simPrice);
    emit cashUpdated(m_simCash, m_orderCurrency);
    emit portfolioUpdated(m_simPositions);  // clears the table on an instrument switch
    emit leverageOptions(simLeverageValues(m_symbol));
}

void SimulationEngine::tick()
{
    m_simPrice *= (1.0 + (gaussian() * 0.0008));
    if (m_simPrice < 1.0) {
        m_simPrice = 1.0;
    }
    emit priceUpdated(QDateTime::currentDateTime(), m_simPrice);
    recomputePortfolio();

    // Ratchet trailing stops: follow the price in the trade's favour by the trailing
    // distance, but never let the stop move against the position. m_simPrice is the
    // current instrument's price, so only touch positions on that instrument.
    for (Position &p : m_simPositions) {
        if (p.symbol.compare(m_symbol, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (!p.trailingStop || (p.trailDistance <= 0.0)) {
            continue;
        }
        if (p.isBuy) {
            const double s = m_simPrice - p.trailDistance;
            if (s > p.stopLossRate) {
                p.stopLossRate = s;
            }
        } else {
            const double s = m_simPrice + p.trailDistance;
            if ((p.stopLossRate <= 0.0) || (s < p.stopLossRate)) {
                p.stopLossRate = s;
            }
        }
    }

    // Auto-close current-instrument positions whose stop-loss or take-profit is hit
    // (other instruments have no live sim price this tick, so leave them alone).
    for (qsizetype i = m_simPositions.size() - 1; i >= 0; --i) {
        const Position &p = m_simPositions[i];
        if (p.symbol.compare(m_symbol, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const bool slHit = (p.stopLossRate > 0.0)
                           && (p.isBuy ? (m_simPrice <= p.stopLossRate)
                                       : (m_simPrice >= p.stopLossRate));
        const bool tpHit = (p.takeProfitRate > 0.0)
                           && (p.isBuy ? (m_simPrice >= p.takeProfitRate)
                                       : (m_simPrice <= p.takeProfitRate));
        if (slHit || tpHit) {
            const QString id = p.positionId;
            closePosition(id);  // realises P/L, returns margin, emits updates
        }
    }
    emit portfolioUpdated(m_simPositions);
}

void SimulationEngine::recomputePortfolio()
{
    // Only the current instrument has a live sim price; positions on other
    // instruments keep their last computed (frozen) P/L.
    for (Position &p : m_simPositions) {
        if (p.symbol.compare(m_symbol, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const double direction = p.isBuy ? 1.0 : -1.0;
        p.profit = direction * p.units * (m_simPrice - p.openRate);
    }
}

void SimulationEngine::refreshPortfolio()
{
    recomputePortfolio();
    emit portfolioUpdated(m_simPositions);
}

void SimulationEngine::openPosition(bool isBuy, double amount, double leverage,
                                    double stopLossAmount, double takeProfitAmount,
                                    bool trailingStop)
{
    if (amount > m_simCash) {
        emit orderResult(false, QStringLiteral("[SIM] Insufficient funds: $%1 requested, "
                                               "only $%2 available.")
                                    .arg(amount, 0, 'f', 2)
                                    .arg(m_simCash, 0, 'f', 2));
        return;
    }

    Position pos;
    pos.positionId = QString::number(++m_simSeq);
    pos.instrumentId = m_instrument.instrumentId;
    pos.symbol = m_symbol;
    pos.isBuy = isBuy;
    pos.amount = amount;
    pos.leverage = leverage;
    pos.openRate = m_simPrice;
    pos.units = (m_simPrice > 0.0) ? ((amount * leverage) / m_simPrice) : 0.0;
    pos.profit = 0.0;
    pos.openTime = QDateTime::currentDateTime();
    // Record SL/TP as rates (same amount->rate conversion as the real path).
    if (pos.units > 0.0) {
        if (stopLossAmount > 0.0) {
            const double slDist = stopLossAmount / pos.units;
            pos.stopLossRate = isBuy ? (pos.openRate - slDist) : (pos.openRate + slDist);
            // A trailing stop keeps this same price distance behind the best price.
            pos.trailingStop = trailingStop;
            pos.trailDistance = slDist;
        }
        if (takeProfitAmount > 0.0) {
            const double tpDist = takeProfitAmount / pos.units;
            pos.takeProfitRate = isBuy ? (pos.openRate + tpDist) : (pos.openRate - tpDist);
        }
    }
    m_simPositions << pos;
    m_simCash -= amount;  // margin set aside for this position

    emit orderResult(true, QStringLiteral("[SIM] %1 $%2 %3 @ %4 (x%5), SL $%6%9 / TP $%7, position #%8")
                               .arg(isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                               .arg(amount, 0, 'f', 2)
                               .arg(m_symbol)
                               .arg(pos.openRate, 0, 'f', 2)
                               .arg(leverage)
                               .arg(stopLossAmount, 0, 'f', 0)
                               .arg(takeProfitAmount, 0, 'f', 0)
                               .arg(pos.positionId)
                               .arg(pos.trailingStop ? QStringLiteral(" trailing") : QString()));
    emit portfolioUpdated(m_simPositions);
    emit cashUpdated(m_simCash, m_orderCurrency);
}

void SimulationEngine::closePosition(const QString &positionId)
{
    for (qsizetype i = 0; i < m_simPositions.size(); ++i) {
        if (m_simPositions[i].positionId == positionId) {
            const Position p = m_simPositions.takeAt(i);
            // Realise the last computed unrealised P/L. Using p.profit (rather than
            // recomputing from m_simPrice) keeps it correct when closing a trade on
            // an instrument other than the one currently shown.
            const double realized = p.profit;
            m_simCash += p.amount + realized;  // return margin plus realised P/L
            m_simClosed.append({p.symbol, realized, QDateTime::currentDateTime()});
            emit positionClosed(true, QStringLiteral("[SIM] Closed %1 position #%2, "
                                                     "realised P/L $%3")
                                          .arg(p.symbol, positionId)
                                          .arg(realized, 0, 'f', 2));
            emit portfolioUpdated(m_simPositions);
            emit cashUpdated(m_simCash, m_orderCurrency);
            return;
        }
    }
    emit positionClosed(false, QStringLiteral("[SIM] Position #%1 not found.").arg(positionId));
}

void SimulationEngine::modifyPosition(const QString &positionId, double stopLossRate,
                                      double takeProfitRate, bool trailingStop)
{
    for (Position &p : m_simPositions) {
        if (p.positionId != positionId) {
            continue;
        }
        p.stopLossRate = stopLossRate;
        p.takeProfitRate = takeProfitRate;
        p.trailingStop = trailingStop && (stopLossRate > 0.0);
        // Trailing keeps the same price gap the (new) stop sits behind the open rate.
        p.trailDistance = p.trailingStop ? std::abs(p.openRate - stopLossRate) : 0.0;
        emit log(QStringLiteral("[SIM] Trade #%1: stop-loss / take-profit updated.").arg(positionId),
                 false);
        emit portfolioUpdated(m_simPositions);
        return;
    }
    emit log(QStringLiteral("[SIM] Position #%1 not found for SL/TP update.").arg(positionId),
             true);
}

void SimulationEngine::scanInstruments(const QStringList &symbols)
{
    const auto total = static_cast<qint32>(symbols.size());
    emit screenerProgress(0, total);
    qint32 done = 0;
    for (const QString &sym : symbols) {
        ScreenerRow row;
        row.symbol = sym;
        const QList<int> levs = simLeverageValues(sym);
        for (const qint32 v : levs) {
            row.maxLeverage = std::max(row.maxLeverage, v);
        }
        double p = simBasePrice(sym);
        constexpr double kVol = 0.004;  // ~0.4% per bar
        row.closes.reserve(200);
        for (qint32 i = 0; i < 200; ++i) {
            p *= (1.0 + (kVol * gaussian()));
            if (p <= 0.0) {
                p = simBasePrice(sym);
            }
            row.closes.append(p);
        }
        row.lastPrice = row.closes.last();
        row.ok = true;
        emit screenerRow(row);
        emit screenerProgress(++done, total);
    }
    emit screenerFinished();
}

void SimulationEngine::summarizeMonthly()
{
    // Summarise the trades closed this session within the last 7 weeks. Every sim
    // instrument is a listed one (the app only trades listed symbols), so all
    // recorded closes count towards both the listed and account totals.
    MonthlyPnl s;
    s.fromDate = QDate::currentDate().addDays(-49);  // last 7 weeks
    s.toDate = QDate::currentDate();
    s.currency = m_orderCurrency.toUpper();

    QHash<QString, InstrumentPnl> bySymbol;
    for (const SimClosedTrade &t : m_simClosed) {
        if (t.closeTime.date() < s.fromDate) {
            continue;
        }
        s.accountTrades++;
        s.accountNet += t.netProfit;
        InstrumentPnl &r = bySymbol[t.symbol];
        r.symbol = t.symbol;
        r.trades++;
        r.netProfit += t.netProfit;
    }
    for (auto it = bySymbol.constBegin(); it != bySymbol.constEnd(); ++it) {
        s.perInstrument.append(it.value());
        s.trades += it.value().trades;
        s.netProfit += it.value().netProfit;
    }
    const auto sortBegin = s.perInstrument.begin();
    const auto sortEnd = s.perInstrument.end();
    std::sort(sortBegin, sortEnd,
              [](const InstrumentPnl &a, const InstrumentPnl &b) {
                  return a.netProfit > b.netProfit;
              });
    emit monthlyPnlReady(s);
}
