#include "domain/SignalEnsemble.h"

#include "domain/Forecasting.h"
#include "domain/Indicators.h"

#include <cmath>

namespace trading {

Ensemble computeEnsemble(const QList<double> &series, bool vixValid, double vixChangePct)
{
    Ensemble e;
    constexpr qsizetype kFast = 10;
    constexpr qsizetype kSlow = 30;
    constexpr qsizetype kRsi = 14;
    if (series.size() < (kSlow + 1)) {
        return e;  // invalid: caller shows "gathering data..."
    }

    const double fast = sma(series, kFast);
    const double slow = sma(series, kSlow);
    const double r = rsi(series, kRsi);
    const double hist = macdHistogram(series);
    const double pctB = bollingerPercentB(series, 20);
    const double momentum = roc(series, 10);
    const Regression reg = linRegForecast(series, 30);
    const Knn kn = knnForecast(series, 10, 5);
    const double stochK = stochasticK(series, 14);
    const double sma50 = sma(series, 50);
    const bool aboveTrend = (sma50 > 0.0) && (series.last() > sma50);
    const bool bull = fast > slow;
    e.vol = volatilityPct(series, 20);

    qint32 score = 0;
    qint32 votes = 0;
    auto vote = [&score, &votes](qint32 v) {
        score += v;
        ++votes;
    };
    vote(bull ? 1 : -1);                                        // trend
    vote((hist >= 0.0) ? 1 : -1);                               // MACD
    vote((momentum > 0.0) ? 1 : ((momentum < 0.0) ? -1 : 0));   // ROC
    vote((r > 55.0) ? 1 : ((r < 45.0) ? -1 : 0));               // RSI bias
    vote((pctB < 0.2) ? 1 : ((pctB > 0.8) ? -1 : 0));           // mean-reversion at bands
    if (reg.valid && (reg.r2 > 0.2)) {                          // regression trend (decent fit only)
        vote((reg.slopePct > 0.0) ? 1 : -1);
    }
    if ((kn.k > 0) && (kn.agree >= 0.6)) {                      // kNN analog consensus
        vote((kn.retPct > 0.0) ? 1 : -1);
    }
    vote((stochK < 20.0) ? 1 : ((stochK > 80.0) ? -1 : 0));     // stochastic timing
    if (sma50 > 0.0) {                                          // long-term trend regime
        vote(aboveTrend ? 1 : -1);
    }
    if (vixValid) {                                             // fear index deviation
        if (vixChangePct > 8.0) {
            vote(-1);
        } else if (vixChangePct < -8.0) {
            vote(1);
        } else {
            // VIX close to its norm — no directional vote
        }
    }

    e.valid = true;
    e.score = score;
    e.votes = votes;
    e.confidence =
        (votes > 0) ? ((std::abs(score) / static_cast<double>(votes)) * 100.0) : 0.0;
    e.dir = (score > 0) ? 1 : ((score < 0) ? -1 : 0);
    if (score >= 2) {
        e.signal = QStringLiteral("BUY");
        e.signalDir = 1;
    } else if (score <= -2) {
        e.signal = QStringLiteral("SELL");
        e.signalDir = -1;
    } else {
        e.signal = QStringLiteral("NEUTRAL");
        e.signalDir = 0;
    }
    return e;
}

double applyVixHaircut(double confidence, bool vixValid, double vixLevel)
{
    if (vixValid && (vixLevel >= 25.0)) {
        return confidence * ((vixLevel >= 35.0) ? 0.6 : 0.8);
    }
    return confidence;
}

} // namespace trading
