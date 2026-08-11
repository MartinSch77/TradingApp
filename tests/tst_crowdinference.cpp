// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/CrowdInference.h"

#include <QtTest/QtTest>

#include <cmath>

using namespace trading::crowd;

// The PURE half of the crowd-model inference (REQ-F-042): the metadata contract a Phase 4
// export embeds, the by-name feature assembly against it, and the probability shaping. No
// runtime, no file, no network — these rules must hold on every machine, including the ones
// that will never install ONNX Runtime.
namespace {

QHash<QString, QString> validProps()
{
    QHash<QString, QString> props;
    props.insert(QStringLiteral("feature_names"), QStringLiteral(R"(["a","b","c"])"));
    props.insert(QStringLiteral("imputation_medians"), QStringLiteral("[1.5, -2.0, 0.0]"));
    props.insert(QStringLiteral("classes"), QStringLiteral(R"(["SHORT","NO_TRADE","LONG"])"));
    props.insert(QStringLiteral("manifest_version"), QStringLiteral("1"));
    props.insert(QStringLiteral("instrument"), QStringLiteral("SPX500"));
    props.insert(QStringLiteral("horizon_days"), QStringLiteral("5"));
    props.insert(QStringLiteral("dead_zone_pct"), QStringLiteral("0.25"));
    props.insert(QStringLiteral("training_rows"), QStringLiteral("265"));
    return props;
}

} // namespace

class TestCrowdInference : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-INF-001 @design DES-DOM-CROWDINFER
    // @relation(REQ-F-042, scope=function)
    void TS_INF_001_theMetadataContractIsReadOrRefusedNeverGuessed()
    {
        // A model that cannot say what it eats or answers must not be scored: every broken
        // contract is a NAMED refusal, and only the complete one parses.
        const CrowdModelMeta good = crowdModelMetaFromProps(validProps());
        QVERIFY2(good.ok, qPrintable(good.error));
        QCOMPARE(good.features, (QStringList{QStringLiteral("a"), QStringLiteral("b"),
                                             QStringLiteral("c")}));
        QCOMPARE(good.medians, (QList<double>{1.5, -2.0, 0.0}));
        QCOMPARE(good.classes.size(), 3);
        QCOMPARE(good.manifestVersion, 1);
        QCOMPARE(good.instrument, QStringLiteral("SPX500"));
        QCOMPARE(good.horizonDays, 5);
        QCOMPARE(good.trainingRows, qint64(265));

        // Each contract key, absent: refused with the key named.
        for (const auto &key : {QStringLiteral("feature_names"),
                                QStringLiteral("imputation_medians"),
                                QStringLiteral("classes")}) {
            QHash<QString, QString> props = validProps();
            props.remove(key);
            const CrowdModelMeta refused = crowdModelMetaFromProps(props);
            QVERIFY(!refused.ok);
            QVERIFY2(refused.error.contains(key), qPrintable(refused.error));
        }

        // Medians that do not pair 1:1 with the features break the imputation contract.
        QHash<QString, QString> mismatched = validProps();
        mismatched.insert(QStringLiteral("imputation_medians"), QStringLiteral("[1.0, 2.0]"));
        QVERIFY(!crowdModelMetaFromProps(mismatched).ok);

        // No classes = unlabelled probability columns; no features = nothing to assemble;
        // prose instead of JSON = unreadable; a number where a name belongs = unreadable.
        QHash<QString, QString> broken = validProps();
        broken.insert(QStringLiteral("classes"), QStringLiteral("[]"));
        QVERIFY(!crowdModelMetaFromProps(broken).ok);
        broken = validProps();
        broken.insert(QStringLiteral("feature_names"), QStringLiteral("[]"));
        broken.insert(QStringLiteral("imputation_medians"), QStringLiteral("[]"));
        QVERIFY(!crowdModelMetaFromProps(broken).ok);
        broken = validProps();
        broken.insert(QStringLiteral("feature_names"), QStringLiteral("not json at all"));
        QVERIFY(!crowdModelMetaFromProps(broken).ok);
        broken = validProps();
        broken.insert(QStringLiteral("imputation_medians"), QStringLiteral(R"([1.0, "x", 2.0])"));
        QVERIFY(!crowdModelMetaFromProps(broken).ok);
    }

    //! @tstid TS-INF-002 @design DES-DOM-CROWDINFER
    // @relation(REQ-F-042, scope=function)
    void TS_INF_002_featuresAreMatchedByNameAndMissingCarriesTheTrainersMedian()
    {
        const CrowdModelMeta meta = crowdModelMetaFromProps(validProps());
        QVERIFY(meta.ok);

        // A full input in any map order lands in the METADATA's order, nothing imputed.
        const CrowdFeatureVector full = assembleCrowdFeatures(
            meta, {{QStringLiteral("c"), 4.0}, {QStringLiteral("a"), 2.0},
                   {QStringLiteral("b"), 3.0}});
        QVERIFY(full.ok);
        QCOMPARE(full.values, (QList<float>{2.0F, 3.0F, 4.0F}));
        QCOMPARE(full.imputed, 0);

        // Missing features carry the trainer's medians — and are COUNTED, because a
        // prediction made of fill-ins is a weaker claim.
        const CrowdFeatureVector partial =
            assembleCrowdFeatures(meta, {{QStringLiteral("a"), 2.0}});
        QVERIFY(partial.ok);
        QCOMPARE(partial.values, (QList<float>{2.0F, -2.0F, 0.0F}));
        QCOMPARE(partial.imputed, 2);

        // A non-finite value is a measurement that never happened: imputed, never fed as NaN.
        const CrowdFeatureVector nan = assembleCrowdFeatures(
            meta, {{QStringLiteral("a"), std::nan("")}, {QStringLiteral("b"), 1.0},
                   {QStringLiteral("c"), 1.0}});
        QVERIFY(nan.ok);
        QCOMPARE(nan.values.first(), 1.5F);
        QCOMPARE(nan.imputed, 1);

        // A name the model never declared is ignored — the caller may know more than the
        // model eats — and a refused metadata refuses the assembly with its reason carried.
        const CrowdFeatureVector extra = assembleCrowdFeatures(
            meta, {{QStringLiteral("a"), 1.0}, {QStringLiteral("b"), 1.0},
                   {QStringLiteral("c"), 1.0}, {QStringLiteral("unheard_of"), 9.0}});
        QVERIFY(extra.ok);
        QCOMPARE(extra.values.size(), 3);
        QVERIFY(!assembleCrowdFeatures(CrowdModelMeta{}, {}).ok);
    }

    //! @tstid TS-INF-003 @design DES-DOM-CROWDINFER
    // @relation(REQ-F-042, scope=function)
    void TS_INF_003_probabilitiesAreLabelledByTheMetadataOrRefused()
    {
        const CrowdModelMeta meta = crowdModelMetaFromProps(validProps());
        QVERIFY(meta.ok);

        // A proper distribution: labelled by the metadata's class order, argmax named, the
        // imputation count carried through to the caller.
        const CrowdPrediction good = crowdPredictionFrom(meta, {0.2, 0.3, 0.5}, 4);
        QVERIFY2(good.ok, qPrintable(good.error));
        QCOMPARE(good.classes, meta.classes);
        QCOMPARE(good.topClass, QStringLiteral("LONG"));
        QCOMPARE(good.topProbability, 0.5);
        QCOMPARE(good.imputed, 4);

        // Refusals, each named: a column count that cannot be labelled, values that are not
        // probabilities, and a sum that is not a distribution — never repaired into one.
        QVERIFY(!crowdPredictionFrom(meta, {0.5, 0.5}, 0).ok);
        QVERIFY(!crowdPredictionFrom(meta, {0.2, -0.1, 0.9}, 0).ok);
        QVERIFY(!crowdPredictionFrom(meta, {0.2, 0.2, 0.2}, 0).ok);
        QVERIFY(!crowdPredictionFrom(meta, {0.2, std::nan(""), 0.5}, 0).ok);
        QVERIFY(!crowdPredictionFrom(CrowdModelMeta{}, {1.0}, 0).ok);
    }

    //! @tstid TS-INF-006 @design DES-DOM-CROWDINFER
    // @relation(REQ-F-046, scope=function)
    void TS_INF_006_theBotEvidenceLineIsHonestWordsOrNoLineAtAll()
    {
        // Nothing measured is NO line — absent evidence must never read as a neutral zero.
        QVERIFY(crowdEvidenceLine(QStringLiteral("SPX500"), CrowdScoreResult{},
                                  CrowdPrediction{})
                    .isEmpty());

        // The score alone: the instrument, the experimental label and the score's own words.
        CrowdScoreResult score;
        score.score = -0.4;
        score.direction = QStringLiteral("bearish");
        score.confidence = 0.6;
        score.coverage = 0.5;
        const QString scoreOnly =
            crowdEvidenceLine(QStringLiteral("SPX500"), score, CrowdPrediction{});
        QVERIFY(scoreOnly.contains(QStringLiteral("SPX500")));
        QVERIFY(scoreOnly.contains(QStringLiteral("experimental")));
        QVERIFY(scoreOnly.contains(QStringLiteral("bearish")));

        // With a model answer: labelled probabilities, the imputation count, and the word
        // UNCALIBRATED — a probability is measured, never asserted. Never an instruction.
        const CrowdModelMeta meta = crowdModelMetaFromProps(validProps());
        const CrowdPrediction prediction = crowdPredictionFrom(meta, {0.2, 0.3, 0.5}, 4);
        QVERIFY(prediction.ok);
        const QString full = crowdEvidenceLine(QStringLiteral("NSDQ100"), score, prediction);
        QVERIFY(full.contains(QStringLiteral("uncalibrated")));
        QVERIFY(full.contains(QStringLiteral("4 inputs imputed")));
        QVERIFY(full.contains(QStringLiteral("LONG 50%")));
        QVERIFY(!full.contains(QStringLiteral("BUY")));
        QVERIFY(!full.contains(QStringLiteral("SELL")));

        // A prediction with no score still speaks — one measured half is evidence too.
        QVERIFY(!crowdEvidenceLine(QStringLiteral("NSDQ100"), CrowdScoreResult{}, prediction)
                     .isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCrowdInference)
#include "tst_crowdinference.moc"
