// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The declarative cockpit's data, tested WITHOUT rendering (REQ-F-038).
//
// This file is the reason the shaping lives in C++ rather than in QML: every claim the view
// makes is checked here, headless, with no window, no GPU and no screenshot. A screenshot
// proves that something was drawn; it cannot prove that an unmeasurable read was reported as
// unmeasurable, or that a stale price was not rendered as a live one.

#include "ui/CockpitModel.h"

#include <QtTest/QtTest>

using namespace trading;
using namespace trading::ui;

namespace {

// A read that was taken, supporting `dir`.
Read measured(qint32 dir, const QString &detail = QStringLiteral("x"))
{
    Read read;
    read.known = true;
    read.dir = dir;
    read.detail = detail;
    return read;
}

// A read that could NOT be taken. Note it still carries a dir, deliberately: the tests below
// prove that dir is ignored when known is false.
Read absent(qint32 misleadingDir = 1)
{
    Read read;
    read.known = false;
    read.dir = misleadingDir;
    read.detail = QStringLiteral("no series");
    return read;
}

} // namespace

class TestCockpitModel : public QObject
{
    Q_OBJECT;

private slots:
    //! @tstid TS-COCKPIT-001 @design DES-UI-COCKPIT
    // @relation(REQ-F-038, REQ-F-035, scope=function)
    void TS_COCKPIT_001_anUnmeasurableReadIsNeverCountedAsAgreement()
    {
        // Four measured reads supporting a long, five that could not be taken — and every
        // absent one carrying dir = +1, which would look like agreement to a naive count.
        IndexReads reads;
        reads.futuresLead = measured(1);
        reads.futuresMomentum = measured(1);
        reads.volatility = measured(1);
        reads.yields = measured(1);
        reads.curve = absent();
        reads.participation = absent();
        reads.aboveVwap = absent();
        reads.upDownVolume = absent();
        reads.structure = absent();

        const QList<ReadTick> ticks = cockpitTicks(reads, 1);
        QCOMPARE(ticks.size(), qsizetype(9));

        qint32 agreeing = 0;
        qint32 unmeasurable = 0;
        for (const ReadTick &tick : ticks) {
            if (tick.state == QStringLiteral("agrees")) {
                ++agreeing;
            }
            if (tick.state == QStringLiteral("unmeasurable")) {
                ++unmeasurable;
            }
        }
        // The whole point: 4, not 9. An absent read's dir is not evidence.
        QCOMPARE(agreeing, 4);
        QCOMPARE(unmeasurable, 5);

        // …and the text states BOTH facts. "4 of 9" alone would be true and misleading.
        const QString text = cockpitAgreementText(ticks);
        QVERIFY(text.contains(QStringLiteral("4 of 9 agree")));
        QVERIFY(text.contains(QStringLiteral("5 unmeasurable")));
    }

    //! @tstid TS-COCKPIT-002 @design DES-UI-COCKPIT
    // @relation(REQ-F-038, scope=function)
    void TS_COCKPIT_002_everyStateIsCarriedByAGlyphAndNotByColour()
    {
        IndexReads reads;
        reads.futuresLead = measured(1);      // agrees with a long
        reads.futuresMomentum = measured(-1); // disagrees
        reads.volatility = measured(0);       // measured but NEUTRAL — not agreement
        reads.yields = absent();
        reads.curve = measured(1);
        reads.participation = measured(1);
        reads.aboveVwap = measured(1);
        reads.upDownVolume = measured(1);
        reads.structure = measured(1);

        const QList<ReadTick> ticks = cockpitTicks(reads, 1);

        // Three distinct glyphs, differing in SHAPE (filled / hollow / crossed) so the
        // states survive without colour — the up/down pair is exactly what deuteranopia
        // cannot separate.
        QCOMPARE(ticks.at(0).glyph, QStringLiteral("●"));
        QCOMPARE(ticks.at(1).glyph, QStringLiteral("○"));
        QCOMPARE(ticks.at(3).glyph, QStringLiteral("✕"));
        // A measured-but-neutral read does NOT agree: counting it would inflate the
        // fraction with reads that said nothing.
        QCOMPARE(ticks.at(2).state, QStringLiteral("disagrees"));
        // Every tick also carries its state as TEXT, so identity never rests on the glyph
        // alone either.
        for (const ReadTick &tick : ticks) {
            QVERIFY(!tick.state.isEmpty());
            QVERIFY(!tick.glyph.isEmpty());
            QVERIFY(!tick.label.isEmpty());
        }
    }

    //! @tstid TS-COCKPIT-003 @design DES-UI-COCKPIT
    // @relation(REQ-F-038, REQ-F-037, scope=function)
    void TS_COCKPIT_003_evidenceIsNeverDressedUpAsAProbability()
    {
        LeadSignal signal;
        signal.dir = 1;
        signal.strength = 74.0;
        signal.grade = LeadGrade::Strong;

        const QString evidence = cockpitEvidenceText(signal);
        QVERIFY(evidence.contains(QStringLiteral("LONG")));
        QVERIFY(evidence.contains(QStringLiteral("evidence 74 of 100")));
        // The forbidden rendering: 74 with a percent sign, or the word "confidence", either
        // of which reads as a claim about frequency. It is a weighted sum of indicators.
        QVERIFY(!evidence.contains(QStringLiteral("74%")));
        QVERIFY(!evidence.contains(QStringLiteral("confidence"), Qt::CaseInsensitive));

        // Below the sample floor there is NO number, and the shortfall is named.
        const QString thin = cockpitProbabilityText(11, 40, 0.68);
        QVERIFY(thin.contains(QStringLiteral("UNCALIBRATED")));
        QVERIFY(thin.contains(QStringLiteral("11 of 40")));
        QVERIFY(!thin.contains(QStringLiteral("68")));

        // At or above it, a MEASURED hit rate is a frequency, so a percentage is legitimate.
        const QString calibrated = cockpitProbabilityText(40, 40, 0.68);
        QVERIFY(calibrated.contains(QStringLiteral("68%")));
        QVERIFY(!calibrated.contains(QStringLiteral("UNCALIBRATED")));
    }

    //! @tstid TS-COCKPIT-004 @design DES-UI-COCKPIT
    // @relation(REQ-F-038, scope=function)
    void TS_COCKPIT_004_aStalePriceIsNeverShownAsALiveOneAndAnAbsentOneIsNotZero()
    {
        constexpr qint64 kStale = 60000;   // one minute
        QCOMPARE(cockpitFreshness(5000, kStale), Freshness::Live);
        QCOMPARE(cockpitFreshness(kStale, kStale), Freshness::Lagging);
        // A negative age means no quote ever arrived — its own state, not "very old".
        QCOMPARE(cockpitFreshness(-1, kStale), Freshness::Absent);

        // The lag is spelled out in minutes: this project measured the rates feed running
        // 6-12 minutes behind, and "372000 ms" tells a reader nothing.
        QCOMPARE(freshnessLabel(Freshness::Lagging, 372000), QStringLiteral("6m old"));
        QCOMPARE(freshnessLabel(Freshness::Live, 1200), QStringLiteral("live"));
        QCOMPARE(freshnessLabel(Freshness::Absent, -1), QStringLiteral("—"));

        // An ABSENT card must not render a number. This is the regression that matters: a
        // last-known price styled as current is the silent lie.
        CockpitCard gone;
        gone.symbol = QStringLiteral("NSDQ100");
        gone.price = 18732.90;      // a plausible value, deliberately present in the struct
        gone.changePct = 0.71;
        gone.freshness = Freshness::Absent;
        gone.ageMs = -1;
        const QVariantMap absentCard = cardToVariant(gone);
        QCOMPARE(absentCard.value(QStringLiteral("price")).toString(), QStringLiteral("—"));
        QCOMPARE(absentCard.value(QStringLiteral("changePct")).toString(),
                 QStringLiteral("—"));
        QCOMPARE(absentCard.value(QStringLiteral("dir")).toInt(), 0);

        // A live card shows its numbers, with the SIGN as a second channel beside colour.
        CockpitCard live = gone;
        live.freshness = Freshness::Live;
        live.ageMs = 900;
        const QVariantMap liveCard = cardToVariant(live);
        QCOMPARE(liveCard.value(QStringLiteral("price")).toString(),
                 QStringLiteral("18732.90"));
        QCOMPARE(liveCard.value(QStringLiteral("changePct")).toString(),
                 QStringLiteral("+0.71%"));
        QCOMPARE(liveCard.value(QStringLiteral("dir")).toInt(), 1);
    }

    //! @tstid TS-COCKPIT-005 @design DES-UI-COCKPIT
    // @relation(REQ-F-038, scope=function)
    void TS_COCKPIT_005_theViewModelPublishesWhatTheQmlBindsToAndClaimsSimulationByDefault()
    {
        CockpitModel model;
        // The safe default: until something says otherwise the view claims SIMULATION, so a
        // mis-wired host cannot silently present a live account as simulated.
        QVERIFY(model.simulation());

        const QSignalSpy spy(&model, &CockpitModel::changed);

        IndexReads reads;
        reads.futuresLead = measured(1, QStringLiteral("Nasdaq +0.16%"));
        reads.futuresMomentum = absent();
        LeadSignal signal;
        signal.dir = 1;
        signal.strength = 62.0;
        signal.grade = LeadGrade::Fair;
        model.setSignal(QStringLiteral("NSDQ100"), signal, reads);

        QCOMPARE(model.instrument(), QStringLiteral("NSDQ100"));
        QCOMPARE(model.ticks().size(), qsizetype(9));
        QVERIFY(model.agreement().contains(QStringLiteral("unmeasurable")));
        QVERIFY(model.evidence().contains(QStringLiteral("evidence 62 of 100")));

        // The detail travels with the tick, so the tooltip shows the NUMBER behind a read
        // rather than only its verdict.
        const QVariantMap first = model.ticks().at(0).toMap();
        QCOMPARE(first.value(QStringLiteral("detail")).toString(),
                 QStringLiteral("Nasdaq +0.16%"));

        model.setCards({CockpitCard{QStringLiteral("SPX500"), 5321.45, 0.54,
                                    Freshness::Live, 800}});
        QCOMPARE(model.cards().size(), qsizetype(1));

        model.setCalibration(3, 40, 0.0);
        QVERIFY(model.probability().contains(QStringLiteral("UNCALIBRATED")));

        model.setSimulation(false);
        QVERIFY(!model.simulation());

        // Every setter notifies, or a bound view would show stale data indefinitely.
        QCOMPARE(spy.count(), 4);
    }

    //! @tstid TS-COCKPIT-006 @design DES-UI-COCKPIT
    // @relation(REQ-F-038, scope=function)
    //
    // The price chart's empty state is STATED, never drawn. A fresh model has no bars, and
    // an axis of 0..0 with no note would render as a flat line at zero — indistinguishable
    // from a market that did not move. This is the same absent-is-not-zero discipline the
    // cards apply to a missing price and the meter applies to an unmeasurable read.
    void TS_COCKPIT_006_anEmptyChartSaysSoRatherThanDrawingAnAxisAtZero()
    {
        CockpitModel model;
        QVERIFY(model.candles().isEmpty());
        QVERIFY(!model.candleNote().isEmpty());     // it SAYS there are no bars

        const QSignalSpy spy(&model, &CockpitModel::changed);
        model.setCandles(trading::candlesFrom({100.0, 101.0}, {102.0, 103.0},
                                              {99.0, 100.5}, {101.0, 100.5}));

        QCOMPARE(spy.count(), 1);
        QCOMPARE(model.candles().size(), qsizetype(2));
        QVERIFY(model.candleNote().isEmpty());      // …and stops saying it once bars arrive

        // The axis is fitted to the wicks and padded outwards, so no extreme sits on the frame.
        QVERIFY(model.candleMin() < 99.0);
        QVERIFY(model.candleMax() > 103.0);

        // Direction crosses as a BOOLEAN decided in C++, not as two numbers for a binding to
        // compare. `up` is what PriceChart.qml switches the hollow/solid body on, and the
        // hollow-vs-solid fill — not the colour — is what a deuteranope actually reads.
        const QVariantMap rising = model.candles().at(0).toMap();
        const QVariantMap falling = model.candles().at(1).toMap();
        QCOMPARE(rising.value(QStringLiteral("up")).toBool(), true);
        QCOMPARE(falling.value(QStringLiteral("up")).toBool(), false);
        QCOMPARE(rising.value(QStringLiteral("high")).toDouble(), 102.0);
        QCOMPARE(falling.value(QStringLiteral("low")).toDouble(), 100.5);

        // Going back to nothing restores the stated empty state rather than leaving the last
        // session on screen — a chart that keeps showing old bars after the feed dies is the
        // stale-price failure in another costume.
        model.setCandles({});
        QVERIFY(model.candles().isEmpty());
        QVERIFY(!model.candleNote().isEmpty());
    }

    //! @tstid TS-COCKPIT-007 @design DES-UI-COCKPIT
    // @relation(REQ-F-038, scope=function)
    //
    // A truncated chart SAYS it is truncated. The model draws only the most recent bars so
    // the hollow/solid body stays wide enough to read, and a view that quietly showed two
    // hours of a six-hour session under the label "1-minute candles" would be making a claim
    // about the session that is not true. The axis must follow the same cut, or the visible
    // bars are squashed into a band by extremes that are off-screen.
    void TS_COCKPIT_007_aTruncatedChartSaysSoAndItsAxisFollowsTheVisibleBars()
    {
        // 200 bars climbing from 100, so the OLDEST are the lowest and would drag an
        // un-cut axis far below anything on screen.
        QList<double> opens;
        QList<double> highs;
        QList<double> lows;
        QList<double> closes;
        for (qint32 i = 0; i < 200; ++i) {
            const double base = 100.0 + i;
            opens.append(base);
            highs.append(base + 2.0);
            lows.append(base - 1.0);
            closes.append(base + 1.0);
        }

        CockpitModel model;
        model.setCandles(trading::candlesFrom(opens, highs, lows, closes));

        // Fewer bars are drawn than were supplied, and both counts are stated.
        QCOMPARE(model.candles().size(), qsizetype(120));
        QVERIFY(model.candleSpan().contains(QStringLiteral("120")));
        QVERIFY(model.candleSpan().contains(QStringLiteral("200")));

        // The axis covers the DRAWN bars. The whole series would reach down to 99; the
        // visible tail starts at bar 80, whose low is 179.
        QVERIFY(model.candleMin() > 150.0);
        QVERIFY(model.candleMax() > 300.0);

        // A series that fits needs no "last N of M" — it says only how many bars there are.
        model.setCandles(trading::candlesFrom({100.0}, {102.0}, {99.0}, {101.0}));
        QVERIFY(!model.candleSpan().isEmpty());
        QVERIFY(!model.candleSpan().contains(QStringLiteral("last")));
    }
};

QTEST_MAIN(TestCockpitModel)
#include "tst_cockpitmodel.moc"
