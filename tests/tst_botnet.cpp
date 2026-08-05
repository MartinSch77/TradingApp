#include "domain/BotNet.h"
#include "domain/PaperTrader.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace trading;

namespace {

// A model file of the shape tools/train_bot_net.py writes: one hidden unit that
// simply passes the FIRST feature through, so the expected score is arithmetic
// anyone can check by hand rather than a number this test would have to trust.
QJsonObject modelWithOneUnit(double weight, double bias, qint32 samples, double auc)
{
    const QStringList names = entryFeatureNames();
    QJsonArray features;
    QJsonArray mean;
    QJsonArray stddev;
    QJsonArray row;
    for (qsizetype i = 0; i < names.size(); ++i) {
        features.append(names.at(i));
        mean.append(0.0);
        stddev.append(1.0);
        row.append((i == 0) ? weight : 0.0);
    }
    QJsonObject net;
    net.insert(QStringLiteral("features"), features);
    net.insert(QStringLiteral("mean"), mean);
    net.insert(QStringLiteral("stddev"), stddev);
    net.insert(QStringLiteral("w1"), QJsonArray{row});
    net.insert(QStringLiteral("b1"), QJsonArray{0.0});
    net.insert(QStringLiteral("w2"), QJsonArray{1.0});
    net.insert(QStringLiteral("b2"), bias);
    net.insert(QStringLiteral("samples"), samples);
    net.insert(QStringLiteral("valAuc"), auc);
    net.insert(QStringLiteral("valAccuracy"), 0.6);
    net.insert(QStringLiteral("trainedAt"), QStringLiteral("2026-08-05"));
    return net;
}

EntryFeatures featuresWithConfidence(double confidence)
{
    EntryFeatures f;
    f.confidence = confidence;
    f.volPct = 0.2;
    f.stopPct = 1.0;
    f.targetPct = 1.5;
    f.spreadPct = 0.03;
    f.edgeOverCost = 8.0;
    f.leverage = 10;
    f.dir = 1;
    f.hourUtc = 14;
    f.dayOfWeek = 3;
    return f;
}

// The repository root, from this test's own location — the trainer lives there.
QString repoRoot()
{
    return QStringLiteral(TRADINGAPP_SOURCE_DIR);
}

} // namespace

class TestBotNet : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-NET-001 @design DES-DOM-BOTNET
    // @relation(REQ-F-033, scope=function)
    void TS_NET_001_aModelIsReadOrRejectedButNeverHalfUsed()
    {
        // Nothing to read is a stated reason, not a silent zero — the gate treats a
        // score of 0 as "refuse", so an unreadable model must never produce one.
        const BotNet none = botNetFromJson({});
        QVERIFY(!none.ok);
        QVERIFY(!none.error.isEmpty());
        QCOMPARE(none.score({}), 0.0);

        // Weights that do not line up with the feature list are refused by shape.
        QJsonObject broken = modelWithOneUnit(1.0, 0.0, 500, 0.7);
        broken.insert(QStringLiteral("b1"), QJsonArray{0.0, 0.0});   // two biases, one unit
        const BotNet mismatched = botNetFromJson(broken);
        QVERIFY(!mismatched.ok);
        QVERIFY(mismatched.error.contains(QStringLiteral("line up")));

        // A model trained on OTHER features is answering a different question.
        QJsonObject renamed = modelWithOneUnit(1.0, 0.0, 500, 0.7);
        renamed.insert(QStringLiteral("features"), QJsonArray{QStringLiteral("vibes")});
        const BotNet foreign = botNetFromJson(renamed);
        QVERIFY(!foreign.ok);
        QVERIFY(foreign.error.contains(QStringLiteral("other features")));

        // A well-formed one reads back with its measured record intact.
        const BotNet good = botNetFromJson(modelWithOneUnit(1.0, 0.0, 500, 0.7));
        QVERIFY(good.ok);
        QVERIFY(good.error.isEmpty());
        QCOMPARE(good.samples, 500);
        QCOMPARE(good.valAuc, 0.7);
        QCOMPARE(good.features, entryFeatureNames());
    }

    //! @tstid TS-NET-002 @design DES-DOM-BOTNET
    // @relation(REQ-F-033, scope=function)
    void TS_NET_002_theScoreIsTheArithmeticItClaimsToBe()
    {
        const BotNet net = botNetFromJson(modelWithOneUnit(1.0, 0.0, 500, 0.7));
        // One unit, weight 1 on the first feature, mean 0 / sd 1: the answer is
        // sigmoid(tanh(confidence)), which saturates for anything far from zero.
        const double atOne = net.score(entryFeatureMap(featuresWithConfidence(1.0)));
        QVERIFY(qAbs(atOne - (1.0 / (1.0 + std::exp(-std::tanh(1.0))))) < 1e-9);
        QCOMPARE(net.score(entryFeatureMap(featuresWithConfidence(0.0))), 0.5);
        // Monotone in the feature it was given, and inside [0, 1] at every extreme.
        QVERIFY(net.score(entryFeatureMap(featuresWithConfidence(5.0))) > atOne);
        QVERIFY(net.score(entryFeatureMap(featuresWithConfidence(-5.0))) < 0.5);
        const double huge = net.score(entryFeatureMap(featuresWithConfidence(1e9)));
        QVERIFY((huge > 0.0) && (huge <= 1.0));

        // An input the caller does not have becomes the training mean, i.e. neutral.
        QCOMPARE(net.score({}), 0.5);
    }

    //! @tstid TS-NET-003 @design DES-DOM-BOTNET
    // @relation(REQ-F-033, scope=function)
    void TS_NET_003_anUnprovenModelNeverRefusesATrade()
    {
        const NetGateConfig cfg;
        const EntryFeatures bad = featuresWithConfidence(-5.0);   // this model hates it

        // Off: not consulted at all.
        const BotNet trained = botNetFromJson(modelWithOneUnit(1.0, 0.0, 500, 0.7));
        const NetVerdict off = paperNetGate(trained, bad, BotNetMode::Off, cfg);
        QVERIFY(off.allow);
        QVERIFY(!off.scored);

        // No model: allowed, with the reason stated.
        const NetVerdict absent = paperNetGate(botNetFromJson({}), bad, BotNetMode::Gate, cfg);
        QVERIFY(absent.allow);
        QVERIFY(!absent.scored);
        QVERIFY(!absent.why.isEmpty());

        // Too few trades behind it, or no better than a coin flip: it scores and
        // says so, but it may not refuse anything.
        const BotNet young = botNetFromJson(modelWithOneUnit(1.0, 0.0, 10, 0.9));
        const NetVerdict youngVerdict = paperNetGate(young, bad, BotNetMode::Gate, cfg);
        QVERIFY(youngVerdict.allow);
        QVERIFY(youngVerdict.scored);
        QVERIFY(youngVerdict.why.contains(QStringLiteral("not trusted")));
        const BotNet coinFlip = botNetFromJson(modelWithOneUnit(1.0, 0.0, 500, 0.50));
        QVERIFY(paperNetGate(coinFlip, bad, BotNetMode::Gate, cfg).allow);

        // Advise: a trusted model that dislikes the setup still only annotates it.
        const NetVerdict advised = paperNetGate(trained, bad, BotNetMode::Advise, cfg);
        QVERIFY(advised.allow);
        QVERIFY(advised.scored);
        QVERIFY(advised.score < cfg.minScore);

        // Gate: now — and only now — it refuses, with a countable code.
        const NetVerdict gated = paperNetGate(trained, bad, BotNetMode::Gate, cfg);
        QVERIFY(!gated.allow);
        QCOMPARE(gated.code, QStringLiteral("net-score"));
        QVERIFY(gated.why.contains(QStringLiteral("floor")));

        // …and a setup it likes passes the same gate.
        const NetVerdict liked =
            paperNetGate(trained, featuresWithConfidence(5.0), BotNetMode::Gate, cfg);
        QVERIFY(liked.allow);
        QVERIFY(liked.score > cfg.minScore);

        // The window's one-liner says which of those states the bot is in.
        QVERIFY(botNetSummary(trained, BotNetMode::Off, cfg).contains(QStringLiteral("off")));
        QVERIFY(botNetSummary(botNetFromJson({}), BotNetMode::Gate, cfg)
                    .contains(QStringLiteral("none yet")));
        QVERIFY(botNetSummary(young, BotNetMode::Gate, cfg).contains(QStringLiteral("advisory")));
        QVERIFY(botNetSummary(trained, BotNetMode::Gate, cfg).contains(QStringLiteral("gate")));
        QCOMPARE(botNetModeFromWord(QStringLiteral("GATE")), BotNetMode::Gate);
        QCOMPARE(botNetModeFromWord(QStringLiteral("advise")), BotNetMode::Advise);
        QCOMPARE(botNetModeFromWord(QStringLiteral("nonsense")), BotNetMode::Off);
        QCOMPARE(botNetModeWord(BotNetMode::Advise), QStringLiteral("advise"));
    }

    //! @tstid TS-NET-005 @design DES-DOM-BOTNET
    // @relation(REQ-F-033, scope=function)
    void TS_NET_005_theAppTrainsItselfWithoutASecondRuntime()
    {
        // The machines this runs on unattended may have no Python at all, so the
        // learning loop has to close inside the app (REQ-F-033).
        QList<TrainingExample> examples;
        for (int i = 0; i < 300; ++i) {
            const bool win = (i % 2) == 0;
            TrainingExample e;
            e.features = featuresWithConfidence(win ? 70.0 : 15.0);
            e.features.hourUtc = i % 24;
            e.win = win;
            examples.append(e);
        }
        const TrainResult trained = trainBotNet(examples);
        QVERIFY2(trained.ok, qPrintable(trained.message));
        QVERIFY(trained.net.ok);
        QCOMPARE(trained.net.features, entryFeatureNames());
        QVERIFY(trained.net.samples >= 200);
        QVERIFY2(trained.net.valAuc > 0.9, qPrintable(QStringLiteral("AUC %1")
                                                          .arg(trained.net.valAuc)));
        QVERIFY(!trained.net.trainedAt.isEmpty());
        QVERIFY(trained.message.contains(QStringLiteral("AUC")));
        // It learned the separation it was shown…
        QVERIFY(trained.net.score(entryFeatureMap(featuresWithConfidence(70.0)))
                > trained.net.score(entryFeatureMap(featuresWithConfidence(15.0))));
        // …and having earned both thresholds, it may now refuse.
        const NetVerdict verdict =
            paperNetGate(trained.net, featuresWithConfidence(15.0), BotNetMode::Gate, {});
        QVERIFY(!verdict.allow);
        QCOMPARE(verdict.code, QStringLiteral("net-score"));

        // Two runs over one record agree — a model that changed on every retrain
        // would make the record it is judged by meaningless.
        const TrainResult again = trainBotNet(examples);
        QVERIFY(again.ok);
        QCOMPARE(again.net.valAuc, trained.net.valAuc);
        QCOMPARE(again.net.b2, trained.net.b2);

        // It round-trips through the file the app writes and reads.
        const BotNet reloaded = botNetFromJson(botNetToJson(trained.net));
        QVERIFY2(reloaded.ok, qPrintable(reloaded.error));
        QCOMPARE(reloaded.samples, trained.net.samples);
        QCOMPARE(reloaded.valAuc, trained.net.valAuc);
        QCOMPARE(reloaded.score(entryFeatureMap(featuresWithConfidence(70.0))),
                 trained.net.score(entryFeatureMap(featuresWithConfidence(70.0))));

        // Too small a record, or one with a single kind of outcome in it, produces
        // NO weights and a stated reason — never a model that scores everything.
        const TrainResult tiny = trainBotNet(examples.mid(0, 10));
        QVERIFY(!tiny.ok);
        QVERIFY(!tiny.net.ok);
        QVERIFY(tiny.message.contains(QStringLiteral("collecting")));
        QList<TrainingExample> allWinners;
        for (int i = 0; i < 100; ++i) {
            TrainingExample e;
            e.features = featuresWithConfidence(50.0);
            e.win = true;
            allWinners.append(e);
        }
        const TrainResult flat = trainBotNet(allWinners);
        QVERIFY(!flat.ok);
        QVERIFY(flat.message.contains(QStringLiteral("same way")));

        // And one line of the experience log becomes one example — or nothing,
        // when it is not readable, rather than a half-read one.
        QJsonObject features;
        const QStringList names = entryFeatureNames();
        const QList<double> values = entryFeatureValues(featuresWithConfidence(42.0));
        for (qsizetype i = 0; i < names.size(); ++i) {
            features.insert(names.at(i), values.at(i));
        }
        QJsonObject line;
        line.insert(QStringLiteral("features"), features);
        line.insert(QStringLiteral("netPnl"), -12.5);
        const std::optional<TrainingExample> parsed = experienceExampleFrom(line);
        QVERIFY(parsed.has_value());
        const TrainingExample example = parsed.value_or(TrainingExample{});
        QVERIFY(!example.win);                       // net after costs, so a loss
        QCOMPARE(example.features.confidence, 42.0);
        line.insert(QStringLiteral("netPnl"), 0.01);
        QVERIFY(experienceExampleFrom(line).value_or(TrainingExample{}).win);
        QVERIFY(!experienceExampleFrom({}).has_value());
        QJsonObject noOutcome;
        noOutcome.insert(QStringLiteral("features"), features);
        QVERIFY(!experienceExampleFrom(noOutcome).has_value());
        QJsonObject partial;
        QJsonObject fewer = features;
        fewer.remove(QStringLiteral("volPct"));
        partial.insert(QStringLiteral("features"), fewer);
        partial.insert(QStringLiteral("netPnl"), 5.0);
        QVERIFY(!experienceExampleFrom(partial).has_value());
    }

    //! @tstid TS-NET-004 @design DES-DOM-BOTNET
    // @relation(REQ-F-033, scope=function)
    void TS_NET_004_theTrainerAndTheAppAgreeOnTheModelTheyExchange()
    {
        // The one test that pins the CONTRACT between the two languages: the trainer
        // really runs, on a record the app's own writer could have produced, and the
        // file it emits is one this build can read, trust and score with.
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString logPath = dir.filePath(QStringLiteral("experience.jsonl"));
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::WriteOnly | QIODevice::Text));
        const QStringList names = entryFeatureNames();
        // A learnable record: high confidence wins, low confidence loses. 240 trades
        // is above the app's own trust threshold, so the round trip covers the
        // "trusted" branch too.
        for (int i = 0; i < 240; ++i) {
            const bool win = (i % 2) == 0;
            EntryFeatures f = featuresWithConfidence(win ? 70.0 : 15.0);
            f.hourUtc = i % 24;
            QJsonObject features;
            const QList<double> values = entryFeatureValues(f);
            for (qsizetype k = 0; k < names.size(); ++k) {
                features.insert(names.at(k), values.at(k));
            }
            QJsonObject rec;
            rec.insert(QStringLiteral("symbol"), QStringLiteral("SPX500"));
            rec.insert(QStringLiteral("netPnl"), win ? 42.0 : -37.0);
            rec.insert(QStringLiteral("features"), features);
            static_cast<void>(log.write(QJsonDocument(rec).toJson(QJsonDocument::Compact) + "\n"));
        }
        log.close();

        const QString modelPath = dir.filePath(QStringLiteral("botnet.json"));
        QProcess trainer;
        trainer.start(QStringLiteral("python3"),
                      {repoRoot() + QStringLiteral("/tools/train_bot_net.py"),
                       QStringLiteral("--log"), logPath, QStringLiteral("--out"), modelPath,
                       QStringLiteral("--epochs"), QStringLiteral("60")});
        if (!trainer.waitForStarted(5000)) {
            QSKIP("python3 is not available on this host");
        }
        QVERIFY(trainer.waitForFinished(120000));
        QCOMPARE(trainer.exitCode(), 0);

        QFile written(modelPath);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const BotNet net = botNetFromJson(QJsonDocument::fromJson(written.readAll()).object());
        QVERIFY2(net.ok, qPrintable(net.error));
        QCOMPARE(net.features, names);          // the two languages' column order agrees
        QVERIFY(net.samples > 100);
        QVERIFY(net.valAuc >= 0.5);             // it learned the pattern it was shown
        QVERIFY(!net.trainedAt.isEmpty());
        // …and it separates the two kinds of trade it was trained on.
        const double good = net.score(entryFeatureMap(featuresWithConfidence(70.0)));
        const double poor = net.score(entryFeatureMap(featuresWithConfidence(15.0)));
        QVERIFY2(good > poor, qPrintable(QStringLiteral("%1 vs %2").arg(good).arg(poor)));

        // A record with nothing to learn from is refused with the "skipped" code
        // rather than turned into weights that mean nothing.
        const QString flatPath = dir.filePath(QStringLiteral("flat.jsonl"));
        QFile flat(flatPath);
        QVERIFY(flat.open(QIODevice::WriteOnly | QIODevice::Text));
        for (int i = 0; i < 60; ++i) {
            QJsonObject features;
            const QList<double> values = entryFeatureValues(featuresWithConfidence(50.0));
            for (qsizetype k = 0; k < names.size(); ++k) {
                features.insert(names.at(k), values.at(k));
            }
            QJsonObject rec;
            rec.insert(QStringLiteral("netPnl"), 10.0);   // every single trade a winner
            rec.insert(QStringLiteral("features"), features);
            static_cast<void>(flat.write(QJsonDocument(rec).toJson(QJsonDocument::Compact) + "\n"));
        }
        flat.close();
        QProcess flatRun;
        flatRun.start(QStringLiteral("python3"),
                      {repoRoot() + QStringLiteral("/tools/train_bot_net.py"),
                       QStringLiteral("--log"), flatPath, QStringLiteral("--out"),
                       dir.filePath(QStringLiteral("flat.json"))});
        QVERIFY(flatRun.waitForFinished(60000));
        QCOMPARE(flatRun.exitCode(), 3);        // the project's "skipped" code
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("flat.json"))));
    }
};

QTEST_GUILESS_MAIN(TestBotNet)
#include "tst_botnet.moc"
