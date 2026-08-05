#ifndef TRADINGAPP_DOMAIN_BOTNET_H
#define TRADINGAPP_DOMAIN_BOTNET_H

#include "domain/PaperTrader.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// The bot learning from its own record (REQ-F-033): a small feed-forward network,
// trained from the experience log the simulator writes, and evaluated to score a
// candidate before it is taken. BOTH halves are C++ — the app trains itself, with
// no second runtime to install, because the machines this runs on (a Raspberry Pi
// left trading for weeks) may have no Python at all. tools/train_bot_net.py is the
// optional desktop counterpart that writes the identical file.
//
// Deliberately tiny and dependency-free. One hidden layer with tanh and a sigmoid
// output is enough for a few thousand examples over eleven features, and it means
// the app needs no runtime beyond what it already links: a trained model is a JSON
// file of numbers, and scoring one candidate is a few dozen multiplications on the
// GUI thread's own budget.
//
// Three properties are load-bearing, and all three exist because a model fitted to
// a handful of trades is confidently wrong:
//
//  * A model must EARN the right to refuse. Below the sample and quality
//    thresholds it is reported and logged but never blocks a trade — an untrained
//    network that vetoes everything would look exactly like a broken bot.
//  * Inputs are matched BY NAME against the model's own feature list. A feature
//    added to the app then produces a mismatch that is detected, instead of the
//    network reading its inputs one column out of step.
//  * A model that cannot be read at all is an error to report, never a silent
//    "score 0" — which the gate would otherwise treat as "refuse everything".
namespace trading {

// What the trainer wrote, as the app reads it back.
struct BotNet {
    bool ok = false;
    QString error;              // why it could not be read (empty when ok)

    QStringList features;       // input names, in the order the weights expect
    QList<double> mean;         // per-feature standardisation, from the training set
    QList<double> stddev;
    QList<QList<double>> w1;    // [hidden][input]
    QList<double> b1;           // [hidden]
    QList<double> w2;           // [hidden]
    double b2 = 0.0;

    // What the training run measured — the basis on which this model is trusted.
    qint32 samples = 0;         // training examples it saw
    double valAuc = 0.0;        // area under ROC on the held-out (LATER) trades
    double valAccuracy = 0.0;
    QString trainedAt;          // ISO date of the run

    // p(the trade ends positive), 0..1. Missing inputs count as the training
    // mean, i.e. "nothing unusual", which is the only neutral answer available.
    [[nodiscard]] double score(const QHash<QString, double> &inputs) const;
};

// Parse a model file. A malformed or empty object yields ok=false with a reason.
[[nodiscard]] BotNet botNetFromJson(const QJsonObject &obj);
// …and the other direction, so the app can train and save one itself.
[[nodiscard]] QJsonObject botNetToJson(const BotNet &net);

// ---------------------------------------------------------------------------
// Training, in C++ (REQ-F-033)
// ---------------------------------------------------------------------------
//
// The bot trains itself: the target this app runs on may have no Python at all,
// and a learning loop that needs a second runtime to close is a learning loop
// that stops on a Raspberry Pi. tools/train_bot_net.py stays as the offline
// counterpart for experimenting on a desktop — both write the SAME model file,
// and the tests pin that they do.

// One closed trade as the network sees it: what was true at entry, and whether
// the account kept anything. The order of the list is the order the trades
// CLOSED in — the validation split relies on it.
struct TrainingExample {
    EntryFeatures features;
    bool win = false;
};

// One line of the experience log, or nothing when it cannot be read.
[[nodiscard]] std::optional<TrainingExample> experienceExampleFrom(const QJsonObject &line);

struct TrainConfig {
    qint32 hidden = 6;
    qint32 epochs = 300;
    double learningRate = 0.05;
    double weightDecay = 1e-4;
    quint32 seed = 20260805U;      // fixed: two runs on one record agree
    double valFraction = 0.2;      // the LAST fifth, in time, is never trained on
    qint32 minSamples = 40;        // below this there is nothing to fit
};

struct TrainResult {
    bool ok = false;
    QString message;   // why not, or what was measured
    BotNet net;
};

// Fit a model to the record. Refuses — with a reason, and without producing
// weights — when the record is too small or has only one kind of outcome in it,
// because a model that cannot separate anything would still score every setup.
[[nodiscard]] TrainResult trainBotNet(const QList<TrainingExample> &examples,
                                      const TrainConfig &cfg = {});

// How much say the network has.
enum class BotNetMode : qint8 {
    Off,      // not consulted at all
    Advise,   // scored and logged; the score decides nothing
    Gate,     // …and a score below the floor refuses the trade
};
[[nodiscard]] QString botNetModeWord(BotNetMode mode);
[[nodiscard]] BotNetMode botNetModeFromWord(const QString &word);

// When the network may refuse, and by how little it may be convinced.
struct NetGateConfig {
    double minScore = 0.5;      // below this the trade is not taken (Gate mode)
    qint32 minSamples = 200;    // …but only once the model has seen this much
    double minAuc = 0.55;       // …and beat a coin flip on unseen trades
};

// The verdict on one candidate. `allow` is true whenever the network is not
// trusted yet — an unproven model never blocks the bot, it only annotates it.
struct NetVerdict {
    bool allow = true;
    bool scored = false;        // false = no usable model, so `score` means nothing
    double score = 0.0;
    QString why;
    QString code;               // "" | "net-score"
};
[[nodiscard]] NetVerdict paperNetGate(const BotNet &net, const EntryFeatures &features,
                                      BotNetMode mode, const NetGateConfig &cfg);

// One line for the window: what the model is, what it measured, and whether it is
// allowed to refuse anything yet.
[[nodiscard]] QString botNetSummary(const BotNet &net, BotNetMode mode,
                                    const NetGateConfig &cfg);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_BOTNET_H
