// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CrowdFixtures.h"

#include "services/CrowdModel.h"

#include <QtTest/QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace trading::crowd;
using namespace crowdtest;

// The OPTIONAL in-process inference (REQ-F-042, Phase 5). Two facts are pinned: the seam is
// mock-able (a consumer holds ICrowdModel and never needs the runtime), and the real
// OnnxCrowdModel is honest in EVERY build — a machine without the runtime answers
// "unavailable" with the remedy named, a machine with it round-trips the very models the
// Phase 4 pipeline exports, driven by their own embedded metadata.
namespace {

// The test double that proves the seam: a consumer written against ICrowdModel runs on canned
// answers, no runtime anywhere.
class FakeCrowdModel final : public ICrowdModel
{
public:
    FakeCrowdModel()
    {
        m_meta.ok = true;
        m_meta.features = QStringList{QStringLiteral("vix_z")};
        m_meta.medians = QList<double>{0.0};
        m_meta.classes = QStringList{QStringLiteral("SHORT"), QStringLiteral("LONG")};
    }

    [[nodiscard]] bool available() const override { return true; }
    [[nodiscard]] bool ready() const override { return true; }
    [[nodiscard]] QString status() const override { return QStringLiteral("fake: ready"); }
    [[nodiscard]] CrowdModelMeta meta() const override { return m_meta; }
    bool load(const QString &path) override
    {
        Q_UNUSED(path);
        return true;
    }
    [[nodiscard]] CrowdPrediction predict(const QHash<QString, double> &featuresByName) override
    {
        // Canned but shape-honest: bearish when the one feature says high volatility.
        const double z = featuresByName.value(QStringLiteral("vix_z"), 0.0);
        const double pShort = z > 0.0 ? 0.8 : 0.2;
        return crowdPredictionFrom(m_meta, {pShort, 1.0 - pShort}, 0);
    }

private:
    CrowdModelMeta m_meta;
};

} // namespace

class TestCrowdModel : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-INF-004 @design DES-SVC-CROWDMODEL
    // @relation(REQ-F-042, scope=function)
    void TS_INF_004_theSeamIsMockableAndEveryFailureIsANamedResultNeverACrash()
    {
        // The seam: a consumer holding only ICrowdModel gets labelled, shaped answers from a
        // double — which is what makes everything downstream testable without the runtime.
        FakeCrowdModel fake;
        ICrowdModel &seam = fake;
        QVERIFY(seam.available());
        const CrowdPrediction bearish =
            seam.predict({{QStringLiteral("vix_z"), 2.0}});
        QVERIFY(bearish.ok);
        QCOMPARE(bearish.topClass, QStringLiteral("SHORT"));

        // The real implementation, before any load: not ready, a status in words, and a
        // predict that answers an error result rather than crashing.
        OnnxCrowdModel model;
        QVERIFY(!model.ready());
        QVERIFY(!model.status().isEmpty());
        const CrowdPrediction early = model.predict({});
        QVERIFY(!early.ok);
        QVERIFY(!early.error.isEmpty());

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());

        if (!model.available()) {
            // The stub build IS a correct build: unavailable in words that name the remedy,
            // and a load that fails honestly instead of pretending.
            QVERIFY2(model.status().contains(QStringLiteral("setup.sh ml")),
                     qPrintable(model.status()));
            QVERIFY(!model.load(dir.filePath(QStringLiteral("any.onnx"))));
            QVERIFY(!model.ready());
            return;
        }

        // With the runtime present: a missing file and a file that is not a model are each a
        // named refusal that leaves NO half-usable session.
        QVERIFY(!model.load(dir.filePath(QStringLiteral("missing.onnx"))));
        QVERIFY2(model.status().contains(QStringLiteral("not found")),
                 qPrintable(model.status()));
        const QString junkPath = dir.filePath(QStringLiteral("junk.onnx"));
        QFile junk(junkPath);
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("this is not an onnx graph");
        junk.close();
        QVERIFY(!model.load(junkPath));
        QVERIFY(!model.ready());
        QVERIFY(!model.status().isEmpty());
        QVERIFY(!model.predict({}).ok);
    }

    //! @tstid TS-INF-005 @design DES-SVC-CROWDMODEL
    // @relation(REQ-F-042, scope=function)
    void TS_INF_005_theExportedModelsScoreInProcessDrivenByTheirOwnMetadata()
    {
        // The full circle: the Phase 4 pipeline trains and exports over a learnable record,
        // and THIS build loads the files and reproduces the fit — features matched by name
        // against the embedded metadata, medians imputing what a caller omits. Skips without
        // the runtime or the training environment, like every licence-bound stage.
        OnnxCrowdModel model;
        if (!model.available()) {
            QSKIP("built without ONNX Runtime (./setup.sh ml + reconfigure)");
        }
        const QString python = mlPython();
        if (python.isEmpty()) {
            QSKIP("the optional ML environment is not provisioned (./setup.sh ml)");
        }

        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString storePath = dir.filePath(QStringLiteral("crowd.db"));
        const QString prices = dir.filePath(QStringLiteral("prices.csv"));
        QVERIFY(writeRegimeFixture(storePath, prices, QDate(2025, 9, 1), 270));
        const QString dataset = dir.filePath(QStringLiteral("dataset.csv"));
        const QString manifest = dir.filePath(QStringLiteral("manifest.json"));
        ToolRun run = runPython(
            QStringLiteral("python3"),
            buildToolArgs(storePath, prices, dataset, manifest,
                          {QStringLiteral("--dead-zone-pct"), QStringLiteral("0.5")}),
            60000);
        if (!run.started) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));
        const QString outDir = dir.filePath(QStringLiteral("out"));
        run = runPython(python,
                        trainerArgs(dataset, manifest, outDir,
                                    {QStringLiteral("--min-samples"), QStringLiteral("100"),
                                     QStringLiteral("--xgb-estimators"), QStringLiteral("25")}),
                        300000);
        QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr));

        // The XGBoost export loads, and its DECLARED contract matches the manifest it was
        // trained from.
        QVERIFY2(model.load(outDir + QStringLiteral("/crowd-xgb.onnx")),
                 qPrintable(model.status()));
        QVERIFY(model.ready());
        const CrowdModelMeta meta = model.meta();
        const QJsonObject manifestJson = QJsonDocument::fromJson(fileBytes(manifest)).object();
        const QJsonArray manifestFeatures =
            manifestJson.value(QStringLiteral("features")).toArray();
        QCOMPARE(meta.features.size(), manifestFeatures.size());
        QCOMPARE(meta.manifestVersion, 1);
        QCOMPARE(meta.instrument, QStringLiteral("SPX500"));
        QVERIFY(meta.classes.size() >= 2);

        // Scored over EVERY dataset row (features by NAME from the CSV, empty cells left to
        // the medians), the in-process model reproduces the fit it reported: agreement with
        // the labels well above chance on this near-separable record.
        const QList<QHash<QString, QString>> rows = readCsv(dataset);
        QVERIFY(rows.size() > 200);
        int agreed = 0;
        for (const auto &row : rows) {
            QHash<QString, double> byName;
            for (const QString &name : meta.features) {
                const QString cell = row.value(name);
                if (!cell.isEmpty()) {
                    byName.insert(name, cell.toDouble());
                }
            }
            const CrowdPrediction prediction = model.predict(byName);
            QVERIFY2(prediction.ok, qPrintable(prediction.error));
            double sum = 0.0;
            for (const double p : prediction.probabilities) {
                sum += p;
            }
            QVERIFY(qAbs(sum - 1.0) < 0.02);
            if (prediction.topClass == row.value(QStringLiteral("label"))) {
                ++agreed;
            }
        }
        const double agreement = double(agreed) / double(rows.size());
        QVERIFY2(agreement > 0.7, qPrintable(QStringLiteral("agreement %1").arg(agreement)));

        // A caller who knows NOTHING still gets an honest answer: every feature imputed with
        // the trainer's median, and the count says so.
        const CrowdPrediction blind = model.predict({});
        QVERIFY2(blind.ok, qPrintable(blind.error));
        QCOMPARE(blind.imputed, qint32(meta.features.size()));

        // The logistic-regression export loads through the same seam.
        QVERIFY2(model.load(outDir + QStringLiteral("/crowd-logreg.onnx")),
                 qPrintable(model.status()));
        QVERIFY(model.predict({}).ok);
    }
};

QTEST_GUILESS_MAIN(TestCrowdModel)
#include "tst_crowdmodel.moc"
