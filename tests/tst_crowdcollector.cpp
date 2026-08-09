// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CrowdFixtures.h"

#include "services/CrowdCollector.h"
#include "services/MockCrowdProvider.h"

#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>

using namespace trading::crowd;
using namespace crowdtest;

// The collection loop that makes the crowd subsystem RUN (REQ-F-043, Phase 7). No test here
// touches the network: refreshNow() is never called (the real CFTC provider would fire a real
// request); the loop's logic is driven through ingest() with the deterministic mock's batches,
// exactly the path the providers' observationsReady signal feeds.
namespace {

// A collector over a fresh store, with every optional credential and model deterministically
// ABSENT — neither a real key in the environment nor a model installed in this machine's app
// config dir may leak into "the unconfigured provider is unavailable". The model paths are
// pointed at nowhere EXPLICITLY, because unsetting them would fall back to the config dir.
struct CleanCollector {
    explicit CleanCollector(const QString &storePath)
    {
        qunsetenv("TRADINGAPP_FRED_API_KEY");
        qputenv("TRADINGAPP_CROWD_MODEL", "/nonexistent/crowd-model.onnx");
        qputenv("TRADINGAPP_FINBERT_DIR", "/nonexistent/finbert");
        collector = std::make_unique<CrowdCollector>(Config{}, storePath);
    }
    std::unique_ptr<CrowdCollector> collector;
};

QDateTime dayStamp(int dayOffset)
{
    return {QDate(2026, 8, 3).addDays(dayOffset), QTime(18, 0), QTimeZone::UTC};
}

} // namespace

class TestCrowdCollector : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-DASH-001 @design DES-SVC-CROWDCOLLECT
    // @relation(REQ-F-043, scope=function)
    void TS_DASH_001_ingestStoresIdempotentlyAndRecomputesThePersistedScore()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CleanCollector clean(dir.filePath(QStringLiteral("crowd.db")));
        CrowdCollector &collector = *clean.collector;

        QList<CrowdScoreResult> results;
        static_cast<void>(connect(&collector, &CrowdCollector::scoreUpdated, this,
                                  [&results](const QString &, const CrowdScoreResult &result) {
                                      results.append(result);
                                  }));

        // A week of the deterministic mock, ingested the way observationsReady would feed it.
        MockCrowdProvider mock;
        for (int day = 0; day < 7; ++day) {
            collector.ingest(mock.fetch(QStringLiteral("SPX500"), dayStamp(day)).observations);
        }
        QVERIFY(collector.store().count() > 0);
        QVERIFY(!results.isEmpty());

        // IDEMPOTENT: re-ingesting the same day's fetch stores NOTHING new — a poll loop can
        // never grow the record by repetition.
        const qint64 before = collector.store().count();
        collector.ingest(mock.fetch(QStringLiteral("SPX500"), dayStamp(6)).observations);
        QCOMPARE(collector.store().count(), before);

        // The score is computed from the accumulated history (past-only z needs several days)
        // and PERSISTED — the dashboard shows it again after a restart.
        QVERIFY(results.last().coverage > 0.0);
        QVERIFY(!collector.store().latestScore(QStringLiteral("SPX500")).isEmpty());
    }

    //! @tstid TS-DASH-002 @design DES-SVC-CROWDCOLLECT
    // @relation(REQ-F-043, scope=function)
    void TS_DASH_002_modelFeaturesMatchTheTrainersNamesAndKeepMissingMissing()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CleanCollector clean(dir.filePath(QStringLiteral("crowd.db")));
        CrowdCollector &collector = *clean.collector;
        MockCrowdProvider mock;
        for (int day = 0; day < 7; ++day) {
            collector.ingest(mock.fetch(QStringLiteral("SPX500"), dayStamp(day)).observations);
        }

        const QDateTime now = dayStamp(7);
        const QHash<QString, double> features =
            collector.modelFeaturesFor(QStringLiteral("SPX500"), now);

        // A series the mock DOES feed: measured, with its value and age.
        QCOMPARE(features.value(QStringLiteral("vix_measured")), 1.0);
        QVERIFY(features.contains(QStringLiteral("vix_value")));
        QVERIFY(features.contains(QStringLiteral("vix_age_days")));

        // A series NOTHING has fed (the mock has no leveraged-fund leg): only the 0 marker —
        // no invented value, no invented z, exactly the missing-stays-missing contract the
        // model's embedded medians exist for.
        QCOMPARE(features.value(QStringLiteral("cot_lev_fund_net_measured")), 0.0);
        QVERIFY(!features.contains(QStringLiteral("cot_lev_fund_net_value")));

        // The price-context features are DELIBERATELY absent (imputed and counted by the
        // model itself, never recomputed here where they could drift from the trainer).
        QVERIFY(!features.contains(QStringLiteral("ret_1d_pct")));
        QVERIFY(!features.contains(QStringLiteral("vol_20d_pct")));

        // Every name the collector emits is one the trainer's manifest declares — the
        // BY-NAME contract with the exported model, pinned against a pipeline-built manifest.
        const QString prices = dir.filePath(QStringLiteral("prices.csv"));
        QList<double> closes;
        for (int i = 0; i < 10; ++i) {
            closes.append(100.0 + i);
        }
        QVERIFY(writePricesCsv(prices, QDate(2026, 8, 3), closes));
        const QString dataset = dir.filePath(QStringLiteral("dataset.csv"));
        const QString manifest = dir.filePath(QStringLiteral("manifest.json"));
        const ToolRun run = runPython(
            QStringLiteral("python3"),
            buildToolArgs(dir.filePath(QStringLiteral("crowd.db")), prices, dataset, manifest),
            60000);
        if (!run.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));
        const QJsonObject manifestJson = QJsonDocument::fromJson(fileBytes(manifest)).object();
        const QJsonArray manifestFeatures =
            manifestJson.value(QStringLiteral("features")).toArray();
        for (auto it = features.constBegin(); it != features.constEnd(); ++it) {
            QVERIFY2(manifestFeatures.contains(QJsonValue(it.key())), qPrintable(it.key()));
        }
    }

    //! @tstid TS-DASH-003 @design DES-SVC-CROWDCOLLECT
    // @relation(REQ-F-043, scope=function)
    void TS_DASH_003_statusesAreHonestWordsAndAnIssueIsNamedNeverACrash()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CleanCollector clean(dir.filePath(QStringLiteral("crowd.db")));
        CrowdCollector &collector = *clean.collector;

        // Three network providers plus the local FinBERT scorer; only the keyless CFTC one is
        // configured on a machine with no credentials and no model — everything else is
        // honestly "not configured", not an error.
        const QList<CollectorProviderStatus> statuses = collector.providerStatuses();
        QCOMPARE(statuses.size(), 4);
        int configured = 0;
        for (const CollectorProviderStatus &status : statuses) {
            QVERIFY(!status.name.isEmpty());
            QVERIFY(!status.detail.isEmpty());
            configured += status.configured ? 1 : 0;
            if (status.name == QStringLiteral("CFTC-COT")) {
                QVERIFY(status.configured);
            }
        }
        QCOMPARE(configured, 1);

        // A provider's failure becomes a NAMED status beside the others.
        const QSignalSpy statusSpy(&collector, &CrowdCollector::statusChanged);
        collector.noteProviderIssue(QStringLiteral("FRED"),
                                    QStringLiteral("HTTP 500 from the observations endpoint"));
        QCOMPARE(statusSpy.count(), 1);
        bool found = false;
        const QList<CollectorProviderStatus> after = collector.providerStatuses();
        for (const CollectorProviderStatus &status : after) {
            found = found
                    || (status.name == QStringLiteral("FRED")
                        && status.detail.contains(QStringLiteral("500")));
        }
        QVERIFY(found);

        // No model file anywhere: not ready, and the status SAYS why (either "no model
        // loaded" on a runtime build or the missing-runtime remedy on a stub build) — a
        // visibly absent capability, never a silent one.
        QVERIFY(!collector.model().ready());
        QVERIFY(!collector.modelStatus().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCrowdCollector)
#include "tst_crowdcollector.moc"
