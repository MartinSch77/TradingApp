// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/MockCrowdProvider.h"

#include <QtTest/QtTest>

#include <algorithm>

using namespace trading::crowd;

// The offline mock is what runs the whole subsystem without a paid API, so it has to be
// reproducible (a test can assert its output), span the families the score will combine, and
// model the CFTC publication lag — plus behave as a recoverable "unavailable" when switched off.
class TestMockCrowdProvider : public QObject
{
    Q_OBJECT;

private slots:
    //! @tstid TS-CROWD-003 @design DES-SVC-CROWDDATA
    // @relation(REQ-F-039, scope=function)
    void TS_CROWD_003_reproducibleSpansFamiliesModelsLag()
    {
        MockCrowdProvider provider;
        QVERIFY(provider.isConfigured());
        const QDateTime now(QDate(2026, 8, 9), QTime(18, 0), QTimeZone::UTC);

        const ProviderResult first = provider.fetch(QStringLiteral("SPX500"), now);
        QVERIFY(first.available);
        QVERIFY(!first.observations.isEmpty());

        // REPRODUCIBLE: same instrument + same UTC day yields identical values (a deterministic
        // seed, not Qt's per-process-randomised qHash).
        const ProviderResult again = provider.fetch(QStringLiteral("SPX500"), now);
        QCOMPARE(first.observations.size(), again.observations.size());
        for (qsizetype i = 0; i < first.observations.size(); ++i) {
            QCOMPARE(first.observations.at(i).value, again.observations.at(i).value);
            QCOMPARE(first.observations.at(i).seriesId, again.observations.at(i).seriesId);
        }
        // A different instrument draws differently.
        const ProviderResult nsdq = provider.fetch(QStringLiteral("NSDQ100"), now);
        QVERIFY(nsdq.observations.constFirst().value != first.observations.constFirst().value);

        // Spans the families the Crowd Score will combine, and every datum is valid + carries
        // both timestamps.
        QSet<Source> families;
        for (const Observation &obs : first.observations) {
            QVERIFY(obs.valid);
            QVERIFY(obs.eventTime.isValid());
            QVERIFY(obs.receivedTime.isValid());
            families.insert(obs.source);
        }
        QVERIFY(families.contains(Source::Volatility));
        QVERIFY(families.contains(Source::RetailPositioning));
        QVERIFY(families.contains(Source::Options));
        QVERIFY(families.contains(Source::InstitutionalPositioning));

        // The COT datum models the publication LAG: received several days AFTER the event.
        const auto cot = std::find_if(
            first.observations.cbegin(), first.observations.cend(),
            [](const Observation &o) { return o.source == Source::InstitutionalPositioning; });
        QVERIFY(cot != first.observations.cend());
        QVERIFY(cot->receivedTime > cot->eventTime);
        QVERIFY(cot->eventTime.daysTo(cot->receivedTime) >= 2);

        // The no-credentials path is recoverable: unavailable, empty, with a reason — not a crash
        // and not empty-but-available (which would read as "market is silent").
        MockCrowdProvider off(false);
        QVERIFY(!off.isConfigured());
        const ProviderResult none = off.fetch(QStringLiteral("SPX500"), now);
        QVERIFY(!none.available);
        QVERIFY(none.observations.isEmpty());
        QVERIFY(!none.note.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestMockCrowdProvider)
#include "tst_mockcrowdprovider.moc"
