// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_PREDICTIONLEDGER_H
#define TRADINGAPP_DOMAIN_PREDICTIONLEDGER_H

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// The record that turns a strength score into a PROBABILITY, and the only thing that
// honestly can (REQ-F-037).
//
// The app can already say "the evidence is 62 of 100 for a long". It cannot say "67%
// chance of being higher in fifteen minutes" from that number, and printing a percent
// sign on a weighted sum would be the single most dishonest thing this codebase could
// do: a probability is a CLAIM ABOUT FREQUENCY, and the only way to earn it is to
// record what was predicted, wait, and count.
//
// Four design decisions carry the whole module:
//
//  1. EVERY evaluation is recorded, including the ones that decided to stay out. A
//     ledger of the trades actually taken measures the SELECTION, not the signal — it
//     cannot see the calls that were right and skipped, and it inherits every bias of
//     the gate in front of it. The refusal code is recorded with the row.
//  2. Outcomes are resolved by PAIRING rows of the same instrument: the ledger is its
//     own price history, so measuring what happened next needs no extra feed and no
//     separate store that could disagree with it. A pairing that is too far away to be
//     the horizon asked about is refused rather than stretched.
//  3. Until there are enough resolved samples in a bucket, the answer is "uncalibrated"
//     and NO number is offered. This is the same discipline `paperLiveReadiness` uses
//     for real money: report the unmet threshold, never a placeholder.
//  4. Every score is reported next to BASELINES computed on the same samples. A 58%
//     directional hit rate is worthless if "always long" scored 61% over the same
//     minutes, and that comparison is the difference between a measurement and a boast.
namespace trading {

// The sample floor a calibration BAND must clear before a probability may be printed at all.
// Public because the VIEW has to state the threshold it is short of — "UNCALIBRATED, 3 of 15"
// is the honest message, and hardcoding 15 in the presentation layer would be a second source
// of truth that drifts from this one. Deliberately fewer than the per-horizon floor, because
// a band is a narrower question — but "fewer" is still not "one", and a band below this
// reports itself untrustworthy.
inline constexpr qint32 kMinSamplesPerBucket = 15;

// The horizons the ledger scores at. Minutes, because that is the scale at which a CFD
// with a spread can be traded at all — a seconds-scale forecast would be measuring a
// cost this app cannot avoid.
enum class Horizon : quint8 { M5 = 0, M15, M60, M180 };

[[nodiscard]] qint32 horizonMinutes(Horizon horizon);
[[nodiscard]] QString horizonWord(Horizon horizon);
// All four, in order — so a caller cannot iterate a subset by accident.
[[nodiscard]] QList<Horizon> allHorizons();

// What kind of market the prediction was made in. Reported with every row because the
// same evidence means different things in a trend and in a range, and a hit rate
// averaged across both hides which one it came from.
enum class Regime : quint8 {
    Unknown = 0,       // not measurable — never guessed
    Trend,             // persistent: moves continue more often than they revert
    Range,             // mean-reverting: moves get given back
    HighVolatility,    // wide enough that the horizon's noise swamps the signal
    EventWindow        // a scheduled release owns the next print
};

[[nodiscard]] QString regimeWord(Regime regime);

// Everything measured about a market at the moment of one decision, plus the decision.
struct Prediction {
    QDateTime at;                 // when the call was made (UTC)
    QString symbol;
    qint32 dir = 0;               // +1 long, −1 short, 0 = no call / stayed out
    double strength = 0.0;        // the combined indication's 0..100
    qint32 measured = 0;          // inputs that could be read
    qint32 unknowns = 0;          // inputs that could not
    double price = 0.0;           // the mark the call was made at
    Regime regime = Regime::Unknown;
    bool taken = false;           // did a position actually open?
    QString refusal;              // the stable refusal code when it did not

    // The baselines, recorded AT DECISION TIME rather than reconstructed afterwards.
    // Reconstructing a baseline later is how a comparison quietly becomes flattering:
    // the same series that produced the signal would be used to produce its rival, with
    // hindsight about which bars mattered.
    qint32 priorMoveDir = 0;      // the previous five minutes' direction
    qint32 vwapSide = 0;          // +1 above the session VWAP, −1 below, 0 unknown

    [[nodiscard]] bool isValid() const;
};

// What actually happened after one prediction, at one horizon.
struct Outcome {
    double movePct = 0.0;         // signed move of the instrument over the horizon
    qint32 actualDir = 0;         // its sign
    qint32 elapsedMinutes = 0;    // the REAL gap, which is never exactly the horizon
};

// The outcome of `prediction` at `horizon`, measured against `later` — rows for the SAME
// instrument, any order. Nothing when the ledger does not (yet) contain a row far enough
// ahead, or when the nearest candidate is too far away to be about this horizon at all:
// resolving a 5-minute call against a row from the next session would manufacture a
// result out of an overnight gap.
[[nodiscard]] std::optional<Outcome> resolveOutcome(const Prediction &prediction,
                                                    Horizon horizon,
                                                    const QList<Prediction> &later);

// One strength band, and how often calls in it were right. This is the calibration
// curve: the thing that makes "67%" mean 67%.
struct CalibrationBucket {
    qint32 lowStrength = 0;       // inclusive
    qint32 highStrength = 0;      // exclusive (100 for the top band)
    qint32 samples = 0;
    qint32 hits = 0;
    double hitRate = 0.0;         // 0..100, of the resolved samples in this band
    [[nodiscard]] bool trustworthy() const;
};

// How the app's own directional call scored at one horizon — and how three baselines
// scored on THE SAME resolved samples.
//
// The baselines are deliberately the cheap ones a critic would reach for. "Always long"
// is the one that embarrasses most equity-index models, because indices drift up. A
// random baseline is NOT measured: its expected hit rate is exactly 50% by construction,
// so measuring it would add sampling noise and no information.
struct HorizonScore {
    Horizon horizon = Horizon::M5;
    qint32 samples = 0;           // resolved rows that carried a directional call
    qint32 hits = 0;
    double hitRate = 0.0;         // 0..100
    // Mean squared error of the calibrated probability against the outcome, 0..1. The
    // reference is 0.25 — what an honest "50%, always" scores — and anything above it
    // means the confidence is worse than saying nothing.
    double brier = 0.25;
    // Each baseline carries the number of rows on which it HAD a side. A baseline that
    // could not be computed (no session VWAP for an index CFD, whose candles carry no
    // volume) must not be scored at 0% and counted as beaten — that would turn a missing
    // measurement into a victory, which is the same lie the reads refuse to tell.
    double alwaysLongHitRate = 0.0;
    qint32 alwaysLongSamples = 0;
    double priorMoveHitRate = 0.0;
    qint32 priorMoveSamples = 0;
    double vwapSideHitRate = 0.0;
    qint32 vwapSideSamples = 0;
    QList<CalibrationBucket> buckets;

    // Enough resolved samples to be worth reading at all.
    [[nodiscard]] bool trustworthy() const;
    // Did the app's own call beat every baseline that could actually be MEASURED? The
    // question the whole ledger exists to answer, and it is allowed to answer "no".
    // An unmeasurable baseline is excluded and named, never treated as beaten.
    [[nodiscard]] bool beatsBaselines() const;
    [[nodiscard]] QString headline() const;
};

// P(up) at one horizon: either a MEASURED frequency or an explicit refusal to guess.
struct HorizonProbability {
    Horizon horizon = Horizon::M5;
    bool calibrated = false;      // false = not enough resolved samples in this band
    double pUp = 50.0;            // only meaningful when calibrated
    qint32 samples = 0;
    // The expected range over the horizon, from the instrument's own recent bars. This
    // is available even when the direction is not calibrated — how far it can move is a
    // property of the series, not a claim about which way.
    bool rangeKnown = false;
    double p5 = 0.0;
    double p95 = 0.0;
    QString sentence;             // what the window shows, in words
};

// The three (plus one) horizon answers for one live prediction, given the record so far.
// `history` is every row for this instrument, oldest or newest first — it is sorted here.
[[nodiscard]] QList<HorizonProbability> horizonProbabilities(const Prediction &now,
                                                             const QList<Prediction> &history,
                                                             const QList<double> &recentBars);

// The score at one horizon over `history`, resolving every row that can be resolved.
[[nodiscard]] HorizonScore scoreHorizon(const QList<Prediction> &history, Horizon horizon);

// The regime label, from the reads the app already computes. `hurst` is the persistence
// exponent (Forecasting::hurstExponent), `vix` the level, and the two flags what the
// session clock says. Unknown inputs yield Unknown rather than a comfortable "Range".
struct RegimeInputs {
    bool hurstKnown = false;
    double hurst = 0.5;
    bool vixValid = false;
    double vix = 0.0;
    bool eventWindow = false;
};
[[nodiscard]] Regime regimeFor(const RegimeInputs &in);

// --- persistence -------------------------------------------------------------
// One row, as one JSON object — the on-disk format is JSONL, exactly like the bot's
// experience log, so the two can be read by the same tooling.
[[nodiscard]] QJsonObject predictionToJson(const Prediction &prediction);
[[nodiscard]] std::optional<Prediction> predictionFromJson(const QJsonObject &object);

// Append one row to `path`, creating it if needed. Returns false when the row could not
// be written — a ledger that silently loses rows would quietly bias every number
// computed from it, so the caller is told.
[[nodiscard]] bool appendPrediction(const QString &path, const Prediction &prediction);
// Every readable row of `path`, oldest first. Unreadable lines are SKIPPED rather than
// aborting the load: a truncated last line (a kill during a write) must not cost the
// entire record.
[[nodiscard]] QList<Prediction> loadPredictions(const QString &path);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_PREDICTIONLEDGER_H
