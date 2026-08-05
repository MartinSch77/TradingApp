#include "domain/BotNet.h"

#include <QDate>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <random>

namespace trading {

namespace {

QList<double> numbersFrom(const QJsonValue &value)
{
    QList<double> out;
    const QJsonArray arr = value.toArray();
    out.reserve(arr.size());
    for (const auto &v : arr) {
        out.append(v.toDouble());
    }
    return out;
}

QJsonArray toArray(const QList<double> &values)
{
    QJsonArray out;
    for (const double v : values) {
        out.append(v);
    }
    return out;
}

double sigmoid(double x)
{
    // Guarded: a saturated pre-activation is a perfectly ordinary answer from a
    // small network, and std::exp on ±800 is not.
    const double clamped = std::clamp(x, -40.0, 40.0);
    return 1.0 / (1.0 + std::exp(-clamped));
}

// The forward pass over an ALREADY standardised row — the training loop's own
// view, where BotNet::score's name lookup and scaling would only get in the way.
double forwargradOutput(const BotNet &net, const QList<double> &x)
{
    double out = net.b2;
    for (qsizetype h = 0; h < net.w1.size(); ++h) {
        double sum = net.b1.at(h);
        for (qsizetype i = 0; i < x.size(); ++i) {
            sum += net.w1.at(h).at(i) * x.at(i);
        }
        out += net.w2.at(h) * std::tanh(sum);
    }
    return sigmoid(out);
}

// One stochastic gradient step on the cross-entropy loss. For a sigmoid output
// dL/dz is simply (p − y), which is why this fits in a dozen lines.
void sgdStep(BotNet &net, const QList<double> &x, double y, double lr, double l2)
{
    QList<double> hidden;
    hidden.reserve(net.w1.size());
    for (qsizetype h = 0; h < net.w1.size(); ++h) {
        double sum = net.b1.at(h);
        for (qsizetype i = 0; i < x.size(); ++i) {
            sum += net.w1.at(h).at(i) * x.at(i);
        }
        hidden.append(std::tanh(sum));
    }
    double z = net.b2;
    for (qsizetype h = 0; h < hidden.size(); ++h) {
        z += net.w2.at(h) * hidden.at(h);
    }
    const double gradOut = sigmoid(z) - y;
    for (qsizetype h = 0; h < net.w1.size(); ++h) {
        const double gradHidden = gradOut * net.w2.at(h) * (1.0 - (hidden.at(h) * hidden.at(h)));
        for (qsizetype i = 0; i < x.size(); ++i) {
            net.w1[h][i] -= lr * ((gradHidden * x.at(i)) + (l2 * net.w1.at(h).at(i)));
        }
        net.b1[h] -= lr * gradHidden;
    }
    for (qsizetype h = 0; h < net.w2.size(); ++h) {
        net.w2[h] -= lr * ((gradOut * hidden.at(h)) + (l2 * net.w2.at(h)));
    }
    net.b2 -= lr * gradOut;
}

// Centre and scale each column by the mean and deviation of the FIRST `cut` rows,
// writing those back for the model to carry: the app has to standardise a live
// candidate exactly as the training set was.
void standardise(QList<QList<double>> &rows, qsizetype cut, QList<double> &mean,
                 QList<double> &sd)
{
    const qsizetype inputs = rows.isEmpty() ? 0 : rows.constFirst().size();
    mean = QList<double>(inputs, 0.0);
    sd = QList<double>(inputs, 0.0);
    for (qsizetype i = 0; i < cut; ++i) {
        for (qsizetype k = 0; k < inputs; ++k) {
            mean[k] += rows.at(i).at(k);
        }
    }
    for (qsizetype k = 0; k < inputs; ++k) {
        mean[k] /= static_cast<double>(cut);
    }
    for (qsizetype i = 0; i < cut; ++i) {
        for (qsizetype k = 0; k < inputs; ++k) {
            const double d = rows.at(i).at(k) - mean.at(k);
            sd[k] += d * d;
        }
    }
    for (qsizetype k = 0; k < inputs; ++k) {
        const double variance = sd.at(k) / static_cast<double>(cut);
        // A constant column carries no information; scaling it by 1 leaves it at
        // zero after centring instead of dividing by nothing.
        sd[k] = (variance > 1e-12) ? std::sqrt(variance) : 1.0;
    }
    for (QList<double> &row : rows) {
        for (qsizetype k = 0; k < inputs; ++k) {
            row[k] = (row.at(k) - mean.at(k)) / sd.at(k);
        }
    }
}

// Xavier-uniform starting weights from a SEEDED generator, so two runs over one
// record produce the same model.
void initWeights(BotNet &net, qsizetype inputs, qint32 hidden, std::mt19937 &rng)
{
    const double limit = std::sqrt(6.0 / static_cast<double>(inputs + hidden));
    std::uniform_real_distribution<double> init(-limit, limit);
    for (qint32 h = 0; h < hidden; ++h) {
        QList<double> row;
        row.reserve(inputs);
        for (qsizetype k = 0; k < inputs; ++k) {
            row.append(init(rng));
        }
        net.w1.append(row);
        net.b1.append(0.0);
        net.w2.append(init(rng));
    }
    net.b2 = 0.0;
}

// What the model is worth on the trades it never saw — the only number that may
// decide whether it is allowed to refuse anything.
void scoreHelgradOut(BotNet &net, const QList<QList<double>> &rows, const QList<double> &labels,
                  qsizetype cut);

// Area under the ROC curve by rank, ties averaged. 0.5 = a coin flip.
double rankAuc(const QList<double> &scores, const QList<double> &labels)
{
    const qsizetype positives = std::count_if(labels.cbegin(), labels.cend(),
                                              [](double y) { return y > 0.5; });
    const qsizetype negatives = labels.size() - positives;
    if ((positives == 0) || (negatives == 0)) {
        return 0.5;   // one-sided: the number would say nothing
    }
    QList<qsizetype> order;
    order.reserve(scores.size());
    for (qsizetype i = 0; i < scores.size(); ++i) {
        order.append(i);
    }
    std::sort(order.begin(), order.end(),
              [&scores](qsizetype a, qsizetype b) { return scores.at(a) < scores.at(b); });
    QList<double> ranks(scores.size(), 0.0);
    qsizetype i = 0;
    while (i < order.size()) {
        qsizetype j = i;
        while (((j + 1) < order.size())
               && qFuzzyCompare(scores.at(order.at(j + 1)), scores.at(order.at(i)))) {
            ++j;
        }
        const double shared = ((static_cast<double>(i) + static_cast<double>(j)) / 2.0) + 1.0;
        for (qsizetype k = i; k <= j; ++k) {
            ranks[order.at(k)] = shared;
        }
        i = j + 1;
    }
    double rankSum = 0.0;
    for (qsizetype k = 0; k < ranks.size(); ++k) {
        if (labels.at(k) > 0.5) {
            rankSum += ranks.at(k);
        }
    }
    const auto p = static_cast<double>(positives);
    return (rankSum - (p * (p + 1.0) / 2.0)) / (p * static_cast<double>(negatives));
}

void scoreHelgradOut(BotNet &net, const QList<QList<double>> &rows, const QList<double> &labels,
                  qsizetype cut)
{
    QList<double> scores;
    QList<double> truth;
    qsizetype hits = 0;
    for (qsizetype i = cut; i < rows.size(); ++i) {
        const double p = forwargradOutput(net, rows.at(i));
        scores.append(p);
        truth.append(labels.at(i));
        if ((p >= 0.5) == (labels.at(i) > 0.5)) {
            ++hits;
        }
    }
    net.valAuc = rankAuc(scores, truth);
    net.valAccuracy = scores.isEmpty()
                          ? 0.0
                          : (static_cast<double>(hits) / static_cast<double>(scores.size()));
}

} // namespace

double BotNet::score(const QHash<QString, double> &inputs) const
{
    if (!ok || features.isEmpty() || w1.isEmpty()) {
        return 0.0;
    }
    QList<double> x;
    x.reserve(features.size());
    for (qsizetype i = 0; i < features.size(); ++i) {
        // An input the caller does not have becomes the training mean, which
        // standardises to zero: "nothing unusual about this one".
        const double raw = inputs.value(features.at(i), mean.value(i, 0.0));
        const double sd = stddev.value(i, 1.0);
        x.append((raw - mean.value(i, 0.0)) / ((sd > 1e-9) ? sd : 1.0));
    }
    double out = b2;
    for (qsizetype h = 0; h < w1.size(); ++h) {
        const QList<double> &row = w1.at(h);
        double sum = b1.value(h, 0.0);
        for (qsizetype i = 0; (i < row.size()) && (i < x.size()); ++i) {
            sum += row.at(i) * x.at(i);
        }
        out += w2.value(h, 0.0) * std::tanh(sum);
    }
    return sigmoid(out);
}

BotNet botNetFromJson(const QJsonObject &obj)
{
    BotNet net;
    if (obj.isEmpty()) {
        net.error = QStringLiteral("no model file");
        return net;
    }
    const QJsonArray featureArr = obj.value(QStringLiteral("features")).toArray();
    for (const auto &v : featureArr) {
        net.features << v.toString();
    }
    net.mean = numbersFrom(obj.value(QStringLiteral("mean")));
    net.stddev = numbersFrom(obj.value(QStringLiteral("stddev")));
    const QJsonArray rows = obj.value(QStringLiteral("w1")).toArray();
    for (const auto &row : rows) {
        net.w1.append(numbersFrom(row));
    }
    net.b1 = numbersFrom(obj.value(QStringLiteral("b1")));
    net.w2 = numbersFrom(obj.value(QStringLiteral("w2")));
    net.b2 = obj.value(QStringLiteral("b2")).toDouble();
    net.samples = static_cast<qint32>(obj.value(QStringLiteral("samples")).toDouble());
    net.valAuc = obj.value(QStringLiteral("valAuc")).toDouble();
    net.valAccuracy = obj.value(QStringLiteral("valAccuracy")).toDouble();
    net.trainedAt = obj.value(QStringLiteral("trainedAt")).toString();

    // The feature list first, because it is the more specific diagnosis: a model
    // trained on a different feature set than this build produces is not wrong
    // about the market, it is answering a different question.
    if (net.features.isEmpty()) {
        net.error = QStringLiteral("model has no feature list");
        return net;
    }
    if (net.features != entryFeatureNames()) {
        net.error = QStringLiteral("model was trained on other features (%1)")
                        .arg(net.features.join(u", "));
        return net;
    }
    // Then the shape, because a model whose weights do not match its feature list
    // would still produce a number — just not a meaningful one.
    const qsizetype n = net.features.size();
    const bool shaped = (net.mean.size() == n) && (net.stddev.size() == n)
                        && !net.w1.isEmpty() && (net.b1.size() == net.w1.size())
                        && (net.w2.size() == net.w1.size());
    if (!shaped) {
        net.error = QStringLiteral("model shape does not line up with its feature list");
        return net;
    }
    const auto misshaped = std::find_if(net.w1.cbegin(), net.w1.cend(),
                                        [n](const QList<double> &row) {
                                            return row.size() != n;
                                        });
    if (misshaped != net.w1.cend()) {
        net.error = QStringLiteral("a hidden unit has %1 weights for %2 features")
                        .arg(misshaped->size())
                        .arg(n);
        return net;
    }
    net.ok = true;
    return net;
}

QJsonObject botNetToJson(const BotNet &net)
{
    QJsonObject obj;
    QJsonArray features;
    for (const QString &name : net.features) {
        features.append(name);
    }
    obj.insert(QStringLiteral("features"), features);
    obj.insert(QStringLiteral("mean"), toArray(net.mean));
    obj.insert(QStringLiteral("stddev"), toArray(net.stddev));
    QJsonArray w1;
    for (const QList<double> &row : net.w1) {
        w1.append(toArray(row));
    }
    obj.insert(QStringLiteral("w1"), w1);
    obj.insert(QStringLiteral("b1"), toArray(net.b1));
    obj.insert(QStringLiteral("w2"), toArray(net.w2));
    obj.insert(QStringLiteral("b2"), net.b2);
    obj.insert(QStringLiteral("samples"), net.samples);
    obj.insert(QStringLiteral("valAuc"), net.valAuc);
    obj.insert(QStringLiteral("valAccuracy"), net.valAccuracy);
    obj.insert(QStringLiteral("trainedAt"), net.trainedAt);
    return obj;
}

std::optional<TrainingExample> experienceExampleFrom(const QJsonObject &line)
{
    const QJsonObject features = line.value(QStringLiteral("features")).toObject();
    if (features.isEmpty() || !line.contains(QStringLiteral("netPnl"))) {
        return std::nullopt;
    }
    TrainingExample example;
    EntryFeatures &f = example.features;
    const auto num = [&features](const char *key, bool *found) {
        const QJsonValue v = features.value(QLatin1String(key));
        *found = *found && v.isDouble();
        return v.toDouble();
    };
    bool complete = true;
    f.confidence = num("confidence", &complete);
    f.volPct = num("volPct", &complete);
    f.stopPct = num("stopPct", &complete);
    f.targetPct = num("targetPct", &complete);
    f.spreadPct = num("spreadPct", &complete);
    f.edgeOverCost = num("edgeOverCost", &complete);
    f.leverage = static_cast<qint32>(num("leverage", &complete));
    f.dir = static_cast<qint32>(num("dir", &complete));
    f.hourUtc = static_cast<qint32>(num("hourUtc", &complete));
    f.dayOfWeek = static_cast<qint32>(num("dayOfWeek", &complete));
    f.aiBacked = num("aiBacked", &complete) > 0.5;
    if (!complete) {
        return std::nullopt;   // a half-read example would teach the wrong thing
    }
    // The label is what the account KEPT: a trade that moved the right way but did
    // not cover its costs is a loss, and has to be learned as one.
    example.win = line.value(QStringLiteral("netPnl")).toDouble() > 0.0;
    return example;
}

TrainResult trainBotNet(const QList<TrainingExample> &examples, const TrainConfig &cfg)
{
    TrainResult out;
    if (examples.size() < cfg.minSamples) {
        out.message = QStringLiteral("only %1 of %2 examples needed — still collecting")
                          .arg(examples.size())
                          .arg(cfg.minSamples);
        return out;
    }
    const qsizetype wins = std::count_if(examples.cbegin(), examples.cend(),
                                         [](const TrainingExample &e) { return e.win; });
    if ((wins == 0) || (wins == examples.size())) {
        out.message = QStringLiteral("every one of the %1 trades ended the same way — "
                                     "there is nothing to separate yet")
                          .arg(examples.size());
        return out;
    }
    // The split is by TIME, never at random: the record is a time series, and a
    // random split would let the model see the future of the very conditions it is
    // then scored on. The AUC below is the number that decides whether it may
    // refuse trades, so it has to be earned on trades it never saw.
    const qsizetype cut = std::max<qsizetype>(1, static_cast<qsizetype>(
                                                     std::llround(static_cast<double>(
                                                                      examples.size())
                                                                  * (1.0 - cfg.valFraction))));
    if (cut >= examples.size()) {
        out.message = QStringLiteral("not enough history for a held-out tail");
        return out;
    }

    const qsizetype inputs = entryFeatureNames().size();
    QList<QList<double>> rows;
    QList<double> labels;
    rows.reserve(examples.size());
    labels.reserve(examples.size());
    for (const TrainingExample &e : examples) {
        rows.append(entryFeatureValues(e.features));
        labels.append(e.win ? 1.0 : 0.0);
    }

    BotNet net;
    net.features = entryFeatureNames();
    // Standardise on the TRAINING part only — the held-out tail must not influence
    // even the scaling, or the "unseen" claim is not quite true.
    standardise(rows, cut, net.mean, net.stddev);
    std::mt19937 rng(cfg.seed);
    initWeights(net, inputs, cfg.hidden, rng);

    QList<qsizetype> order;
    order.reserve(cut);
    for (qsizetype i = 0; i < cut; ++i) {
        order.append(i);
    }
    for (qint32 epoch = 0; epoch < cfg.epochs; ++epoch) {
        std::shuffle(order.begin(), order.end(), rng);
        for (const qsizetype idx : order) {
            sgdStep(net, rows.at(idx), labels.at(idx), cfg.learningRate, cfg.weightDecay);
        }
    }

    net.samples = static_cast<qint32>(cut);
    scoreHelgradOut(net, rows, labels, cut);
    net.trainedAt = QDate::currentDate().toString(Qt::ISODate);
    net.ok = true;

    out.ok = true;
    out.net = net;
    out.message = QStringLiteral("trained on %1 trades, held-out AUC %2, accuracy %3%")
                      .arg(net.samples)
                      .arg(net.valAuc, 0, 'f', 2)
                      .arg(net.valAccuracy * 100.0, 0, 'f', 0);
    return out;
}

QString botNetModeWord(BotNetMode mode)
{
    switch (mode) {
    case BotNetMode::Advise:
        return QStringLiteral("advise");
    case BotNetMode::Gate:
        return QStringLiteral("gate");
    case BotNetMode::Off:
        break;
    }
    return QStringLiteral("off");
}

BotNetMode botNetModeFromWord(const QString &word)
{
    const QString w = word.trimmed().toLower();
    if (w == QStringLiteral("advise")) {
        return BotNetMode::Advise;
    }
    if (w == QStringLiteral("gate")) {
        return BotNetMode::Gate;
    }
    return BotNetMode::Off;
}

NetVerdict paperNetGate(const BotNet &net, const EntryFeatures &features, BotNetMode mode,
                        const NetGateConfig &cfg)
{
    NetVerdict out;
    if (mode == BotNetMode::Off) {
        return out;
    }
    if (!net.ok) {
        out.why = net.error.isEmpty() ? QStringLiteral("no trained model") : net.error;
        return out;   // allow: an absent model is not an opinion
    }
    out.scored = true;
    out.score = net.score(entryFeatureMap(features));
    // Trusted only once it has seen enough trades AND beaten a coin flip on ones it
    // never saw. Until then it rides along and says what it would have said.
    const bool trusted = (net.samples >= cfg.minSamples) && (net.valAuc >= cfg.minAuc);
    if (!trusted) {
        out.why = QStringLiteral("model scores %1 but is not trusted yet (%2 trades, AUC %3)")
                      .arg(out.score, 0, 'f', 2)
                      .arg(net.samples)
                      .arg(net.valAuc, 0, 'f', 2);
        return out;
    }
    if (mode == BotNetMode::Gate && (out.score < cfg.minScore)) {
        out.allow = false;
        out.code = QStringLiteral("net-score");
        out.why = QStringLiteral("the network scores this setup %1, below the %2 floor")
                      .arg(out.score, 0, 'f', 2)
                      .arg(cfg.minScore, 0, 'f', 2);
        return out;
    }
    out.why = QStringLiteral("network score %1").arg(out.score, 0, 'f', 2);
    return out;
}

QString botNetSummary(const BotNet &net, BotNetMode mode, const NetGateConfig &cfg)
{
    if (mode == BotNetMode::Off) {
        return QStringLiteral("Learned model: off (experience is still recorded)");
    }
    if (!net.ok) {
        return QStringLiteral("Learned model: none yet — %1. \"Train from experience\" "
                              "fits one here as soon as the log has trades in it.")
            .arg(net.error);
    }
    const bool trusted = (net.samples >= cfg.minSamples) && (net.valAuc >= cfg.minAuc);
    return QStringLiteral("Learned model: %1 trades, AUC %2, accuracy %3% (trained %4) — %5")
        .arg(net.samples)
        .arg(net.valAuc, 0, 'f', 2)
        .arg(net.valAccuracy * 100.0, 0, 'f', 0)
        .arg(net.trainedAt.isEmpty() ? QStringLiteral("unknown") : net.trainedAt,
             trusted ? QStringLiteral("%1 mode, refusing below %2")
                           .arg(botNetModeWord(mode))
                           .arg(cfg.minScore, 0, 'f', 2)
                     : QStringLiteral("advisory only until %1 trades and AUC %2")
                           .arg(cfg.minSamples)
                           .arg(cfg.minAuc, 0, 'f', 2));
}

} // namespace trading
