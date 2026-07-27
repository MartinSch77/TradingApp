#include "domain/Indicators.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace trading {

double sma(const QList<double> &values, qsizetype n)
{
    if ((n <= 0) || (values.size() < n)) {
        return 0.0;
    }
    double sum = 0.0;
    for (qsizetype i = values.size() - n; i < values.size(); ++i) {
        sum += values[i];
    }
    return sum / static_cast<double>(n);
}

double rsi(const QList<double> &values, qsizetype n)
{
    if ((n <= 0) || (values.size() < (n + 1))) {
        return -1.0;
    }
    double gain = 0.0;
    double loss = 0.0;
    for (qsizetype i = values.size() - n; i < values.size(); ++i) {
        const double d = values[i] - values[i - 1];
        if (d >= 0.0) {
            gain += d;
        } else {
            loss -= d;
        }
    }
    if (loss <= 0.0) {
        return 100.0;
    }
    const double rs = gain / loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

double stochasticK(const QList<double> &values, qsizetype n)
{
    if ((n <= 0) || (values.size() < n)) {
        return 50.0;
    }
    double lo = values[values.size() - n];
    double hi = lo;
    for (qsizetype i = values.size() - n; i < values.size(); ++i) {
        lo = std::min(lo, values[i]);
        hi = std::max(hi, values[i]);
    }
    return ((hi - lo) > 0.0) ? (((values.last() - lo) / (hi - lo)) * 100.0) : 50.0;
}

QList<double> emaSeries(const QList<double> &values, qsizetype n)
{
    QList<double> out;
    if (values.isEmpty() || (n <= 0)) {
        return out;
    }
    const double k = 2.0 / static_cast<double>(n + 1);
    double e = values.first();
    out.reserve(values.size());
    out.append(e);
    for (qsizetype i = 1; i < values.size(); ++i) {
        e = (values[i] * k) + (e * (1.0 - k));
        out.append(e);
    }
    return out;
}

double macdHistogram(const QList<double> &values)
{
    if (values.size() < 35) {
        return 0.0;
    }
    constexpr qsizetype kFastPeriod = 12;
    constexpr qsizetype kSlowPeriod = 26;
    constexpr qsizetype kSignalPeriod = 9;
    const QList<double> fast = emaSeries(values, kFastPeriod);
    const QList<double> slow = emaSeries(values, kSlowPeriod);
    QList<double> macd;
    macd.reserve(values.size());
    for (qsizetype i = 0; i < values.size(); ++i) {
        macd.append(fast[i] - slow[i]);
    }
    const QList<double> signal = emaSeries(macd, kSignalPeriod);
    return macd.last() - signal.last();
}

double bollingerPercentB(const QList<double> &values, qsizetype n)
{
    if ((values.size() < n) || (n <= 0)) {
        return 0.5;
    }
    double mean = 0.0;
    for (qsizetype i = values.size() - n; i < values.size(); ++i) {
        mean += values[i];
    }
    mean /= static_cast<double>(n);
    double var = 0.0;
    for (qsizetype i = values.size() - n; i < values.size(); ++i) {
        const double d = values[i] - mean;
        var += d * d;
    }
    const double sd = std::sqrt(var / static_cast<double>(n));
    const double lower = mean - (2.0 * sd);
    const double width = 4.0 * sd;
    return (width > 0.0) ? ((values.last() - lower) / width) : 0.5;
}

double volatilityPct(const QList<double> &values, qsizetype n)
{
    if (values.size() < (n + 1)) {
        return 0.0;
    }
    QList<double> r;
    for (qsizetype i = values.size() - n; i < values.size(); ++i) {
        if (values[i - 1] > 0.0) {
            r.append((values[i] - values[i - 1]) / values[i - 1]);
        }
    }
    if (r.isEmpty()) {
        return 0.0;
    }
    double mean = 0.0;
    for (const double x : std::as_const(r)) {
        mean += x;
    }
    mean /= static_cast<double>(r.size());
    double var = 0.0;
    for (const double x : std::as_const(r)) {
        const double d = x - mean;
        var += d * d;
    }
    return std::sqrt(var / static_cast<double>(r.size())) * 100.0;
}

double roc(const QList<double> &values, qsizetype n)
{
    if (values.size() < (n + 1)) {
        return 0.0;
    }
    const double base = values[values.size() - 1 - n];
    return (base > 0.0) ? (((values.last() - base) / base) * 100.0) : 0.0;
}

QList<double> returnsOf(const QList<double> &series)
{
    QList<double> r;
    r.reserve(series.size());
    for (qsizetype i = 1; i < series.size(); ++i) {
        if (series[i - 1] > 0.0) {
            r.append((series[i] - series[i - 1]) / series[i - 1]);
        }
    }
    return r;
}

double meanReturn(const QList<double> &values, qsizetype n)
{
    if ((n <= 0) || (values.size() < (n + 1))) {
        return 0.0;
    }
    double sum = 0.0;
    qint32 count = 0;
    for (qsizetype i = values.size() - n; i < values.size(); ++i) {
        if (values[i - 1] > 0.0) {
            sum += (values[i] - values[i - 1]) / values[i - 1];
            ++count;
        }
    }
    return (count > 0) ? (sum / static_cast<double>(count)) : 0.0;
}

} // namespace trading
