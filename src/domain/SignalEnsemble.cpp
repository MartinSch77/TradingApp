#include "domain/SignalEnsemble.h"

#include "domain/Forecasting.h"
#include "domain/Indicators.h"

#include <array>
#include <cmath>
#include <optional>

namespace {

using trading::Knn;
using trading::Regression;

// Everything the voters read, computed once per ensemble run. Plain data so
// each voter stays a pure function of it.
struct Features {
    double fast = 0.0;      // SMA(10)
    double slow = 0.0;      // SMA(30)
    double rsi = 0.0;       // RSI(14)
    double macdHist = 0.0;  // MACD(12/26/9) histogram
    double pctB = 0.0;      // Bollinger %B(20)
    double momentum = 0.0;  // ROC(10)
    Regression reg;         // OLS trend over 30 bars
    Knn knn;                // kNN analog forecast (window 10, k 5)
    double stochK = 0.0;    // stochastic %K(14)
    double sma50 = 0.0;     // long-term trend line (0 = not enough bars)
    double last = 0.0;      // newest close
    bool vixValid = false;
    double vixChangePct = 0.0;
};

// One independent signal strategy: reads the features and either casts a vote
// (+1 bull / -1 bear / 0 counted-but-neutral) or abstains (nullopt) when its
// precondition isn't met. An abstention does NOT dilute the confidence — only
// cast votes are counted.
using Voter = std::optional<qint32> (*)(const Features &);

// The ensemble's voter table. Adding a signal = computing its feature above and
// appending one entry here; the tally in computeEnsemble never changes.
constexpr std::array<Voter, 10> kVoters = {
    // Trend: fast SMA above slow.
    [](const Features &f) -> std::optional<qint32> {
        return (f.fast > f.slow) ? 1 : -1;
    },
    // MACD histogram sign.
    [](const Features &f) -> std::optional<qint32> {
        return (f.macdHist >= 0.0) ? 1 : -1;
    },
    // Momentum (rate of change).
    [](const Features &f) -> std::optional<qint32> {
        return (f.momentum > 0.0) ? 1 : ((f.momentum < 0.0) ? -1 : 0);
    },
    // RSI bias.
    [](const Features &f) -> std::optional<qint32> {
        return (f.rsi > 55.0) ? 1 : ((f.rsi < 45.0) ? -1 : 0);
    },
    // Mean-reversion at the Bollinger bands.
    [](const Features &f) -> std::optional<qint32> {
        return (f.pctB < 0.2) ? 1 : ((f.pctB > 0.8) ? -1 : 0);
    },
    // Regression trend — decent fit only, else abstain.
    [](const Features &f) -> std::optional<qint32> {
        if (f.reg.valid && (f.reg.r2 > 0.2)) {
            return (f.reg.slopePct > 0.0) ? 1 : -1;
        }
        return std::nullopt;
    },
    // kNN analog consensus — clear neighbour agreement only, else abstain.
    [](const Features &f) -> std::optional<qint32> {
        if ((f.knn.k > 0) && (f.knn.agree >= 0.6)) {
            return (f.knn.retPct > 0.0) ? 1 : -1;
        }
        return std::nullopt;
    },
    // Stochastic timing.
    [](const Features &f) -> std::optional<qint32> {
        return (f.stochK < 20.0) ? 1 : ((f.stochK > 80.0) ? -1 : 0);
    },
    // Long-term trend regime (needs the SMA(50) to exist).
    [](const Features &f) -> std::optional<qint32> {
        if (f.sma50 > 0.0) {
            return (f.last > f.sma50) ? 1 : -1;
        }
        return std::nullopt;
    },
    // Fear-index deviation: a VIX stretched far from its norm is risk-off/on;
    // close to the norm it casts no directional vote.
    [](const Features &f) -> std::optional<qint32> {
        if (f.vixValid) {
            if (f.vixChangePct > 8.0) {
                return -1;
            }
            if (f.vixChangePct < -8.0) {
                return 1;
            }
        }
        return std::nullopt;
    },
};

Features computeFeatures(const QList<double> &series, bool vixValid, double vixChangePct)
{
    Features f;
    f.fast = trading::sma(series, 10);
    f.slow = trading::sma(series, 30);
    f.rsi = trading::rsi(series, 14);
    f.macdHist = trading::macdHistogram(series);
    f.pctB = trading::bollingerPercentB(series, 20);
    f.momentum = trading::roc(series, 10);
    f.reg = trading::linRegForecast(series, 30);
    f.knn = trading::knnForecast(series, 10, 5);
    f.stochK = trading::stochasticK(series, 14);
    f.sma50 = trading::sma(series, 50);
    f.last = series.last();
    f.vixValid = vixValid;
    f.vixChangePct = vixChangePct;
    return f;
}

} // namespace

namespace trading {

Ensemble computeEnsemble(const QList<double> &series, bool vixValid, double vixChangePct)
{
    Ensemble e;
    constexpr qsizetype kSlow = 30;
    if (series.size() < (kSlow + 1)) {
        return e;  // invalid: caller shows "gathering data..."
    }

    const Features f = computeFeatures(series, vixValid, vixChangePct);
    e.vol = volatilityPct(series, 20);

    qint32 score = 0;
    qint32 votes = 0;
    for (const auto &voter : kVoters) {
        if (const std::optional<qint32> v = voter(f)) {
            score += *v;
            ++votes;
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

double applyVixHaircut(double confidence, bool vixValid, double vixLevel) noexcept
{
    if (vixValid && (vixLevel >= 25.0)) {
        return confidence * ((vixLevel >= 35.0) ? 0.6 : 0.8);
    }
    return confidence;
}

} // namespace trading
