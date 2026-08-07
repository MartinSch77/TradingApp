// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/Candles.h"

#include <algorithm>
#include <cmath>

namespace trading {

namespace {

// A value that may be drawn: finite and strictly positive. NaN fails every comparison, so
// an inconsistent-bar test alone would let it through — std::isfinite is what stops a NaN
// high from becoming an axis maximum of NaN and blanking the entire chart.
[[nodiscard]] bool drawable(double value)
{
    return std::isfinite(value) && (value > 0.0);
}

} // namespace

QList<Candle> candlesFrom(const QList<double> &opens, const QList<double> &highs,
                          const QList<double> &lows, const QList<double> &closes)
{
    // The shortest array ends the series. Not the longest padded with zeros: a padded bar
    // is a fabricated one, and it would sit at the RIGHT-HAND edge where a reader looks for
    // the latest price.
    const qsizetype bars =
        std::min({opens.size(), highs.size(), lows.size(), closes.size()});

    QList<Candle> out;
    out.reserve(bars);
    for (qsizetype i = 0; i < bars; ++i) {
        const Candle candle{opens.at(i), highs.at(i), lows.at(i), closes.at(i)};
        if (!drawable(candle.open) || !drawable(candle.high) || !drawable(candle.low)
            || !drawable(candle.close)) {
            continue;   // an empty or impossible minute: drop the whole bar, never half of one
        }
        // Internal consistency. A bar whose high is below its own close describes no minute
        // that ever happened, so it is dropped rather than repaired.
        if ((candle.high < std::max(candle.open, candle.close))
            || (candle.low > std::min(candle.open, candle.close)) || (candle.low > candle.high)) {
            continue;
        }
        out.append(candle);
    }
    return out;
}

QList<Candle> recentCandles(const QList<Candle> &candles, qsizetype keep)
{
    if ((keep <= 0) || (candles.size() <= keep)) {
        return candles;
    }
    return candles.last(keep);
}

std::optional<CandleRange> candleRange(const QList<Candle> &candles)
{
    if (candles.isEmpty()) {
        return std::nullopt;
    }
    CandleRange range{candles.first().low, candles.first().high};
    for (const Candle &candle : candles) {
        range.low = std::min(range.low, candle.low);
        range.high = std::max(range.high, candle.high);
    }
    return range;
}

CandleRange paddedRange(CandleRange range, double fraction)
{
    const double span = range.high - range.low;
    // A flat or single-bar series has no span to take a fraction of, so the padding comes
    // off the LEVEL instead. Without this the axis min equals its max, and Qt Graphs renders
    // that as one line through the middle of an empty frame — indistinguishable from a
    // failed chart, when the truth is a quiet market.
    const double pad = (span > 0.0) ? (span * fraction) : (std::abs(range.high) * fraction);
    // Still degenerate only if the level is zero too, which `candlesFrom` already excludes;
    // the guard costs nothing and keeps this function total.
    const double safePad = (pad > 0.0) ? pad : 1.0;
    return CandleRange{range.low - safePad, range.high + safePad};
}

} // namespace trading
