// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CrowdFixtures.h"

#include "services/CrowdStore.h"

#include <QtTest/QtTest>

#include <numeric>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

using namespace trading::crowd;

// The OFFLINE Phase 4 training pipeline (REQ-F-041, tools/ml/). These tests pin the CONTRACT
// between the two languages the same way TS-NET-004 does for the bot net: the C++ CrowdStore
// writes a real SQLite store, the Python tools read it, and the rules that make the dataset
// honest — the received-time as-of join, missing-stays-missing, cost-aware labels, the purged
// walk-forward split, and the trainer's refusals — are asserted on the files that come back.
// Only the dataset/split half (stdlib-only by design) is required everywhere; the model-fitting
// half needs the optional `./setup.sh ml` environment and its test SKIPS without it.
using namespace crowdtest;

namespace {
// A synthetic 30-day close series (flat 100 -> up to 104 for days 10-19 -> back to 100),
// written as a prices CSV, with an empty CrowdStore created and closed at `storePath` first —
// labels are a price-only fact, so both dataset-labelling tests below start from the same
// empty store. Returns the prices CSV path, or an empty string if either write failed.
QString writeStepCloseFixture(const QTemporaryDir &dir, const QString &storePath)
{
    const CrowdStore store(storePath);
    if (!store.isOpen()) {
        return {};
    }
    const QDate start(2026, 6, 1);
    QList<double> closes;
    for (int i = 0; i < 30; ++i) {
        closes.append(i < 10 ? 100.0 : (i < 20 ? 104.0 : 100.0));
    }
    const QString prices = dir.filePath(QStringLiteral("prices.csv"));
    return writePricesCsv(prices, start, closes) ? prices : QString();
}
} // namespace

class TestCrowdMl : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-ML-001 @design DES-ML-TRAIN
    // @relation(REQ-F-041, scope=function)
    void TS_ML_001_theDatasetJoinsAsOfReceivedTimeAndKeepsMissingMissing()
    {
        // The one join that keeps look-ahead out: a COT datum ABOUT Tuesday, RELEASED Friday,
        // must be absent from Wednesday's row and present from Friday's — against the real
        // store the C++ side writes, so the schema contract between the languages is pinned.
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.filePath(QStringLiteral("crowd.db"));
        const QDate start(2026, 6, 1); // a Monday
        {
            CrowdStore store(storePath);
            QVERIFY2(store.isOpen(), qPrintable(store.lastError()));
            // VIX daily: 10, 20, then 30 — so the third day's z against its PRIOR history
            // [10, 20] is exactly (30 - 15) / 5 = 3.
            const QList<double> vix{10.0, 20.0, 30.0, 21.0, 22.0, 20.0, 21.0, 22.0, 20.0, 21.0,
                                    22.0, 20.0, 21.0, 22.0, 20.0, 21.0, 22.0, 20.0, 21.0, 22.0};
            for (qsizetype i = 0; i < vix.size(); ++i) {
                const QDateTime stamp(start.addDays(i), QTime(20, 5), QTimeZone::UTC);
                QCOMPARE(store.upsert(makeObservation(Source::Volatility, QStringLiteral("VIX"),
                                                      stamp, stamp, vix.at(i))),
                         1);
            }
            // COT: event = Tuesday 2026-06-09 20:00, received = Friday 2026-06-12 19:30.
            QCOMPARE(store.upsert(makeObservation(
                         Source::InstitutionalPositioning,
                         QStringLiteral("COT-ASSET-MGR-NET"),
                         QDateTime(QDate(2026, 6, 9), QTime(20, 0), QTimeZone::UTC),
                         QDateTime(QDate(2026, 6, 12), QTime(19, 30), QTimeZone::UTC),
                         12345.0)),
                     1);
        } // the store's connection closes before the tool reads the file

        QList<double> closes;
        for (int i = 0; i < 20; ++i) {
            closes.append(100.0 + i);
        }
        const QString prices = dir.filePath(QStringLiteral("prices.csv"));
        QVERIFY(writePricesCsv(prices, start, closes));

        const QStringList zHistory{QStringLiteral("--z-min-history"), QStringLiteral("2")};
        const QString dataset = dir.filePath(QStringLiteral("dataset.csv"));
        const QString manifest = dir.filePath(QStringLiteral("manifest.json"));
        ToolRun run = runPython(QStringLiteral("python3"),
                                buildToolArgs(storePath, prices, dataset, manifest, zHistory),
                                60000);
        if (!run.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));

        const QList<QHash<QString, QString>> rows = readCsv(dataset);
        QCOMPARE(rows.size(), 15); // 20 prices minus the 5-day horizon

        // NO LOOK-AHEAD: Wednesday after the report's Tuesday — released only on Friday.
        const auto *wednesday = rowForDay(rows, QStringLiteral("2026-06-10"));
        QVERIFY(wednesday != nullptr);
        QCOMPARE(wednesday->value(QStringLiteral("cot_asset_mgr_net_measured")),
                 QStringLiteral("0"));
        QVERIFY(wednesday->value(QStringLiteral("cot_asset_mgr_net_value")).isEmpty());

        // From its release on, the datum is known — with its age measured from RECEIVED time
        // (21:00 decision minus 19:30 release = 0.0625 days).
        const auto *friday = rowForDay(rows, QStringLiteral("2026-06-12"));
        QVERIFY(friday != nullptr);
        QCOMPARE(friday->value(QStringLiteral("cot_asset_mgr_net_measured")),
                 QStringLiteral("1"));
        QCOMPARE(friday->value(QStringLiteral("cot_asset_mgr_net_value")),
                 QStringLiteral("12345"));
        QVERIFY(qAbs(friday->value(QStringLiteral("cot_asset_mgr_net_age_days")).toDouble()
                     - 0.0625) < 1e-9);

        // The z is PAST-ONLY: day three's 30 against [10, 20] is exactly 3.
        const auto *day3 = rowForDay(rows, QStringLiteral("2026-06-03"));
        QVERIFY(day3 != nullptr);
        QCOMPARE(day3->value(QStringLiteral("vix_value")), QStringLiteral("30"));
        QCOMPARE(day3->value(QStringLiteral("vix_z")), QStringLiteral("3"));

        // MISSING STAYS MISSING: families never fetched are empty cells beside a 0 marker on
        // every row — never a zero value.
        for (const auto &row : rows) {
            QCOMPARE(row.value(QStringLiteral("retail_pct_long_measured")), QStringLiteral("0"));
            QVERIFY(row.value(QStringLiteral("retail_pct_long_value")).isEmpty());
            QVERIFY(row.value(QStringLiteral("put_call_z")).isEmpty());
        }

        // The manifest names the columns (append-only contract) and its version.
        const QJsonObject m = QJsonDocument::fromJson(fileBytes(manifest)).object();
        QCOMPARE(m.value(QStringLiteral("version")).toInt(), 1);
        const QJsonArray features = m.value(QStringLiteral("features")).toArray();
        QVERIFY(features.contains(QJsonValue(QStringLiteral("vix_z"))));
        QVERIFY(features.contains(QJsonValue(QStringLiteral("cot_asset_mgr_net_measured"))));
        QCOMPARE(m.value(QStringLiteral("label_classes")).toArray().size(), 3);

        // DETERMINISTIC: the same inputs produce byte-identical files (no wall-clock anywhere).
        const QString dataset2 = dir.filePath(QStringLiteral("dataset2.csv"));
        const QString manifest2 = dir.filePath(QStringLiteral("manifest2.json"));
        run = runPython(QStringLiteral("python3"),
                        buildToolArgs(storePath, prices, dataset2, manifest2, zHistory),
                        60000);
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));
        QCOMPARE(fileBytes(dataset2), fileBytes(dataset));
        QCOMPARE(fileBytes(manifest2), fileBytes(manifest));
    }

    //! @tstid TS-ML-002 @design DES-ML-TRAIN
    // @relation(REQ-F-041, scope=function)
    void TS_ML_002_labelsComeFromForwardReturnsWithACostDeadZone()
    {
        // A constructed price path with known forward moves: +4% is LONG, flat is NO_TRADE
        // (an outcome, not a failure), -3.8% is SHORT — and the rows whose horizon runs past
        // the history are DROPPED, never labelled by invention.
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.filePath(QStringLiteral("crowd.db"));
        const QString prices = writeStepCloseFixture(dir, storePath);
        QVERIFY2(!prices.isEmpty(), "failed to write the step-close fixture");

        const QString dataset = dir.filePath(QStringLiteral("dataset.csv"));
        const ToolRun run = runPython(
            QStringLiteral("python3"),
            buildToolArgs(storePath, prices, dataset, dir.filePath(QStringLiteral("m.json")),
                          {QStringLiteral("--dead-zone-pct"), QStringLiteral("0.25")}),
            60000);
        if (!run.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));

        const QList<QHash<QString, QString>> rows = readCsv(dataset);
        QCOMPARE(rows.size(), 25); // the last 5 have no resolvable forward return
        QVERIFY(rowForDay(rows, QStringLiteral("2026-06-26")) == nullptr);

        const auto *longRow = rowForDay(rows, QStringLiteral("2026-06-06")); // 100 -> 104
        QVERIFY(longRow != nullptr);
        QCOMPARE(longRow->value(QStringLiteral("label")), QStringLiteral("LONG"));
        QCOMPARE(longRow->value(QStringLiteral("forward_return_pct")), QStringLiteral("4"));

        const auto *flatRow = rowForDay(rows, QStringLiteral("2026-06-13")); // 104 -> 104
        QVERIFY(flatRow != nullptr);
        QCOMPARE(flatRow->value(QStringLiteral("label")), QStringLiteral("NO_TRADE"));

        const auto *shortRow = rowForDay(rows, QStringLiteral("2026-06-16")); // 104 -> 100
        QVERIFY(shortRow != nullptr);
        QCOMPARE(shortRow->value(QStringLiteral("label")), QStringLiteral("SHORT"));

        // An empty store invents nothing: every crowd family stays an absence.
        QCOMPARE(longRow->value(QStringLiteral("vix_measured")), QStringLiteral("0"));
        QVERIFY(longRow->value(QStringLiteral("vix_value")).isEmpty());
    }

    //! @tstid TS-ML-003 @design DES-ML-TRAIN
    // @relation(REQ-F-041, scope=function)
    void TS_ML_003_walkForwardFoldsPurgeTheLabelOverlapAndTheEmbargo()
    {
        // Daily rows, 5-day labels, 2-day embargo: a row closer than 7 days to a validation
        // block resolves its label INSIDE that block, so it must be purged from training —
        // train never reaches past (val start - horizon - embargo).
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.filePath(QStringLiteral("crowd.db"));
        {
            const CrowdStore store(storePath);
            QVERIFY2(store.isOpen(), qPrintable(store.lastError()));
        }
        const QDate start(2026, 6, 1);
        QList<double> closes;
        for (int i = 0; i < 60; ++i) {
            closes.append(100.0 + (i % 7));
        }
        const QString prices = dir.filePath(QStringLiteral("prices.csv"));
        QVERIFY(writePricesCsv(prices, start, closes));
        const QString dataset = dir.filePath(QStringLiteral("dataset.csv"));
        ToolRun run = runPython(
            QStringLiteral("python3"),
            buildToolArgs(storePath, prices, dataset, dir.filePath(QStringLiteral("m.json"))),
            60000);
        if (!run.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));

        const QString splitsPath = dir.filePath(QStringLiteral("splits.json"));
        run = runPython(QStringLiteral("python3"),
                        {datasetTool(), QStringLiteral("splits"), QStringLiteral("--dataset"),
                         dataset, QStringLiteral("--folds"), QStringLiteral("3"),
                         QStringLiteral("--embargo-days"), QStringLiteral("2"),
                         QStringLiteral("--out"), splitsPath},
                        60000);
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));

        const QJsonObject splits = QJsonDocument::fromJson(fileBytes(splitsPath)).object();
        QCOMPARE(splits.value(QStringLiteral("row_count")).toInt(), 55);
        const QJsonArray folds = splits.value(QStringLiteral("folds")).toArray();
        QCOMPARE(folds.size(), 3);

        int previousValEnd = 27 - 1; // the first block starts at min-train-fraction (0.5)
        for (const auto &foldValue : folds) {
            const QJsonObject fold = foldValue.toObject();
            const QJsonArray train = fold.value(QStringLiteral("train_rows")).toArray();
            const QJsonArray val = fold.value(QStringLiteral("val_rows")).toArray();
            QVERIFY(!train.isEmpty());
            QVERIFY(!val.isEmpty());
            const int valStart = val.first().toInt();
            QCOMPARE(valStart, previousValEnd + 1); // contiguous, ordered, non-overlapping
            previousValEnd = val.last().toInt();
            const int maxTrain = std::accumulate(
                train.begin(), train.end(), -1,
                [](int acc, const QJsonValue &v) { return qMax(acc, v.toInt()); });
            // Purged: horizon 5 + embargo 2 rows fall between the last usable training row
            // and the block — on contiguous daily rows exactly 7 of them, every fold.
            QCOMPARE(maxTrain, valStart - 8);
            QCOMPARE(fold.value(QStringLiteral("purged")).toInt(), 7);
        }
        QCOMPARE(previousValEnd, 54); // the blocks cover the tail completely
    }

    //! @tstid TS-ML-004 @design DES-ML-TRAIN
    // @relation(REQ-F-041, scope=function)
    void TS_ML_004_theTrainerRefusesWithTheSkippedCodeRatherThanPretend()
    {
        // The refusals run BEFORE the optional imports, so they hold on machines that never
        // installed the ML stack: too small exits 3 and writes NOTHING; one class likewise.
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.filePath(QStringLiteral("crowd.db"));
        const QString prices = writeStepCloseFixture(dir, storePath);
        QVERIFY2(!prices.isEmpty(), "failed to write the step-close fixture");
        const QString dataset = dir.filePath(QStringLiteral("dataset.csv"));
        const QString manifest = dir.filePath(QStringLiteral("manifest.json"));
        ToolRun run = runPython(QStringLiteral("python3"),
                                buildToolArgs(storePath, prices, dataset, manifest), 60000);
        if (!run.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));

        // 25 rows against a 200-row floor: skipped, and the out directory never appears.
        const QString outSmall = dir.filePath(QStringLiteral("out-small"));
        run = runPython(QStringLiteral("python3"), trainerArgs(dataset, manifest, outSmall),
                        60000);
        QCOMPARE(run.exitCode, 3); // the project's "skipped" code
        QVERIFY(run.stdErr.contains(QStringLiteral("skipped")));
        QVERIFY(!QDir(outSmall).exists());

        // A dead zone nothing clears labels every row NO_TRADE: one class separates nothing.
        const QString flatDataset = dir.filePath(QStringLiteral("flat.csv"));
        const QString flatManifest = dir.filePath(QStringLiteral("flat.json"));
        run = runPython(
            QStringLiteral("python3"),
            buildToolArgs(storePath, prices, flatDataset, flatManifest,
                          {QStringLiteral("--dead-zone-pct"), QStringLiteral("50")}),
            60000);
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));
        const QString outFlat = dir.filePath(QStringLiteral("out-flat"));
        run = runPython(QStringLiteral("python3"),
                        trainerArgs(flatDataset, flatManifest, outFlat,
                                    {QStringLiteral("--min-samples"), QStringLiteral("20")}),
                        60000);
        QCOMPARE(run.exitCode, 3);
        QVERIFY(run.stdErr.contains(QStringLiteral("NO_TRADE")));
        QVERIFY(!QDir(outFlat).exists());
    }

    //! @tstid TS-ML-005 @design DES-ML-TRAIN
    // @relation(REQ-F-041, scope=function)
    void TS_ML_005_theFullPipelineTrainsEvaluatesAgainstBaselinesAndExportsOnnx()
    {
        // End to end on a LEARNABLE record (price direction follows the volatility regime):
        // walk-forward numbers beside the named baselines, and two ONNX exports that passed
        // the parity check. Needs the optional environment; skips without it, like every
        // licence-bound stage.
        const QString python = mlPython();
        if (python.isEmpty()) {
            QSKIP("the optional ML environment is not provisioned (./setup.sh ml)");
        }

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString outDir;
        QString dataset;
        QString manifest;
        const ToolRun run = runRegimeTrainingPipeline(dir, python, outDir, dataset, manifest);
        if (!run.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));

        // Both exports exist — they are only written after BOTH passed the parity check.
        QVERIFY(QFileInfo(outDir + QStringLiteral("/crowd-logreg.onnx")).size() > 100);
        QVERIFY(QFileInfo(outDir + QStringLiteral("/crowd-xgb.onnx")).size() > 100);

        const QJsonObject report = QJsonDocument::fromJson(
            fileBytes(outDir + QStringLiteral("/training-report.json"))).object();
        QVERIFY(report.value(QStringLiteral("evaluated_folds")).toInt() >= 1);
        const QJsonObject means =
            report.value(QStringLiteral("means_over_evaluated_folds")).toObject();
        // The models AND the named baselines, on identical rows.
        QVERIFY(means.contains(QStringLiteral("logistic_regression")));
        QVERIFY(means.contains(QStringLiteral("xgboost")));
        QVERIFY(means.contains(QStringLiteral("baseline_majority_class")));
        QVERIFY(means.contains(QStringLiteral("baseline_always_no_trade")));
        QVERIFY(means.contains(QStringLiteral("baseline_crowd_score_sign")));
        // On a record this separable the model must beat the majority baseline — measured
        // out-of-sample, not asserted (0.9 vs 0.5 when this fixture was designed).
        const double xgb = means.value(QStringLiteral("xgboost")).toObject()
                               .value(QStringLiteral("balanced_accuracy")).toDouble();
        const double majority =
            means.value(QStringLiteral("baseline_majority_class")).toObject()
                .value(QStringLiteral("balanced_accuracy")).toDouble();
        QVERIFY2(xgb > 0.6 && xgb > majority,
                 qPrintable(QStringLiteral("xgb %1 vs majority %2").arg(xgb).arg(majority)));

        const QJsonObject exports = report.value(QStringLiteral("exports")).toObject();
        const double parity = exports.value(QStringLiteral("crowd-xgb.onnx")).toObject()
                                  .value(QStringLiteral("parity_max_abs_diff")).toDouble();
        QVERIFY(parity < 5e-3);
    }
};

QTEST_GUILESS_MAIN(TestCrowdMl)
#include "tst_crowdml.moc"
