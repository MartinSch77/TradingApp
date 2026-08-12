// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/SwingPullbackStrategy.h"

#include "domain/Indicators.h"

#include <algorithm>

namespace trading {

namespace {
StrategyDecision refuse(const QString &code, const QString &why)
{
    StrategyDecision out;
    out.enter = false;
    out.code = code;
    out.why = why;
    return out;
}

// Where a controlled pullback fell from, how long it ran, and its own low (by
// close, for the EMA-proximity test, and by bar low, for the stop). Split out
// of evaluate() purely to keep its own McCabe complexity within budget.
struct PullbackWindow {
    bool ok = false;
    qsizetype peakIdx = 0;
    qsizetype sessions = 0;
    double lowClose = 0.0;
    double barLow = 0.0;
    QString code;   // set when !ok
    QString why;
};

// Walks back from yesterday over a clean run of non-increasing closes; where it
// stops is the peak the price fell from.
PullbackWindow findPullback(const QList<double> &closes, const QList<double> &lows,
                            qsizetype yesterday, const SwingPullbackConfig &cfg)
{
    qsizetype i = yesterday;
    qsizetype sessions = 0;
    while ((i > 0) && (sessions <= cfg.maxPullbackSessions) && (closes[i] <= closes[i - 1])) {
        ++sessions;
        --i;
    }
    PullbackWindow out;
    out.peakIdx = i;
    out.sessions = sessions;
    if (sessions < cfg.minPullbackSessions) {
        out.code = QStringLiteral("no-pullback");
        out.why = QStringLiteral("only %1 session(s) of pullback, need at least %2")
                       .arg(sessions)
                       .arg(cfg.minPullbackSessions);
        return out;
    }
    if (sessions > cfg.maxPullbackSessions) {
        out.code = QStringLiteral("pullback-too-long");
        out.why = QStringLiteral("pullback has run %1 sessions, over the %2 limit")
                       .arg(sessions)
                       .arg(cfg.maxPullbackSessions);
        return out;
    }
    out.lowClose = closes[i + 1];
    out.barLow = lows[i + 1];
    for (qsizetype j = i + 1; j <= yesterday; ++j) {
        out.lowClose = std::min(out.lowClose, closes[j]);
        out.barLow = std::min(out.barLow, lows[j]);
    }
    out.ok = true;
    return out;
}

// Close/EMA20/EMA50/EMA200 stacked in an uptrend — split into its own function
// purely to keep evaluate()'s McCabe complexity within budget.
bool uptrendEstablished(double close, double fast, double mid, double slow)
{
    return (close > slow) && (fast > mid) && (mid > slow);
}

// What confirmAndSize needs, bundled so the function itself stays under the
// parameter-count budget.
struct ConfirmInputs {
    double todayClose = 0.0;
    double todayLow = 0.0;
    double yesterdayLow = 0.0;
    double yesterdayHigh = 0.0;
    double pullbackBarLow = 0.0;
    double atr = 0.0;
};

// The confirmation + stop-sizing tail: today has to CONFIRM the reversal (a
// higher low, or a close back above yesterday's high), and the stop — below
// the pullback's own bar low, with an ATR buffer — has to land below today's
// close. Split out of evaluate() purely to keep its own McCabe complexity
// within budget.
StrategyDecision confirmAndSize(const ConfirmInputs &in, const SwingPullbackConfig &cfg,
                                qsizetype pullbackSessions)
{
    const bool higherLow = in.todayLow > in.yesterdayLow;
    const bool closeAbovePriorHigh = in.todayClose > in.yesterdayHigh;
    if (!higherLow && !closeAbovePriorHigh) {
        return refuse(QStringLiteral("no-confirmation"),
                      QStringLiteral("neither a higher low nor a close above yesterday's high"));
    }
    const double stopPrice = in.pullbackBarLow - (cfg.stopAtrBuffer * in.atr);
    if ((stopPrice <= 0.0) || (in.todayClose <= 0.0) || (stopPrice >= in.todayClose)) {
        return refuse(QStringLiteral("invalid-stop"),
                      QStringLiteral("computed stop %1 is not below today's close %2")
                          .arg(stopPrice)
                          .arg(in.todayClose));
    }
    StrategyDecision out;
    out.enter = true;
    out.isBuy = true;   // long-only V1: the design describes an uptrend pullback, not a short
    out.stopFraction = (in.todayClose - stopPrice) / in.todayClose;
    out.why = QStringLiteral("uptrend intact, %1-session pullback to EMA20, %2 confirmed")
                  .arg(pullbackSessions)
                  .arg(higherLow ? QStringLiteral("higher low")
                                 : QStringLiteral("close above prior high"));
    return out;
}
} // namespace

SwingPullbackStrategyV1::SwingPullbackStrategyV1(SwingPullbackConfig config)
    : m_config(config)
{
}

QString SwingPullbackStrategyV1::version() const
{
    return QStringLiteral("swing-pullback-v1");
}

StrategyDecision SwingPullbackStrategyV1::evaluate(const StrategySnapshot &snapshot) const
{
    const QList<DailyBar> &bars = snapshot.bars;
    // Enough sessions for the slowest EMA, plus room to look for a peak and a
    // pullback before today, plus today itself and the ATR window.
    const qsizetype minBars = m_config.slowEmaPeriod + m_config.maxPullbackSessions
                             + m_config.atrPeriod + 2;
    if (bars.size() < minBars) {
        return refuse(QStringLiteral("insufficient-history"),
                      QStringLiteral("%1 daily bars is short of the %2 this strategy needs")
                          .arg(bars.size())
                          .arg(minBars));
    }

    QList<double> closes;
    QList<double> highs;
    QList<double> lows;
    closes.reserve(bars.size());
    highs.reserve(bars.size());
    lows.reserve(bars.size());
    for (const DailyBar &b : bars) {
        closes.append(b.close);
        highs.append(b.high);
        lows.append(b.low);
    }

    const qsizetype n = closes.size();
    const qsizetype today = n - 1;
    const qsizetype yesterday = n - 2;

    // --- 1. The trend itself: an established uptrend, not a guess about one. ---
    const QList<double> emaFast = emaSeries(closes, m_config.fastEmaPeriod);
    const QList<double> emaMid = emaSeries(closes, m_config.midEmaPeriod);
    const QList<double> emaSlow = emaSeries(closes, m_config.slowEmaPeriod);
    const double todayClose = closes[today];
    const double todayFast = emaFast[today];
    const double todayMid = emaMid[today];
    const double todaySlow = emaSlow[today];
    if (!uptrendEstablished(todayClose, todayFast, todayMid, todaySlow)) {
        return refuse(QStringLiteral("no-uptrend"),
                      QStringLiteral("close/EMA20/EMA50/EMA200 are not stacked in an uptrend"));
    }

    // --- 2. Regime: sit out a strongly inverted VIX term structure. ---
    if (snapshot.termStructure.known
        && (snapshot.termStructure.nearFarRatio > m_config.maxTermStructureRatio)) {
        return refuse(QStringLiteral("vix-inverted"),
                      QStringLiteral("VIX term structure inverted at %1")
                          .arg(snapshot.termStructure.nearFarRatio, 0, 'f', 3));
    }

    // --- 3. No imminent high-impact print to trade into. ---
    if (snapshot.eventRiskImminent) {
        return refuse(QStringLiteral("event-risk"),
                      QStringLiteral("a high-impact release is imminent"));
    }

    // --- 4. The pullback: findPullback walks back over a clean run of
    // non-increasing closes; where it stops is the peak it fell from. ---
    const PullbackWindow pullback = findPullback(closes, lows, yesterday, m_config);
    if (!pullback.ok) {
        return refuse(pullback.code, pullback.why);
    }

    // --- 5. The pullback must have come within reach of the fast EMA. Today's
    // ATR/EMA20 stand in for their value during the pullback window — both move
    // slowly over 2-5 sessions, so this is a reasonable proxy, not the exact
    // contemporaneous reading. The prior-breakout-level and anchored-VWAP
    // alternatives the design also allows are NOT implemented (see the class
    // comment) — a pullback that only qualifies by one of those is refused here.
    const double atr = averageTrueRange(highs, lows, closes, m_config.atrPeriod);
    if ((atr <= 0.0)
        || (std::abs(pullback.lowClose - todayFast) > (m_config.emaProximityAtr * atr))) {
        return refuse(QStringLiteral("pullback-too-shallow-or-deep"),
                      QStringLiteral("pullback low %1 did not come within %2 ATR of EMA20 (%3)")
                          .arg(pullback.lowClose)
                          .arg(m_config.emaProximityAtr, 0, 'f', 2)
                          .arg(todayFast));
    }

    // --- 6. Today has to CONFIRM the reversal (a higher low, or a close back
    // above yesterday's high), and the stop has to land below today's close. ---
    ConfirmInputs confirmIn;
    confirmIn.todayClose = todayClose;
    confirmIn.todayLow = lows[today];
    confirmIn.yesterdayLow = lows[yesterday];
    confirmIn.yesterdayHigh = highs[yesterday];
    confirmIn.pullbackBarLow = pullback.barLow;
    confirmIn.atr = atr;
    return confirmAndSize(confirmIn, m_config, pullback.sessions);
}

} // namespace trading
