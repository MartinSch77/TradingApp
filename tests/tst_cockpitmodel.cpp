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
};

QTEST_MAIN(TestCockpitModel)
#include "tst_cockpitmodel.moc"
