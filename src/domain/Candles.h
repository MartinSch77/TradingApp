// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_CANDLES_H
#define TRADINGAPP_DOMAIN_CANDLES_H

#include <QList>

#include <optional>

// OHLC bars, and the two rules that decide whether a bar may be DRAWN (REQ-F-038).
//
// This exists because a price chart is a claim about what the market did, and the two ways
// it lies are both silent:
//
//  1. A HALF-PARSED BAR. Yahoo's chart response carries open/high/low/close as four
//     separate arrays with independent gaps — the same shape that already forced
//     `yahooBars` to parse closes and volumes together (a VWAP across misaligned arrays is
//     a number about nothing). Four arrays make it worse: index i can hold a real open, a
//     null high and a stale close, and a candle built from that renders a wick that never
//     happened. A bar survives here only when all four values are present, positive AND
//     mutually consistent; anything else is dropped whole, never repaired.
//  2. AN AXIS THAT CLIPS THE WICKS. The extremes of a session are its high and low, not the
//     highest and lowest CLOSE. An axis fitted to the bodies cuts the wicks off at the
//     frame and understates exactly the range a trader is reading the chart for.
//
// Domain, so both are reachable from a unit test with no window, no GPU and no network.
namespace trading {

// One bar. No timestamp: the series is contiguous minutes by construction (a bar that
// could not be parsed is absent, so position in the list is not a clock) and the cockpit
// labels the axis from the series length. Adding a time here would invite the reader to
// treat the index as one, which is the misreading the drop-whole-bars rule creates.
struct Candle {
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;

    // Up when the close is at or above the open. The tie goes to UP deliberately and it is
    // a display decision, not a market one: a flat bar drawn as a fall reads as a loss that
    // did not happen, and the renderer distinguishes the two by FILL rather than by colour
    // anyway (see PriceChart.qml).
    [[nodiscard]] bool up() const { return close >= open; }

    // The body, always non-negative — the delegate needs a height, not a signed move.
    [[nodiscard]] double bodyHeight() const { return (close >= open) ? (close - open) : (open - close); }
};

// The four raw columns of a bar feed, before any judgement.
//
// A named bundle rather than four loose QLists for the reason `ReadInputs` exists: four
// same-typed parameters in a row is an argument-order defect waiting to happen, and swapping
// highs with lows produces a chart that renders — every bar inverted, no error anywhere.
struct CandleColumns {
    QList<double> opens;
    QList<double> highs;
    QList<double> lows;
    QList<double> closes;
};

// The candles of four raw arrays, keeping only bars that can be drawn honestly.
//
// A bar at index i survives when, and only when:
//   * all four arrays reach index i (the shortest array ends the series), and
//   * every one of the four values is finite and strictly positive, and
//   * high >= max(open, close) and low <= min(open, close), and low <= high.
//
// The last test is the one that catches a partially-filled bar whose values individually
// look plausible. Such a bar is DROPPED rather than clamped into shape: clamping would
// invent a high the market never printed, and one invented extreme moves the whole axis.
[[nodiscard]] QList<Candle> candlesFrom(const QList<double> &opens, const QList<double> &highs,
                                        const QList<double> &lows, const QList<double> &closes);

// The vertical extent a chart must show: the lowest LOW and the highest HIGH.
struct CandleRange {
    double low = 0.0;
    double high = 0.0;
};

// The most recent `keep` candles, or all of them when there are fewer.
//
// Not a performance measure — a few hundred rectangles cost nothing. It is what makes the
// hollow-versus-solid body READABLE, and that is the chart's accessibility guarantee rather
// than a preference: direction is encoded by fill first and colour second, because the
// green-red pair is the worst case for deuteranopia (measured: CVD dE 4.1 between the status
// good and critical steps). A full session of 339 one-minute bars across a 940-pixel plot
// leaves each body about three pixels wide, which a one-pixel border fills completely — so
// every candle looks solid, the fill channel carries nothing, and the guarantee is void while
// the code still claims it. Fewer, wider candles keep the claim true.
//
// The TAIL, never the head: a price chart is read right-to-edge for what just happened.
[[nodiscard]] QList<Candle> recentCandles(const QList<Candle> &candles, qsizetype keep);

// The range of a series, or nothing when there is nothing to draw.
//
// std::optional rather than a zeroed range, because "no data" and "everything is at zero"
// are different facts and a chart that renders the second when it means the first is the
// stale-price failure in another costume. The caller must say "no data" out loud.
[[nodiscard]] std::optional<CandleRange> candleRange(const QList<Candle> &candles);

// The range widened by `fraction` of its own span on each side, so the extreme wicks do not
// sit on the frame. A series with no span at all (one bar, or a dead-flat market) is padded
// by a fraction of its LEVEL instead — padding a zero span by a fraction of zero leaves the
// axis degenerate, and Qt Graphs draws a degenerate axis as a single line through the middle
// with no gridlines, which reads as a broken chart rather than as a quiet market.
[[nodiscard]] CandleRange paddedRange(CandleRange range, double fraction);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_CANDLES_H
