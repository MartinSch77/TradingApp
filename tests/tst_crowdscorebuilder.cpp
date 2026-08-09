// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/CrowdScoreBuilder.h"

#include "services/CrowdStore.h"
#include "services/MockCrowdProvider.h"

#include <QtTest/QtTest>

using namespace trading::crowd;

// The builder connects the raw store to the pure score (past-only z-scores) and the store's
// score layer persists the result. Both are pinned against an in-memory database seeded with
// several days of the deterministic mock, so each series has real prior history to normalize.
class TestCrowdScoreBuilder : public QObject
{
    Q_OBJECT;

private slots:
    //! @tstid TS-CROWD-007 @design DES-SVC-CROWDSCORE
    // @relation(REQ-F-040, scope=function)
    void TS_CROWD_007_buildFromStoreThenPersist()
    {
        CrowdStore store(QStringLiteral(":memory:"));
        QVERIFY2(store.isOpen(), qPrintable(store.lastError()));
        MockCrowdProvider provider;

        // Seed ten prior days plus today, so the daily series have prior history to z-score
        // against. (The weekly COT series dedups within a week, so it may stay uncalibrated —
        // that is honest, and the score simply reports reduced coverage.)
        const QDateTime now(QDate(2026, 8, 20), QTime(18, 0), QTimeZone::UTC);
        for (int day = 10; day >= 0; --day) {
            store.upsert(provider.fetch(QStringLiteral("SPX500"), now.addDays(-day)).observations);
        }

        const CrowdScoreConfig cfg;
        const CrowdScoreResult result = buildCrowdScore(store, QStringLiteral("SPX500"), now, cfg);
        QVERIFY(!result.isEmpty());
        QVERIFY(result.coverage > 0.0);
        QCOMPARE(result.components.size(), 4);
        QVERIFY(!result.direction.isEmpty());

        // Persist to the SEPARATE score layer; idempotent on (instrument, computed_at).
        QCOMPARE(store.scoreCount(), qint64(0));
        QVERIFY(store.saveScore(QStringLiteral("SPX500"), result, now));
        QCOMPARE(store.scoreCount(), qint64(1));
        QVERIFY(!store.saveScore(QStringLiteral("SPX500"), result, now));
        QCOMPARE(store.scoreCount(), qint64(1));

        // Read the latest back; the headline numbers and the component snapshot (input
        // references) survive the round trip.
        const CrowdScoreResult loaded = store.latestScore(QStringLiteral("SPX500"));
        QVERIFY(!loaded.isEmpty());
        QVERIFY(qFuzzyCompare(1.0 + loaded.score, 1.0 + result.score));
        QCOMPARE(loaded.direction, result.direction);
        QCOMPARE(loaded.version, result.version);
        QCOMPARE(loaded.components.size(), 4);
    }
};

QTEST_GUILESS_MAIN(TestCrowdScoreBuilder)
#include "tst_crowdscorebuilder.moc"
