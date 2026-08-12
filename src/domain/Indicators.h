// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_INDICATORS_H
#define TRADINGAPP_DOMAIN_INDICATORS_H

#include <QList>

// Classic technical indicators over a close-price series (oldest first).
// Pure functions of their inputs: no I/O, no UI, no shared state — the same
// series always yields the same reading, which keeps them unit-testable and
// shared verbatim between the live signals panel and the leverage screener.
namespace trading {

// Simple moving average of the last n values (0 if not enough data).
[[nodiscard]] double sma(const QList<double> &values, qsizetype n);

// Classic RSI over the last n deltas (returns -1 when there is not enough data).
[[nodiscard]] double rsi(const QList<double> &values, qsizetype n);

// Stochastic %K over the last n closes: where the latest price sits within the
// recent range (0 = bottom / oversold, 100 = top / overbought).
[[nodiscard]] double stochasticK(const QList<double> &values, qsizetype n);

// Exponential moving average at every point (seeded with the first value).
[[nodiscard]] QList<double> emaSeries(const QList<double> &values, qsizetype n);

// MACD histogram = MACD line (EMA12 - EMA26) minus its signal line (EMA9).
[[nodiscard]] double macdHistogram(const QList<double> &values);

// Bollinger %B over n periods: 0 = lower band, 0.5 = mean, 1 = upper band.
[[nodiscard]] double bollingerPercentB(const QList<double> &values, qsizetype n);

// Standard deviation of the last n simple returns, in percent (a volatility gauge).
[[nodiscard]] double volatilityPct(const QList<double> &values, qsizetype n);

// Rate of change over n periods, in percent.
[[nodiscard]] double roc(const QList<double> &values, qsizetype n);

// Per-bar fractional returns of a close series.
[[nodiscard]] QList<double> returnsOf(const QList<double> &series);

// Average per-bar simple return over the last n bars (the drift estimate).
[[nodiscard]] double meanReturn(const QList<double> &values, qsizetype n);

// Average True Range over the last n bars: the mean of each bar's true range
// (the largest of high-low, |high-prevClose| and |low-prevClose| — so a gap
// through the previous close counts as range even when that bar's own
// high-low is narrow). A simple average of the true ranges, not Wilder's
// smoothing — close enough for a stop-distance buffer, and it keeps this
// function as parameter-free and easy to reason about as its siblings above.
// 0 when there is not enough data (needs n+1 bars: the first true range needs
// a previous close). The three lists must be the same length and index-aligned
// (candlesFrom's own precondition) — a caller with unaligned OHLC has a worse
// problem than this function can catch.
[[nodiscard]] double averageTrueRange(const QList<double> &highs, const QList<double> &lows,
                                      const QList<double> &closes, qsizetype n);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_INDICATORS_H
