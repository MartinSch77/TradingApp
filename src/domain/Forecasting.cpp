#include "domain/Forecasting.h"

#include "domain/Indicators.h"

#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

namespace trading {

Regression linRegForecast(const QList<double> &values, qsizetype n)
{
    Regression r;
    if ((n < 3) || (values.size() < n)) {
        return r;
    }
    const qsizetype start = values.size() - n;
    const auto nD = static_cast<double>(n);
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;
    for (qsizetype i = 0; i < n; ++i) {
        const auto x = static_cast<double>(i);
        const double y = values[start + i];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        syy += y * y;
    }
    const double denom = (nD * sxx) - (sx * sx);
    if (denom == 0.0) {
        return r;
    }
    const double slope = ((nD * sxy) - (sx * sy)) / denom;
    const double intercept = (sy - (slope * sx)) / nD;
    const double meanY = sy / nD;
    double ssRes = 0.0;
    for (qsizetype i = 0; i < n; ++i) {
        const double e = values[start + i] - (intercept + (slope * static_cast<double>(i)));
        ssRes += e * e;
    }
    const double ssTot = syy - ((sy * sy) / nD);
    r.r2 = (ssTot > 0.0) ? (1.0 - (ssRes / ssTot)) : 0.0;
    r.slopePct = (meanY > 0.0) ? ((slope / meanY) * 100.0) : 0.0;
    r.valid = true;
    return r;
}

Knn knnForecast(const QList<double> &values, qsizetype window, qsizetype k)
{
    Knn out;
    if ((window < 3) || (values.size() < (window + k + 3))) {
        return out;
    }
    // Unlike returnsOf(), a non-positive previous value contributes a 0.0 return
    // here (the analog windows need one entry per bar to stay aligned).
    QList<double> r;
    r.reserve(values.size());
    for (qsizetype i = 1; i < values.size(); ++i) {
        r.append((values[i - 1] > 0.0) ? ((values[i] - values[i - 1]) / values[i - 1]) : 0.0);
    }
    const qsizetype m = r.size();
    if (m < (window + 1)) {
        return out;
    }

    struct DistFuture {
        double dist;
        double future;
    };
    QList<DistFuture> cand;
    cand.reserve(m);
    for (qsizetype j = window; j <= (m - 1); ++j) {
        double d = 0.0;
        for (qsizetype w = 0; w < window; ++w) {
            const double current = r[m - window + w];
            const double past = r[j - window + w];
            const double diff = current - past;
            d += diff * diff;
        }
        cand.append({d, r[j]});
    }
    if (cand.isEmpty()) {
        return out;
    }
    const auto sortBegin = cand.begin();
    const auto sortEnd = cand.end();
    std::sort(sortBegin, sortEnd,
              [](const DistFuture &a, const DistFuture &b) { return a.dist < b.dist; });

    const qsizetype kk = std::min(k, cand.size());
    double sum = 0.0;
    for (qsizetype i = 0; i < kk; ++i) {
        sum += cand[i].future;
    }
    const double mean = sum / static_cast<double>(kk);
    qint32 agreeing = 0;
    for (qsizetype i = 0; i < kk; ++i) {
        if ((mean >= 0.0) == (cand[i].future >= 0.0)) {
            ++agreeing;
        }
    }
    out.retPct = mean * 100.0;
    out.agree = static_cast<double>(agreeing) / static_cast<double>(kk);
    out.k = static_cast<qint32>(kk);
    return out;
}

double hurstExponent(const QList<double> &series)
{
    const QList<double> ret = returnsOf(series);
    if (ret.size() < 20) {
        return 0.5;
    }
    const qsizetype n = std::min<qsizetype>(ret.size(), 120);
    const qsizetype start = ret.size() - n;
    double mean = 0.0;
    for (qsizetype i = start; i < ret.size(); ++i) {
        mean += ret[i];
    }
    mean /= static_cast<double>(n);
    double cum = 0.0;
    double minc = 0.0;
    double maxc = 0.0;
    double var = 0.0;
    for (qsizetype i = start; i < ret.size(); ++i) {
        const double d = ret[i] - mean;
        cum += d;
        minc = std::min(minc, cum);
        maxc = std::max(maxc, cum);
        var += d * d;
    }
    const double range = maxc - minc;
    const double stddev = std::sqrt(var / static_cast<double>(n));
    if ((stddev <= 0.0) || (range <= 0.0)) {
        return 0.5;
    }
    const double logRs = std::log(range / stddev);
    const double logN = std::log(static_cast<double>(n));
    return logRs / logN;
}

McOutlook monteCarlo(const QList<double> &series, double price, qint32 horizon,
                     double tpFrac, double slFrac, qint32 paths)
{
    McOutlook o;
    const QList<double> ret = returnsOf(series);
    if ((ret.size() < 10) || (price <= 0.0) || (horizon <= 0) || (paths <= 0)) {
        return o;
    }
    const auto m = static_cast<qint32>(ret.size());
    const bool haveBarriers = (tpFrac > 0.0) && (slFrac > 0.0);
    QRandomGenerator *rng = QRandomGenerator::global();

    QList<double> finals;
    finals.reserve(paths);
    qint32 upCount = 0;
    qint32 winLong = 0;
    qint32 winShort = 0;
    qint32 loseLong = 0;
    qint32 loseShort = 0;
    for (qint32 p = 0; p < paths; ++p) {
        double cum = 0.0;  // cumulative fractional move from the current price
        bool longDone = false;
        bool shortDone = false;
        for (qint32 t = 0; t < horizon; ++t) {
            const auto pickIdx = static_cast<qsizetype>(rng->bounded(m));
            cum = ((1.0 + cum) * (1.0 + ret[pickIdx])) - 1.0;
            if (haveBarriers) {
                if (!longDone) {
                    if (cum >= tpFrac) {
                        ++winLong;
                        longDone = true;
                    } else if (cum <= -slFrac) {
                        ++loseLong;
                        longDone = true;
                    } else {
                        // barrier not reached yet — keep simulating
                    }
                }
                if (!shortDone) {
                    if (cum <= -tpFrac) {
                        ++winShort;
                        shortDone = true;
                    } else if (cum >= slFrac) {
                        ++loseShort;
                        shortDone = true;
                    } else {
                        // barrier not reached yet — keep simulating
                    }
                }
            }
        }
        if (cum > 0.0) {
            ++upCount;
        }
        finals.append(price * (1.0 + cum));
    }
    const auto sortBegin = finals.begin();
    const auto sortEnd = finals.end();
    std::sort(sortBegin, sortEnd);
    const auto pathsD = static_cast<double>(paths);
    o.valid = true;
    o.pUp = static_cast<double>(upCount) / pathsD;
    o.p5 = finals[static_cast<qsizetype>(0.05 * pathsD)];
    o.p95 = finals[std::min<qsizetype>(paths - 1, static_cast<qsizetype>(0.95 * pathsD))];
    o.pWinLong = static_cast<double>(winLong) / pathsD;
    o.pWinShort = static_cast<double>(winShort) / pathsD;
    o.pLoseLong = static_cast<double>(loseLong) / pathsD;
    o.pLoseShort = static_cast<double>(loseShort) / pathsD;
    return o;
}

double sigmoid(double x)
{
    return 1.0 / (1.0 + std::exp(-x));
}

} // namespace trading
