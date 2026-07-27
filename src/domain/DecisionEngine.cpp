#include "domain/DecisionEngine.h"

#include "domain/SignalEnsemble.h"

#include <QDateTime>
#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace trading {

double newsSentimentScore(const QList<NewsHeadline> &news, qint32 &countOut)
{
    static const QStringList pos = {
        QStringLiteral("beat"), QStringLiteral("surge"), QStringLiteral("rally"),
        QStringLiteral("gains"), QStringLiteral("jump"), QStringLiteral("soar"),
        QStringLiteral("upgrade"), QStringLiteral("strong"), QStringLiteral("record"),
        QStringLiteral("optimism"), QStringLiteral("boost"), QStringLiteral("rebound"),
        QStringLiteral("rise"), QStringLiteral("climb"), QStringLiteral("tops"),
        QStringLiteral("outperform"), QStringLiteral("bullish"), QStringLiteral("recovery"),
        QStringLiteral("rate cut"), QStringLiteral("ease")};
    static const QStringList neg = {
        QStringLiteral("miss"), QStringLiteral("plunge"), QStringLiteral("fall"),
        QStringLiteral("drop"), QStringLiteral("slump"), QStringLiteral("selloff"),
        QStringLiteral("sell-off"), QStringLiteral("downgrade"), QStringLiteral("weak"),
        QStringLiteral("fear"), QStringLiteral("recession"), QStringLiteral("warn"),
        QStringLiteral("cuts forecast"), QStringLiteral("slide"), QStringLiteral("tumble"),
        QStringLiteral("sink"), QStringLiteral("bearish"), QStringLiteral("crisis"),
        QStringLiteral("rate hike"), QStringLiteral("layoff"), QStringLiteral("default")};
    qint32 s = 0;
    qint32 n = 0;
    for (const NewsHeadline &h : news) {
        const QString t = h.title.toLower();
        bool counted = false;
        for (const QString &w : pos) {
            if (t.contains(w)) {
                s += 1;
                counted = true;
            }
        }
        for (const QString &w : neg) {
            if (t.contains(w)) {
                s -= 1;
                counted = true;
            }
        }
        if (counted) {
            ++n;
        }
        if (n >= 6) {
            break;
        }
    }
    countOut = static_cast<qint32>(news.size());
    return (n > 0)
               ? std::clamp(static_cast<double>(s) / static_cast<double>(n), -1.0, 1.0)
               : 0.0;
}

double intradayTilt(const QList<double> &closes)
{
    constexpr qsizetype kMinPoints = 30;  // enough of the session to mean something
    if (closes.size() < kMinPoints) {
        return 0.0;
    }
    double sum = 0.0;
    for (const double c : closes) {
        sum += c;
    }
    const double mean = sum / static_cast<double>(closes.size());
    double var = 0.0;
    for (const double c : closes) {
        var += (c - mean) * (c - mean);
    }
    const double sigma = std::sqrt(var / static_cast<double>(closes.size()));
    if (sigma <= 0.0) {
        return 0.0;  // flat series — no read
    }
    // Where the latest price sits in the session's own distribution: a z-score
    // against the session mean, softened so ±2σ maps to the full ±1 tilt.
    const double z = (closes.last() - mean) / sigma;
    return std::clamp(z / 2.0, -1.0, 1.0);
}

double crowdTilt(double fearGreed)
{
    const double fg = std::clamp(fearGreed, 0.0, 100.0);
    if (fg <= 20.0) {
        // Extreme fear → contrarian bullish, growing towards the floor (max +0.8).
        return 0.4 + ((20.0 - fg) * 0.02);
    }
    if (fg >= 80.0) {
        // Extreme greed → contrarian bearish (max -0.8).
        return -0.4 - ((fg - 80.0) * 0.02);
    }
    // Ordinary readings: a mild momentum tilt with the crowd (±0.4 at the edges).
    return (fg - 50.0) / 75.0;
}

double marketRegime(const MarketSnapshot &m, bool &eventRiskOut)
{
    // Regime from VIX: calm = mildly risk-on, fearful = risk-off.
    double regime = 0.0;
    if (m.vixValid) {
        if (m.vix >= 25.0) {
            regime = (m.vix >= 35.0) ? -0.6 : -0.35;
        } else if (m.vix < 16.0) {
            regime = 0.2;
        } else {
            // ordinary VIX band — neutral regime
        }
    }
    // An imminent high-impact calendar event flags added volatility.
    eventRiskOut = false;
    const QDateTime now = QDateTime::currentDateTime();
    for (const EconomicEvent &e : m.events) {
        const qint64 secsToEvent = now.secsTo(e.when);
        if (e.when.isValid() && (secsToEvent > 0) && (secsToEvent <= (6LL * 3600))
            && (e.impact.compare(QLatin1String("High"), Qt::CaseInsensitive) == 0)) {
            eventRiskOut = true;
            break;
        }
    }
    return regime;
}

QList<DecisionRow> computeDecisionRows(const MarketSnapshot &m)
{
    bool eventRisk = false;
    const double regime = marketRegime(m, eventRisk);

    QList<DecisionRow> rows;
    for (const ScreenerRow &r : m.screenerRows) {
        if (!r.ok || r.closes.isEmpty()) {
            continue;
        }
        DecisionRow d;
        d.symbol = r.symbol;
        d.maxLev = r.maxLeverage;
        d.regime = regime;
        d.eventRisk = eventRisk;

        const Ensemble e = computeEnsemble(r.closes, m.vixValid, m.vixChangePct);
        double techSigned = 0.0;
        if (e.valid) {
            d.haveTech = true;
            d.techDir = e.signalDir;
            d.techConf = e.confidence;
            d.techLabel = e.signal;
            techSigned = e.signalDir * (e.confidence / 100.0);  // signed, [-1, 1]
        }
        if (m.ratingBySymbol.contains(r.symbol)) {
            const double c = m.ratingBySymbol.value(r.symbol).consensus();
            if (!std::isnan(c)) {
                d.haveRating = true;
                d.rating = c;
            }
        }
        const QList<NewsHeadline> news = m.newsBySymbol.value(r.symbol);
        if (!news.isEmpty()) {
            d.haveNews = true;
            d.newsScore = newsSentimentScore(news, d.newsCount);
        }

        if (m.fgValid) {
            d.haveCrowd = true;
            d.crowd = crowdTilt(m.fearGreed);
        }

        // Independent Yahoo intraday momentum, when a session series arrived.
        const QList<double> intraday = m.intradayBySymbol.value(d.symbol);
        if (intraday.size() >= 30) {
            d.haveYahoo = true;
            d.yahoo = intradayTilt(intraday);
        }

        // Weighted blend over the AVAILABLE sources (weights renormalised) plus the
        // always-present market regime.
        double num = 0.0;
        double den = 0.0;
        if (d.haveTech) {
            num += 0.35 * techSigned;
            den += 0.35;
        }
        if (d.haveRating) {
            num += 0.25 * d.rating;
            den += 0.25;
        }
        if (d.haveNews) {
            num += 0.15 * d.newsScore;
            den += 0.15;
        }
        if (d.haveCrowd) {
            num += 0.10 * d.crowd;
            den += 0.10;
        }
        if (d.haveYahoo) {
            num += 0.10 * d.yahoo;
            den += 0.10;
        }
        num += 0.15 * regime;
        den += 0.15;
        d.composite = (den > 0.0) ? std::clamp(num / den, -1.0, 1.0) : 0.0;
        d.dir = (d.composite > 0.02) ? 1 : ((d.composite < -0.02) ? -1 : 0);
        d.confidence = std::abs(d.composite) * 100.0;
        if (eventRisk) {
            d.confidence *= 0.85;
        }
        rows.append(d);
    }
    const auto sortBegin = rows.begin();
    const auto sortEnd = rows.end();
    std::sort(sortBegin, sortEnd,
              [](const DecisionRow &a, const DecisionRow &b) { return a.confidence > b.confidence; });
    return rows;
}

QString webRatingWord(double score)
{
    if (score >= 0.5) {
        return QStringLiteral("Strong Buy");
    }
    if (score >= 0.1) {
        return QStringLiteral("Buy");
    }
    if (score > -0.1) {
        return QStringLiteral("Neutral");
    }
    if (score > -0.5) {
        return QStringLiteral("Sell");
    }
    return QStringLiteral("Strong Sell");
}

QString buildDecisionEvidence(const QList<DecisionRow> &rows, const MarketSnapshot &m)
{
    QString out = QStringLiteral(
        "Decide the single best instrument to trade right now from these candidates and "
        "their per-source signals. Sources: a technical indicator ensemble, TradingView's "
        "multi-timeframe rating (-1..1), a crude news-sentiment score (-1..1), the "
        "market VIX regime, and crowd sentiment (CNN Fear & Greed).\n\n");
    if (m.vixValid) {
        out += QStringLiteral("Market VIX: %1 (%2).\n")
                   .arg(m.vix, 0, 'f', 1)
                   .arg((m.vix >= 25.0) ? QStringLiteral("risk-off")
                                        : ((m.vix < 16.0) ? QStringLiteral("risk-on")
                                                          : QStringLiteral("neutral")));
    }
    if (m.fgValid) {
        out += QStringLiteral("Crowd sentiment (Fear & Greed): %1/100 (%2).\n")
                   .arg(qRound(m.fearGreed))
                   .arg((m.fearGreed <= 25.0)
                            ? QStringLiteral("fear")
                            : ((m.fearGreed >= 75.0) ? QStringLiteral("greed")
                                                     : QStringLiteral("neutral")));
    }
    out += QStringLiteral("Candidates (strongest composite first):\n");
    qint32 n = 0;
    for (const DecisionRow &d : rows) {
        if (d.dir == 0) {
            continue;
        }
        out += QStringLiteral("- %1: composite %2 (%3), maxLev x%4. ")
                   .arg(d.symbol)
                   .arg(d.composite, 0, 'f', 2)
                   .arg((d.dir > 0) ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                   .arg(d.maxLev);
        if (d.haveTech) {
            out += QStringLiteral("Technical %1 %2%. ").arg(d.techLabel).arg(qRound(d.techConf));
        }
        if (d.haveRating) {
            out += QStringLiteral("TV rating %1. ").arg(d.rating, 0, 'f', 2);
        }
        if (d.haveNews) {
            out += QStringLiteral("News %1 (%2 headlines). ")
                       .arg(d.newsScore, 0, 'f', 2)
                       .arg(d.newsCount);
        }
        const QList<NewsHeadline> news = m.newsBySymbol.value(d.symbol);
        for (qsizetype i = 0; (i < news.size()) && (i < 2); ++i) {
            out += QStringLiteral("[headline: %1] ").arg(news[i].title);
        }
        out += QLatin1Char('\n');
        if (++n >= 6) {
            break;
        }
    }
    if (n == 0) {
        out += QStringLiteral("(No actionable candidates yet — recommend HOLD.)\n");
    }
    out += QStringLiteral(
        "\nPick ONE instrument (must be one of the candidate symbols above), an action "
        "(BUY / SELL / HOLD), a confidence 0-100, a whole-number leverage no greater than "
        "that instrument's maxLev, and a concise rationale that cites the sources.");
    return out;
}

} // namespace trading
