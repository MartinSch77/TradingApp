// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/CrowdObservation.h"

#include <QtTest/QtTest>

using namespace trading::crowd;

// The normalized observation is the unit everything in the crowd subsystem is built on, so its
// identity (dedup key), its two-timestamp leakage model and its quality/freshness metadata are
// pinned here — pure domain, no store, no network.
class TestCrowdObservation : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-CROWD-001 @design DES-DOM-CROWDOBS
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_001_identityTimingAndQuality()
    {
        Observation obs;
        obs.instrument = QStringLiteral("SPX500");
        obs.source = Source::InstitutionalPositioning;
        obs.sourceName = QStringLiteral("CFTC-COT");
        obs.seriesId = QStringLiteral("ES-ASSET-MGR-NET");
        obs.eventTime = QDateTime(QDate(2026, 8, 4), QTime(20, 0), QTimeZone::UTC);    // Tuesday
        obs.receivedTime = QDateTime(QDate(2026, 8, 7), QTime(20, 30), QTimeZone::UTC);  // Friday
        obs.value = 12345.0;
        obs.unit = QStringLiteral("contracts");
        obs.valid = true;

        // The dedup identity is UTC-NORMALISED: the same instant expressed in another zone is the
        // same datum. So a re-fetch that reports the event in +01:00 must not create a duplicate.
        Observation shiftedZone = obs;
        shiftedZone.eventTime = obs.eventTime.toOffsetFromUtc(3600);
        QCOMPARE(shiftedZone.dedupKey(), obs.dedupKey());
        Observation laterWeek = obs;
        laterWeek.eventTime = obs.eventTime.addDays(7);
        QVERIFY(laterWeek.dedupKey() != obs.dedupKey());

        // Publication LAG is representable and true: the datum became known AFTER it was about —
        // the fact every leakage-safe consumer must reason in.
        QVERIFY(obs.receivedTime > obs.eventTime);

        // Age is measured from receivedTime; freshness from that against a threshold.
        const QDateTime now = obs.receivedTime.addSecs(3600);
        QCOMPARE(obs.ageSeconds(now), qint64(3600));
        QCOMPARE(obs.freshness(now, 7200), Freshness::Live);    // within two hours
        QCOMPARE(obs.freshness(now, 1800), Freshness::Stale);   // beyond thirty minutes
        // A future received time (small clock skew) is Live, not "stale".
        QCOMPARE(obs.freshness(obs.receivedTime.addSecs(-60), 100), Freshness::Live);

        // An invalid observation is Absent, and reports age -1 rather than a fabricated 0 —
        // absent is not zero.
        const Observation missing;
        QCOMPARE(missing.freshness(now, 7200), Freshness::Absent);
        QCOMPARE(missing.ageSeconds(now), qint64(-1));

        // Quality is a composable bitfield, Ok by default.
        QVERIFY(obs.hasQuality(Quality::Ok));
        obs.setQuality(Quality::LateArrival);
        obs.setQuality(Quality::Estimated);
        QVERIFY(!obs.hasQuality(Quality::Ok));
        QVERIFY(obs.hasQuality(Quality::LateArrival));
        QVERIFY(obs.hasQuality(Quality::Estimated));
        QVERIFY(!obs.hasQuality(Quality::Interpolated));
    }

    //! @tstid TS-CROWD-002 @design DES-DOM-CROWDOBS
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_002_enumStringsRoundTrip()
    {
        // The store persists the NAME, not the ordinal, so the mapping must be a stable bijection
        // over every family — otherwise re-ordering the enum silently repoints historical rows.
        for (const Source src : {Source::RetailPositioning, Source::Options, Source::Volatility,
                                 Source::InstitutionalPositioning, Source::Social, Source::Macro,
                                 Source::Market}) {
            QCOMPARE(sourceFromString(sourceToString(src)), src);
        }
        // An unknown name falls back to Market rather than throwing or corrupting.
        QCOMPARE(sourceFromString(QStringLiteral("nonsense")), Source::Market);
        QCOMPARE(freshnessWord(Freshness::Live), QStringLiteral("live"));
        QCOMPARE(freshnessWord(Freshness::Stale), QStringLiteral("stale"));
        QCOMPARE(freshnessWord(Freshness::Absent), QStringLiteral("absent"));
    }
};

QTEST_APPLESS_MAIN(TestCrowdObservation)
#include "tst_crowdobservation.moc"
