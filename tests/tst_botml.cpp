// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CrowdFixtures.h"

#include "domain/PredictionLedger.h"

#include <QtTest/QtTest>

#include <QProcess>
#include <QTemporaryDir>

// The OFFLINE bot-model training pipeline (item 9 of the 2026-08-12 strategy redesign;
// tools/ml/bot_dataset.py + train_bot_model.py). Reuses crowdtest's generic Python-process
// and CSV-reading fixtures (crowdtest::runPython/mlPython/readCsv) verbatim — only the tool
// paths and the ledger fixture are specific to this pipeline, exactly mirroring how
// tst_crowdml.cpp drives the crowd half. The ledger fixture is built with the REAL
// trading::appendPrediction — the same function BotSimRunner itself calls — so the file this
// test hands to Python is guaranteed to be in the format the app actually writes, not a
// hand-typed approximation of it.
using namespace trading;

namespace {
QString repoRoot()
{
    return QStringLiteral(TRADINGAPP_SOURCE_DIR);
}

QString datasetTool()
{
    return repoRoot() + QStringLiteral("/tools/ml/bot_dataset.py");
}

QString trainerTool()
{
    return repoRoot() + QStringLiteral("/tools/ml/train_bot_model.py");
}

// A LEARNABLE ledger fixture: `count` rows, five minutes apart, whose composite `dir`
// usually (not always — a real signal is noisy) agrees with the direction price actually
// drifts over the next few rows, so a real model has something above chance to find while
// the pipeline itself is exercised end-to-end. Deterministic (no RNG): the drift alternates
// in a fixed pattern rather than being random, so the fixture is reproducible without a seed.
bool writeLearnableLedger(const QString &path, const QDateTime &start, int count)
{
    double price = 5000.0;
    for (int i = 0; i < count; ++i) {
        // Alternates a rising and falling stretch every 40 rows — long enough for
        // walkPath's default 20-bar horizon to resolve against it.
        const bool risingStretch = ((i / 40) % 2) == 0;
        price *= risingStretch ? 1.0006 : 0.9994;

        Prediction p;
        p.at = start.addSecs(static_cast<qint64>(i) * 300);
        p.symbol = QStringLiteral("SPX500");
        p.price = price;
        p.dir = risingStretch ? 1 : -1;
        p.strength = 55.0 + ((i % 7) * 5.0);   // varies, always well above a 40-strength floor
        p.measured = 6;
        p.unknowns = 3;
        p.regime = risingStretch ? Regime::Trend : Regime::Range;
        p.taken = false;
        p.refusal = QStringLiteral("no-confluence");
        p.priorMoveDir = risingStretch ? 1 : -1;
        p.vwapSide = risingStretch ? 1 : -1;
        if (!appendPrediction(path, p)) {
            return false;
        }
    }
    return true;
}
} // namespace

class TestBotMl : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-BOTML-001 @design DES-ML-BOTTRAIN
    // @relation(REQ-F-037, scope=function)
    //
    // bot_dataset.py build over a REAL PredictionLedger fixture (written via
    // appendPrediction, not hand-typed JSON): every row labels, the label counts are
    // reported, and the manifest carries the feature list a trainer would need.
    void TS_BOTML_001_datasetBuildLabelsARealLedgerFixture()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString ledger = dir.filePath(QStringLiteral("prediction-ledger.jsonl"));
        const QDateTime start(QDate(2026, 8, 1), QTime(9, 0), QTimeZone::UTC);
        QVERIFY(writeLearnableLedger(ledger, start, 300));

        const QString dataset = dir.filePath(QStringLiteral("dataset.csv"));
        const QString manifest = dir.filePath(QStringLiteral("manifest.json"));
        const crowdtest::ToolRun run = crowdtest::runPython(
            QStringLiteral("python3"),
            {datasetTool(), QStringLiteral("build"), QStringLiteral("--ledger"), ledger,
             QStringLiteral("--instrument"), QStringLiteral("SPX500"), QStringLiteral("--out"),
             dataset, QStringLiteral("--manifest"), manifest},
            30000);
        if (!run.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));

        const QList<QHash<QString, QString>> rows = crowdtest::readCsv(dataset);
        // The last ~20 rows cannot resolve a label (fewer than max-bars later rows exist) and
        // are dropped, never guessed at.
        QVERIFY(rows.size() >= 270);
        QVERIFY(rows.size() < 300);
        for (const auto &row : rows) {
            QVERIFY(row.contains(QStringLiteral("label")));
            const QString label = row.value(QStringLiteral("label"));
            QVERIFY(label == QStringLiteral("LONG") || label == QStringLiteral("SHORT")
                    || label == QStringLiteral("NO_TRADE"));
            // Every feature column this manifest promises is present on every row.
            QVERIFY(row.contains(QStringLiteral("strength")));
            QVERIFY(row.contains(QStringLiteral("dir")));
            QVERIFY(row.contains(QStringLiteral("regime_trend")));
        }

        const QByteArray manifestBytes = crowdtest::fileBytes(manifest);
        QVERIFY(!manifestBytes.isEmpty());
        QVERIFY(manifestBytes.contains("\"strength\""));
        QVERIFY(manifestBytes.contains("\"label_classes\""));
    }

    //! @tstid TS-BOTML-002 @design DES-ML-BOTTRAIN
    // @relation(REQ-F-037, scope=function)
    //
    // The end-to-end pipeline (dataset build, then the fitting half) over the SAME learnable
    // fixture: refuses honestly (SKIP, per the shared discipline) when the ML environment is
    // not provisioned, otherwise runs to completion and writes both ONNX exports plus a
    // report whose numbers come from purged walk-forward folds.
    void TS_BOTML_002_endToEndTrainingRunsOrHonestlySkips()
    {
        const QString python = crowdtest::mlPython();
        if (python.isEmpty()) {
            QSKIP("the ML environment is not provisioned (./setup.sh ml)");
        }

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString ledger = dir.filePath(QStringLiteral("prediction-ledger.jsonl"));
        const QDateTime start(QDate(2026, 8, 1), QTime(9, 0), QTimeZone::UTC);
        QVERIFY(writeLearnableLedger(ledger, start, 600));

        const QString dataset = dir.filePath(QStringLiteral("dataset.csv"));
        const QString manifest = dir.filePath(QStringLiteral("manifest.json"));
        const crowdtest::ToolRun buildRun = crowdtest::runPython(
            QStringLiteral("python3"),
            {datasetTool(), QStringLiteral("build"), QStringLiteral("--ledger"), ledger,
             QStringLiteral("--instrument"), QStringLiteral("SPX500"), QStringLiteral("--out"),
             dataset, QStringLiteral("--manifest"), manifest},
            30000);
        if (!buildRun.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(buildRun.exitCode == 0, qPrintable(buildRun.stdErr));

        const QString outDir = dir.filePath(QStringLiteral("out"));
        const crowdtest::ToolRun trainRun = crowdtest::runPython(
            python,
            {trainerTool(), QStringLiteral("--dataset"), dataset, QStringLiteral("--manifest"),
             manifest, QStringLiteral("--out-dir"), outDir, QStringLiteral("--min-samples"),
             QStringLiteral("100"), QStringLiteral("--embargo-days"), QStringLiteral("0.5")},
            180000);
        QVERIFY(trainRun.started);
        QVERIFY2(trainRun.exitCode == 0, qPrintable(trainRun.stdErr));

        QVERIFY(QFile::exists(outDir + QStringLiteral("/bot-logreg.onnx")));
        QVERIFY(QFile::exists(outDir + QStringLiteral("/bot-xgb.onnx")));
        const QByteArray report = crowdtest::fileBytes(outDir
                                                        + QStringLiteral("/training-report.json"));
        QVERIFY(!report.isEmpty());
        QVERIFY(report.contains("purged walk-forward"));
        QVERIFY(report.contains("baseline_composite_sign"));
    }
};

QTEST_GUILESS_MAIN(TestBotMl)
#include "tst_botml.moc"
