// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/CrowdStore.h"

#include "services/MockCrowdProvider.h"

#include <QtTest/QtTest>

#include <algorithm>

using namespace trading::crowd;

// The SQLite store keeps the RAW observation layer. Its guarantees — idempotent dedup, UTC
// round-trip, received-time ordering, and refusing to store a missing datum as a zero — are what
// later features and labels rest on, so they are pinned against an in-memory database.
class TestCrowdStore : public QObject
{
    Q_OBJECT;

private slots:
    //! @tstid TS-CROWD-004 @design DES-SVC-CROWDDATA
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_004_roundTripDedupAndQuery()
    {
        CrowdStore store(QStringLiteral(":memory:"));
        QVERIFY2(store.isOpen(), qPrintable(store.lastError()));
        QCOMPARE(store.count(), qint64(0));

        MockCrowdProvider provider;
        const QDateTime now(QDate(2026, 8, 9), QTime(18, 0), QTimeZone::UTC);
        const ProviderResult scan = provider.fetch(QStringLiteral("SPX500"), now);
        QVERIFY(!scan.observations.isEmpty());
        const auto rows = static_cast<qint32>(scan.observations.size());

        // First upsert writes every valid observation.
        QCOMPARE(store.upsert(scan.observations), rows);
        QCOMPARE(store.count(), static_cast<qint64>(rows));

        // IDEMPOTENT: re-fetching the same day and re-upserting writes NOTHING new (dedup key),
        // so a poll loop does not grow the table without bound.
        QCOMPARE(store.upsert(provider.fetch(QStringLiteral("SPX500"), now).observations), 0);
        QCOMPARE(store.count(), static_cast<qint64>(rows));

        // Round-trip: value and UTC times survive storage exactly.
        const auto srcVix = std::find_if(scan.observations.cbegin(), scan.observations.cend(),
                                         [](const Observation &o) {
                                             return o.seriesId == QStringLiteral("VIX");
                                         });
        QVERIFY(srcVix != scan.observations.cend());
        const Observation vix = store.latest(QStringLiteral("SPX500"), Source::Volatility,
                                             QStringLiteral("VIX"));
        QVERIFY(vix.valid);
        QCOMPARE(vix.instrument, QStringLiteral("SPX500"));
        QVERIFY(qFuzzyCompare(vix.value, srcVix->value));
        QCOMPARE(vix.receivedTime.toUTC(), now.toUTC());

        // An invalid observation is SKIPPED, never written as a zero row.
        Observation missing;
        missing.instrument = QStringLiteral("SPX500");
        missing.source = Source::Macro;
        missing.seriesId = QStringLiteral("PLACEHOLDER");
        missing.eventTime = now;
        missing.receivedTime = now;
        missing.valid = false;
        QCOMPARE(store.upsert(missing), 0);
        QCOMPARE(store.count(), static_cast<qint64>(rows));

        // Query by received-since returns newest-received first…
        const QList<Observation> all = store.observationsReceivedSince(QStringLiteral("SPX500"),
                                                                       QDateTime());
        QCOMPARE(all.size(), static_cast<qsizetype>(rows));
        for (qsizetype i = 1; i < all.size(); ++i) {
            QVERIFY(all.at(i - 1).receivedTime >= all.at(i).receivedTime);
        }
        // …respects the lower bound…
        QVERIFY(store.observationsReceivedSince(QStringLiteral("SPX500"), now.addDays(1)).isEmpty());
        // …and never leaks one instrument's rows into another's query.
        QVERIFY(store.observationsReceivedSince(QStringLiteral("NSDQ100"), QDateTime()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCrowdStore)
#include "tst_crowdstore.moc"
