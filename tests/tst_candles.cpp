// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The candlestick chart's data rules (REQ-F-038, DES-DOM-CANDLES).
//
// A price chart is a claim about what the market did, and both of its failure modes are
// SILENT: a bar assembled from four arrays with independent gaps draws a wick that never
// happened, and an axis fitted to the closes cuts the session's real high and low off at the
// frame. Neither raises an error and neither looks wrong. These tests are the only place
// either is caught.

#include "domain/Candles.h"

#include <QtTest/QtTest>

#include <cmath>
#include <limits>

using namespace trading;

namespace {

// A bar that is internally consistent, for use as the "good" element around the bad ones.
constexpr double kOpen = 100.0;
constexpr double kHigh = 102.0;
constexpr double kLow = 99.0;
constexpr double kClose = 101.0;

} // namespace

class TestCandles : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-CANDLE-001 @design DES-DOM-CANDLES
    // @relation(REQ-F-038, scope=function)
    //
    // A bar is dropped WHOLE when any one of its four values cannot be drawn. The four
    // arrays come from one Yahoo response with independent gaps, so index i can hold a real
    // open beside a null high; keeping the half that parsed would draw a candle nobody
    // traded.
    void TS_CANDLE_001_halfParsedBarsAreDroppedWhole()
    {
        // index 1 has a missing (0) high, index 2 a negative low, index 3 a NaN close.
        const QList<double> opens{kOpen, kOpen, kOpen, kOpen, kOpen};
        const QList<double> highs{kHigh, 0.0, kHigh, kHigh, kHigh};
        const QList<double> lows{kLow, kLow, -5.0, kLow, kLow};
        const QList<double> closes{kClose, kClose, kClose,
                                   std::numeric_limits<double>::quiet_NaN(), kClose};

        const QList<Candle> candles = candlesFrom(opens, highs, lows, closes);

        // Only the two sound bars survive; nothing was repaired into existence.
        QCOMPARE(candles.size(), 2);
        for (const Candle &candle : candles) {
            QCOMPARE(candle.open, kOpen);
            QCOMPARE(candle.high, kHigh);
            QCOMPARE(candle.low, kLow);
            QCOMPARE(candle.close, kClose);
        }
    }

    //! @tstid TS-CANDLE-002 @design DES-DOM-CANDLES
    // @relation(REQ-F-038, scope=function)
    //
    // An internally inconsistent bar is DROPPED, never clamped into shape. Each of the three
    // values below is individually plausible — the defect is only visible in the
    // relationship — and clamping would invent an extreme that then moves the whole axis.
    void TS_CANDLE_002_inconsistentBarsAreDroppedNotClamped()
    {
        // 0: high below the close.   1: low above the open.   2: low above high.
        const QList<double> opens{100.0, 100.0, 100.0, kOpen};
        const QList<double> highs{100.5, 102.0, 99.0, kHigh};
        const QList<double> lows{99.0, 100.5, 101.0, kLow};
        const QList<double> closes{101.0, 101.0, 100.0, kClose};

        const QList<Candle> candles = candlesFrom(opens, highs, lows, closes);

        QCOMPARE(candles.size(), 1);
        // The survivor is the untouched good bar — not a clamped version of a bad one.
        QCOMPARE(candles.first().high, kHigh);
        QCOMPARE(candles.first().low, kLow);
    }

    //! @tstid TS-CANDLE-003 @design DES-DOM-CANDLES
    // @relation(REQ-F-038, scope=function)
    //
    // The SHORTEST array ends the series. Padding the others with zeros would fabricate bars
    // at the right-hand edge, which is precisely where a reader looks for the latest price.
    void TS_CANDLE_003_theShortestArrayEndsTheSeries()
    {
        const QList<double> opens{kOpen, kOpen, kOpen};
        const QList<double> highs{kHigh, kHigh, kHigh};
        const QList<double> lows{kLow, kLow};            // one short
        const QList<double> closes{kClose, kClose, kClose};

        QCOMPARE(candlesFrom(opens, highs, lows, closes).size(), 2);
    }

    //! @tstid TS-CANDLE-004 @design DES-DOM-CANDLES
    // @relation(REQ-F-038, scope=function)
    //
    // The vertical range covers the WICKS. An axis fitted to the bodies would end at 101.0
    // here and cut off both the 105.0 high and the 95.0 low — understating exactly the range
    // the chart is being read for.
    void TS_CANDLE_004_theRangeCoversTheWicksNotTheBodies()
    {
        const QList<Candle> candles = candlesFrom({100.0, 100.0}, {102.0, 105.0},
                                                  {99.0, 95.0}, {101.0, 100.0});
        QCOMPARE(candles.size(), 2);

        const std::optional<CandleRange> range = candleRange(candles);
        QVERIFY(range.has_value());
        // value_or rather than operator->: QVERIFY does return on failure, but clang-tidy's
        // dataflow does not model a macro's return as a guard and reports the access as
        // unchecked. The default is unreachable given the line above.
        const CandleRange got = range.value_or(CandleRange{});
        QCOMPARE(got.low, 95.0);
        QCOMPARE(got.high, 105.0);

        // Padding widens the frame so the extremes do not sit on it, and never narrows it.
        const CandleRange padded = paddedRange(got, 0.04);
        QVERIFY(padded.low < 95.0);
        QVERIFY(padded.high > 105.0);
    }

    //! @tstid TS-CANDLE-005 @design DES-DOM-CANDLES
    // @relation(REQ-F-038, scope=function)
    //
    // Two absent-versus-zero distinctions in one place. No bars gives NO range, so a caller
    // cannot render "nothing arrived" as "everything is at zero"; and a dead-flat series
    // still gets a usable axis, because padding a zero span by a fraction of itself leaves
    // min == max, which Qt Graphs draws as a single line through an empty frame — a quiet
    // market would look like a broken chart.
    void TS_CANDLE_005_noBarsGiveNoRangeAndAFlatSeriesStillGetsAnAxis()
    {
        QVERIFY(!candleRange({}).has_value());

        const QList<Candle> flat = candlesFrom({100.0}, {100.0}, {100.0}, {100.0});
        QCOMPARE(flat.size(), 1);
        const std::optional<CandleRange> range = candleRange(flat);
        QVERIFY(range.has_value());
        const CandleRange got = range.value_or(CandleRange{1.0, 2.0});
        QCOMPARE(got.low, got.high);                // the span really is zero
        // The value_or default deliberately has a NON-zero span, so if the optional were
        // ever empty this comparison would fail rather than pass by accident.

        const CandleRange padded = paddedRange(got, 0.04);
        QVERIFY(padded.high > padded.low);          // …and the axis is still not degenerate
    }

    //! @tstid TS-CANDLE-006 @design DES-DOM-CANDLES
    // @relation(REQ-F-038, scope=function)
    //
    // A flat bar counts as UP, and the rule lives here rather than in a QML binding. Drawing
    // an unchanged minute as a fall reads as a loss that did not happen; pinning it means the
    // renderer and any future reader cannot each pick their own answer.
    void TS_CANDLE_006_aFlatBarCountsAsUp()
    {
        const Candle flat{100.0, 100.0, 100.0, 100.0};
        QVERIFY(flat.up());
        QCOMPARE(flat.bodyHeight(), 0.0);

        const Candle down{101.0, 101.5, 99.0, 100.0};
        QVERIFY(!down.up());
        // The body height is a size, never a signed move — the delegate needs a height.
        QCOMPARE(down.bodyHeight(), 1.0);
    }

    //! @tstid TS-CANDLE-007 @design DES-DOM-CANDLES
    // @relation(REQ-F-038, scope=function)
    //
    // Only the most recent bars are drawn, and the cut takes the TAIL. This is the chart's
    // accessibility guarantee rather than a performance measure: direction is encoded by a
    // hollow-versus-solid body because green/red is the worst pair for deuteranopia, and a
    // full 339-bar session leaves each body about three pixels wide — which a one-pixel
    // border fills completely, so every candle looks solid and the fill channel carries
    // nothing. Taking the HEAD instead would also be wrong in a way no test of the count
    // would catch: a price chart is read at its right edge for what just happened.
    void TS_CANDLE_007_onlyTheMostRecentBarsAreDrawnAndTheCutTakesTheTail()
    {
        QList<Candle> many;
        for (qint32 i = 0; i < 300; ++i) {
            const double base = 100.0 + i;
            many.append(Candle{base, base + 2.0, base - 1.0, base + 1.0});
        }

        const QList<Candle> kept = recentCandles(many, 120);
        QCOMPARE(kept.size(), 120);
        // The LAST 120: the newest bar survives and the oldest does not.
        QCOMPARE(kept.last().open, many.last().open);
        QCOMPARE(kept.first().open, many.at(180).open);

        // Fewer bars than the cap are returned untouched, and a cap of zero means "no cap"
        // rather than "draw nothing" — a limit that could silently empty the chart would be
        // a worse failure than the one it exists to prevent.
        QCOMPARE(recentCandles(many, 500).size(), 300);
        QCOMPARE(recentCandles(many, 0).size(), 300);
    }
};

QTEST_MAIN(TestCandles)
#include "tst_candles.moc"
