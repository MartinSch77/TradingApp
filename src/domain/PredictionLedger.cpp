// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/PredictionLedger.h"

#include "domain/Forecasting.h"

#include <QFile>
#include <QJsonDocument>
#include <QTimeZone>

#include <algorithm>
#include <functional>
#include <cmath>

namespace trading {

namespace {

// A resolved row: the call, and what the market then did. The scoring pass works on
// these so that every baseline is measured on IDENTICAL samples — computing one baseline
// over a different subset than another is the classic way to win a comparison on paper.
struct Resolved {
    Prediction call;
    Outcome outcome;
};

// Below this there is nothing to claim. Forty is the same floor the outcome model uses
// (BotConfig::minSamples): enough that a hit rate is not one lucky afternoon, small
// enough to be reachable in a few sessions of scanning.
constexpr qint32 kMinSamplesPerHorizon = 40;
// kMinSamplesPerBucket now lives in the header: the cockpit view states the threshold it is
// short of, and two definitions of the same floor would eventually disagree.
// The strength bands. Deliberately wide: five bands over 100 points keeps each one
// populated in weeks rather than years, and a calibration curve nobody can fill is a
// curve nobody can use.
constexpr qint32 kBucketWidth = 20;

// How far past the horizon a pairing may sit and still be about that horizon. A scan
// cycle is roughly a minute, so the next row after the horizon is normally within one;
// half the horizon again is generous for a slow cycle and still refuses an overnight gap.
qint32 maxElapsedFor(Horizon horizon)
{
    const qint32 minutes = horizonMinutes(horizon);
    return minutes + std::max(2, minutes / 2);
}

// The band a strength falls into, as [low, high).
QPair<qint32, qint32> bandFor(double strength)
{
    const qint32 clamped = static_cast<qint32>(std::clamp(strength, 0.0, 99.999));
    const qint32 low = (clamped / kBucketWidth) * kBucketWidth;
    return {low, low + kBucketWidth};
}

// Every row of `history` that can be resolved at `horizon`, carrying a directional call.
// Rows that stayed out are recorded and loaded — they are the point of the ledger — but
// they cannot contribute to a DIRECTIONAL hit rate, so they are excluded here and
// counted elsewhere.
QList<Resolved> resolveAll(const QList<Prediction> &history, Horizon horizon)
{
    QList<Prediction> sorted = history;
    std::sort(sorted.begin(), sorted.end(),
              [](const Prediction &a, const Prediction &b) { return a.at < b.at; });
    QList<Resolved> out;
    for (qsizetype i = 0; i < sorted.size(); ++i) {
        const Prediction &call = sorted.at(i);
        if ((call.dir == 0) || !call.isValid()) {
            continue;
        }
        const std::optional<Outcome> outcome =
            resolveOutcome(call, horizon, sorted.mid(i + 1));
        if (outcome.has_value()) {
            out.append(Resolved{call, *outcome});
        }
    }
    return out;
}

// One baseline's score: its hit rate over the rows on which it actually HAD a side, and
// how many those were. One helper for all three, so none of them can accidentally be
// scored by different arithmetic — and the denominator is the measurable rows, not all
// rows, because a baseline that was silent on half the sample did not get those wrong.
struct BaselineScore {
    double hitRate = 0.0;
    qint32 samples = 0;
};

BaselineScore baselineOf(const QList<Resolved> &rows,
                         const std::function<qint32(const Resolved &)> &sideOf)
{
    BaselineScore out;
    qint32 hits = 0;
    for (const Resolved &row : rows) {
        const qint32 side = sideOf(row);
        if (side == 0) {
            continue;   // this baseline had no opinion here: not a miss, not a hit
        }
        ++out.samples;
        if (side == row.outcome.actualDir) {
            ++hits;
        }
    }
    if (out.samples > 0) {
        out.hitRate = (static_cast<double>(hits) / static_cast<double>(out.samples)) * 100.0;
    }
    return out;
}

// The calibration curve over the resolved rows.
QList<CalibrationBucket> bucketsOf(const QList<Resolved> &rows)
{
    QList<CalibrationBucket> buckets;
    for (qint32 low = 0; low < 100; low += kBucketWidth) {
        CalibrationBucket bucket;
        bucket.lowStrength = low;
        bucket.highStrength = low + kBucketWidth;
        for (const Resolved &row : rows) {
            if (bandFor(row.call.strength).first != low) {
                continue;
            }
            ++bucket.samples;
            if (row.call.dir == row.outcome.actualDir) {
                ++bucket.hits;
            }
        }
        if (bucket.samples > 0) {
            bucket.hitRate = (static_cast<double>(bucket.hits)
                              / static_cast<double>(bucket.samples))
                             * 100.0;
        }
        buckets.append(bucket);
    }
    return buckets;
}

// The Brier score of the app's own confidence: it claims `strength` percent, and the
// outcome is 1 when the call was right. 0.25 is the score of always answering 50%.
double brierOf(const QList<Resolved> &rows)
{
    if (rows.isEmpty()) {
        return 0.25;
    }
    double total = 0.0;
    for (const Resolved &row : rows) {
        // The strength is evidence, not a probability, so it is mapped onto one the only
        // defensible way: 0 strength claims a coin flip, 100 claims certainty about the
        // side. This mapping is exactly what the calibration curve then measures.
        const double claimed = 0.5 + ((std::clamp(row.call.strength, 0.0, 100.0) / 100.0) * 0.5);
        const double actual = (row.call.dir == row.outcome.actualDir) ? 1.0 : 0.0;
        total += (claimed - actual) * (claimed - actual);
    }
    return total / static_cast<double>(rows.size());
}

} // namespace

qint32 horizonMinutes(Horizon horizon)
{
    switch (horizon) {
    case Horizon::M5:
        return 5;
    case Horizon::M15:
        return 15;
    case Horizon::M60:
        return 60;
    case Horizon::M180:
        break;
    }
    return 180;
}

QString horizonWord(Horizon horizon)
{
    return QStringLiteral("%1 min").arg(horizonMinutes(horizon));
}

QList<Horizon> allHorizons()
{
    return {Horizon::M5, Horizon::M15, Horizon::M60, Horizon::M180};
}

QString regimeWord(Regime regime)
{
    switch (regime) {
    case Regime::Trend:
        return QStringLiteral("trend");
    case Regime::Range:
        return QStringLiteral("range");
    case Regime::HighVolatility:
        return QStringLiteral("high volatility");
    case Regime::EventWindow:
        return QStringLiteral("event window");
    case Regime::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

bool Prediction::isValid() const
{
    return at.isValid() && !symbol.isEmpty() && (price > 0.0);
}

bool CalibrationBucket::trustworthy() const
{
    return samples >= kMinSamplesPerBucket;
}

bool HorizonScore::trustworthy() const
{
    return samples >= kMinSamplesPerHorizon;
}

bool HorizonScore::beatsBaselines() const
{
    if (!trustworthy()) {
        return false;
    }
    // Only the baselines that were measurable get a vote. A baseline with no samples is
    // reported as unmeasurable by headline() and excluded here.
    const bool beatsLong = (alwaysLongSamples == 0) || (hitRate > alwaysLongHitRate);
    const bool beatsPrior = (priorMoveSamples == 0) || (hitRate > priorMoveHitRate);
    const bool beatsVwap = (vwapSideSamples == 0) || (hitRate > vwapSideHitRate);
    // …but a claim to beat "every baseline" when NONE of them could be measured is empty.
    const bool anyMeasured =
        (alwaysLongSamples > 0) || (priorMoveSamples > 0) || (vwapSideSamples > 0);
    return anyMeasured && beatsLong && beatsPrior && beatsVwap;
}

QString HorizonScore::headline() const
{
    if (!trustworthy()) {
        return QStringLiteral("%1: %2 of %3 samples needed — no claim yet")
            .arg(horizonWord(horizon))
            .arg(samples)
            .arg(kMinSamplesPerHorizon);
    }
    // A baseline with no samples is named as unmeasurable rather than printed as 0%.
    const auto baseline = [](const QString &name, double rate, qint32 count) {
        return (count > 0) ? QStringLiteral("%1 %2%% over %3").arg(name).arg(rate, 0, 'f', 1).arg(count)
                           : QStringLiteral("%1 not measurable").arg(name);
    };
    return QStringLiteral("%1: %2% right over %3 calls (Brier %4) · %5 · %6 · %7 — %8")
        .arg(horizonWord(horizon))
        .arg(hitRate, 0, 'f', 1)
        .arg(samples)
        .arg(brier, 0, 'f', 3)
        .arg(baseline(QStringLiteral("always-long"), alwaysLongHitRate, alwaysLongSamples),
             baseline(QStringLiteral("prior move"), priorMoveHitRate, priorMoveSamples),
             baseline(QStringLiteral("VWAP side"), vwapSideHitRate, vwapSideSamples))
        .arg(beatsBaselines() ? QStringLiteral("beats every measured baseline")
                              : QStringLiteral("does NOT beat every measured baseline"));
}

std::optional<Outcome> resolveOutcome(const Prediction &prediction, Horizon horizon,
                                      const QList<Prediction> &later)
{
    if (!prediction.isValid()) {
        return std::nullopt;
    }
    const qint32 wanted = horizonMinutes(horizon);
    const qint32 limit = maxElapsedFor(horizon);
    // The EARLIEST row at or past the horizon: the first honest answer to "where was it
    // after n minutes". Taking the latest available instead would silently lengthen the
    // horizon whenever the ledger happens to be long.
    const Prediction *best = nullptr;
    qint32 bestElapsed = 0;
    for (const Prediction &row : later) {
        if (!row.isValid() || (row.symbol != prediction.symbol)) {
            continue;
        }
        const qint64 minutes = prediction.at.secsTo(row.at) / 60;
        if ((minutes < wanted) || (minutes > limit)) {
            continue;
        }
        const auto elapsed = static_cast<qint32>(minutes);
        if ((best == nullptr) || (elapsed < bestElapsed)) {
            best = &row;
            bestElapsed = elapsed;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    Outcome out;
    out.movePct = ((best->price - prediction.price) / prediction.price) * 100.0;
    out.actualDir = (out.movePct > 0.0) ? 1 : ((out.movePct < 0.0) ? -1 : 0);
    out.elapsedMinutes = bestElapsed;
    return out;
}

HorizonScore scoreHorizon(const QList<Prediction> &history, Horizon horizon)
{
    HorizonScore out;
    out.horizon = horizon;
    const QList<Resolved> rows = resolveAll(history, horizon);
    out.samples = static_cast<qint32>(rows.size());
    if (rows.isEmpty()) {
        return out;
    }
    for (const Resolved &row : rows) {
        if (row.call.dir == row.outcome.actualDir) {
            ++out.hits;
        }
    }
    out.hitRate = (static_cast<double>(out.hits) / static_cast<double>(rows.size())) * 100.0;
    out.brier = brierOf(rows);
    // The three baselines, on exactly these samples — each with the count of rows on
    // which it had an opinion at all.
    const BaselineScore alwaysLong = baselineOf(rows, [](const Resolved &) { return 1; });
    out.alwaysLongHitRate = alwaysLong.hitRate;
    out.alwaysLongSamples = alwaysLong.samples;
    const BaselineScore priorMove =
        baselineOf(rows, [](const Resolved &row) { return row.call.priorMoveDir; });
    out.priorMoveHitRate = priorMove.hitRate;
    out.priorMoveSamples = priorMove.samples;
    const BaselineScore vwapSide =
        baselineOf(rows, [](const Resolved &row) { return row.call.vwapSide; });
    out.vwapSideHitRate = vwapSide.hitRate;
    out.vwapSideSamples = vwapSide.samples;
    out.buckets = bucketsOf(rows);
    return out;
}

QList<HorizonProbability> horizonProbabilities(const Prediction &now,
                                               const QList<Prediction> &history,
                                               const QList<double> &recentBars)
{
    QList<HorizonProbability> out;
    for (const Horizon horizon : allHorizons()) {
        HorizonProbability probability;
        probability.horizon = horizon;

        // How far it can move is a property of the series and needs no record at all:
        // resample the instrument's own recent bars over the horizon. Reported even when
        // the direction is uncalibrated, because "expect ±0.3% over the next 15 minutes"
        // is useful on its own and is not a claim about which way.
        const McOutlook outlook =
            monteCarlo(recentBars, {.price = now.price,
                                    .horizon = horizonMinutes(horizon),
                                    .tpFrac = 0.0,
                                    .slFrac = 0.0,
                                    .paths = 2000,
                                    .seed = 20260806U});
        if (outlook.valid && (now.price > 0.0)) {
            probability.rangeKnown = true;
            probability.p5 = ((outlook.p5 - now.price) / now.price) * 100.0;
            probability.p95 = ((outlook.p95 - now.price) / now.price) * 100.0;
        }

        // The direction, from the record — and only from the record.
        const HorizonScore score = scoreHorizon(history, horizon);
        const QPair<qint32, qint32> band = bandFor(now.strength);
        const auto sameBand = [&band](const CalibrationBucket &candidate) {
            return candidate.lowStrength == band.first;
        };
        const auto found = std::find_if(score.buckets.cbegin(), score.buckets.cend(), sameBand);
        // No such band means no comparable calls, which is the uncalibrated case below.
        const CalibrationBucket bucket =
            (found != score.buckets.cend()) ? *found : CalibrationBucket{};
        probability.samples = bucket.samples;
        if ((now.dir != 0) && bucket.trustworthy()) {
            probability.calibrated = true;
            // The band's measured hit rate is the chance the CALL is right; the question
            // asked is P(up), so a short's number is its complement.
            probability.pUp = (now.dir > 0) ? bucket.hitRate : (100.0 - bucket.hitRate);
        }

        const QString range =
            probability.rangeKnown
                ? QStringLiteral(" · expect %1% to %2%")
                      .arg(probability.p5, 0, 'f', 2)
                      .arg(probability.p95, 0, 'f', 2)
                : QStringLiteral(" · range not measurable");
        probability.sentence =
            probability.calibrated
                ? QStringLiteral("P(up, %1) %2%% from %3 comparable calls%4")
                      .arg(horizonWord(horizon))
                      .arg(probability.pUp, 0, 'f', 0)
                      .arg(probability.samples)
                      .arg(range)
                : QStringLiteral("P(up, %1) UNCALIBRATED — %2 of %3 comparable calls "
                                 "recorded%4")
                      .arg(horizonWord(horizon))
                      .arg(probability.samples)
                      .arg(kMinSamplesPerBucket)
                      .arg(range);
        out.append(probability);
    }
    return out;
}

Regime regimeFor(const RegimeInputs &in)
{
    // Order matters, and it is the order of what can overrule what. A scheduled release
    // outranks every structural read; violent volatility outranks the trend/range
    // question, because at that width the horizon's noise swamps either answer.
    if (in.eventWindow) {
        return Regime::EventWindow;
    }
    if (in.vixValid && (in.vix >= 30.0)) {
        return Regime::HighVolatility;
    }
    if (!in.hurstKnown) {
        return Regime::Unknown;
    }
    // Rescaled-range persistence: above ~0.55 moves continue, below ~0.45 they revert,
    // and the band between them is a random walk that deserves neither label.
    if (in.hurst >= 0.55) {
        return Regime::Trend;
    }
    if (in.hurst <= 0.45) {
        return Regime::Range;
    }
    return Regime::Unknown;
}

QJsonObject predictionToJson(const Prediction &prediction)
{
    QJsonObject out;
    out.insert(QStringLiteral("at"), prediction.at.toUTC().toString(Qt::ISODate));
    out.insert(QStringLiteral("symbol"), prediction.symbol);
    out.insert(QStringLiteral("dir"), prediction.dir);
    out.insert(QStringLiteral("strength"), prediction.strength);
    out.insert(QStringLiteral("measured"), prediction.measured);
    out.insert(QStringLiteral("unknowns"), prediction.unknowns);
    out.insert(QStringLiteral("price"), prediction.price);
    out.insert(QStringLiteral("regime"), regimeWord(prediction.regime));
    out.insert(QStringLiteral("taken"), prediction.taken);
    out.insert(QStringLiteral("refusal"), prediction.refusal);
    out.insert(QStringLiteral("priorMoveDir"), prediction.priorMoveDir);
    out.insert(QStringLiteral("vwapSide"), prediction.vwapSide);
    out.insert(QStringLiteral("strategyVersion"), prediction.strategyVersion);
    return out;
}

std::optional<Prediction> predictionFromJson(const QJsonObject &object)
{
    Prediction out;
    out.at = QDateTime::fromString(object.value(QStringLiteral("at")).toString(), Qt::ISODate);
    out.at.setTimeZone(QTimeZone::UTC);
    out.symbol = object.value(QStringLiteral("symbol")).toString();
    out.dir = object.value(QStringLiteral("dir")).toInt();
    out.strength = object.value(QStringLiteral("strength")).toDouble();
    out.measured = object.value(QStringLiteral("measured")).toInt();
    out.unknowns = object.value(QStringLiteral("unknowns")).toInt();
    out.price = object.value(QStringLiteral("price")).toDouble();
    out.taken = object.value(QStringLiteral("taken")).toBool();
    out.refusal = object.value(QStringLiteral("refusal")).toString();
    out.priorMoveDir = object.value(QStringLiteral("priorMoveDir")).toInt();
    out.vwapSide = object.value(QStringLiteral("vwapSide")).toInt();
    // Absent on a row written before this field existed: toString() on a missing key
    // already yields an empty string, which is exactly "not attributed" — no special
    // casing needed to read an older ledger.
    out.strategyVersion = object.value(QStringLiteral("strategyVersion")).toString();
    const QString regime = object.value(QStringLiteral("regime")).toString();
    for (const Regime candidate : {Regime::Trend, Regime::Range, Regime::HighVolatility,
                                   Regime::EventWindow}) {
        if (regimeWord(candidate) == regime) {
            out.regime = candidate;
        }
    }
    if (!out.isValid()) {
        return std::nullopt;   // a truncated or foreign line: skipped, never guessed at
    }
    return out;
}

bool appendPrediction(const QString &path, const Prediction &prediction)
{
    if (path.isEmpty() || !prediction.isValid()) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray line =
        QJsonDocument(predictionToJson(prediction)).toJson(QJsonDocument::Compact) + '\n';
    const bool written = (file.write(line) == line.size());
    file.close();
    return written;
}

QList<Prediction> loadPredictions(const QString &path)
{
    QList<Prediction> out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            continue;   // a half-written last line costs that line, never the record
        }
        const std::optional<Prediction> row = predictionFromJson(doc.object());
        if (row.has_value()) {
            out.append(*row);
        }
    }
    file.close();
    std::sort(out.begin(), out.end(),
              [](const Prediction &a, const Prediction &b) { return a.at < b.at; });
    return out;
}

} // namespace trading
