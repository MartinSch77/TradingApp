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

} // namespace trading

#endif // TRADINGAPP_DOMAIN_INDICATORS_H
