// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/LeadSignal.h"

#include "domain/PaperTrader.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace trading {

namespace {

// Each contributing input, as it is scored. `known` is what keeps an absent feed from
// looking like a neutral opinion.
struct Contribution {
    bool known = false;
    qint32 dir = 0;      // the side this input supports
    double weight = 0.0; // its NOMINAL weight — what the input is worth when readable
    // What it actually CONTRIBUTES when it agrees. Normally the same as `weight`, but
    // an input can be discounted for being weak evidence of its own kind (a field
    // whose move is carried by one name). The two are separate on purpose: discounting
    // both sides of the ratio would cancel out and change nothing, which is exactly
    // the bug this split fixes.
    double credit = 0.0;
    QString reason;
};

// The nine independent reads (REQ-F-035), each already carrying whether it could be
// measured. Their weights are equal on purpose: this app has no evidence that one of
// them predicts better than another, and inventing a ranking would be a story rather
// than a measurement.
QList<Contribution> readContributions(const IndexReads &reads)
{
    const auto one = [](const Read &r, const QString &name, double weight) {
        Contribution c;
        c.known = r.known;
        c.dir = r.dir;
        c.weight = weight;
        c.credit = weight;
        c.reason = r.known ? QStringLiteral("%1: %2").arg(name, r.detail)
                           : QStringLiteral("%1: not measurable").arg(name);
        return c;
    };
    constexpr double kPerRead = 8.0;
    return {one(reads.futuresLead, QStringLiteral("futures lead"), kPerRead),
            one(reads.futuresMomentum, QStringLiteral("futures momentum"), kPerRead),
            one(reads.volatility, QStringLiteral("volatility direction"), kPerRead),
            one(reads.yields, QStringLiteral("US 10-year"), kPerRead),
            one(reads.curve, QStringLiteral("yield curve"), kPerRead),
            one(reads.participation, QStringLiteral("participation"), kPerRead),
            one(reads.aboveVwap, QStringLiteral("above own VWAP"), kPerRead),
            one(reads.upDownVolume, QStringLiteral("up/down volume"), kPerRead),
            one(reads.structure, QStringLiteral("opening range"), kPerRead)};
}

// The constituent field: how broad the move is, and whether one name is carrying it.
// This is the input the other five cannot provide — they describe the market around
// the index, this one describes the index's own members.
Contribution fieldContribution(const HeavyweightPulse &pulse)
{
    Contribution c;
    c.weight = 20.0;   // the heaviest single input: it is the most direct evidence
    c.credit = c.weight;
    if (pulse.isEmpty()) {
        c.reason = QStringLiteral("constituent field: no prices read");
        return c;
    }
    c.known = true;
    const double share = static_cast<double>(pulse.up) / static_cast<double>(pulse.measured);
    // A field is only evidence when it LEANS: seven of ten one way, or three of ten.
    if ((share >= 0.7) && (pulse.averageChangePct > 0.0)) {
        c.dir = 1;
    } else if ((share <= 0.3) && (pulse.averageChangePct < 0.0)) {
        c.dir = -1;
    }
    // One name carrying the whole move is the classic false positive: the average is
    // strong, the field is not. Halve the weight when the leader alone accounts for
    // more than the whole average across the field.
    const double leaderShare =
        (std::abs(pulse.averageChangePct) > 0.0)
            ? (std::abs(pulse.leaderChangePct)
               / (std::abs(pulse.averageChangePct) * static_cast<double>(pulse.measured)))
            : 0.0;
    const bool carried = leaderShare > 0.5;
    if (carried) {
        c.credit *= 0.5;   // the same field, worth half as evidence
    }
    c.reason = QStringLiteral("constituent field: %1 of %2 up, average %3%%4")
                   .arg(pulse.up)
                   .arg(pulse.measured)
                   .arg(pulse.averageChangePct, 0, 'f', 2)
                   .arg(carried ? QStringLiteral(" — but %1 is carrying it")
                                      .arg(pulse.leader)
                                : QString());
    return c;
}

// The clock. Not a direction of its own: a phase can only REDUCE what the other reads
// justify, because the same structure means less when the next print can erase it.
double phaseFactor(const QString &symbol, const QDateTime &now, QString &reason)
{
    if (!now.isValid()) {
        reason = QStringLiteral("session phase: clock unknown");
        return 1.0;
    }
    switch (sessionPhaseFor(symbol, now)) {
    case SessionPhase::OpeningChaos:
        reason = QStringLiteral("session phase: opening chaos — the range is still forming");
        return 0.3;
    case SessionPhase::PolicyWindow:
        reason = QStringLiteral("session phase: policy window — a statement outranks structure");
        return 0.2;
    case SessionPhase::DataWindow:
        reason = QStringLiteral("session phase: macro release imminent");
        return 0.5;
    case SessionPhase::OpeningBurst:
        reason = QStringLiteral("session phase: opening burst — the readable part of the open");
        return 1.0;
    case SessionPhase::PowerHour:
        reason = QStringLiteral("session phase: power hour");
        return 1.0;
    case SessionPhase::ClosingBurst:
        reason = QStringLiteral("session phase: closing burst — positioning, not information");
        return 0.6;
    case SessionPhase::Normal:
        break;
    }
    reason = QStringLiteral("session phase: ordinary session");
    return 1.0;
}

// The regime, which also only ever reduces: an elevated VIX and an imminent release
// are both statements that the next few minutes can overrule everything measured.
double regimeFactor(const LeadInputs &in, QStringList &reasons)
{
    double factor = 1.0;
    if (in.vixValid) {
        if (in.vix >= 30.0) {
            factor *= 0.6;
            reasons << QStringLiteral("regime: VIX %1 — size for a market that moves twice as far")
                           .arg(in.vix, 0, 'f', 1);
        } else if (in.vix >= 25.0) {
            factor *= 0.8;
            reasons << QStringLiteral("regime: VIX %1 elevated").arg(in.vix, 0, 'f', 1);
        } else {
            reasons << QStringLiteral("regime: VIX %1").arg(in.vix, 0, 'f', 1);
        }
    } else {
        reasons << QStringLiteral("regime: VIX not measurable");
    }
    if (in.eventRisk) {
        factor *= 0.5;
        reasons << QStringLiteral("regime: a high-impact release is imminent");
    }
    // An inverted term structure is the market paying more for the next nine days than
    // for the next three months. Whatever structure exists now is being priced as
    // temporary, so it justifies less size — and note this is INDEPENDENT of the level
    // above: a 16 VIX with an inverted curve is a different tape from a calm 16.
    if (in.term.known && in.term.inverted) {
        factor *= 0.7;
    }
    // A default-constructed term structure carries no sentence of its own; say the
    // honest thing rather than printing an empty reason.
    reasons << QStringLiteral("regime: %1")
                   .arg(in.term.detail.isEmpty()
                            ? QStringLiteral("term structure: not measurable")
                            : in.term.detail);
    return factor;
}

// The side to judge the inputs against: the app's own call when it has one, otherwise
// whichever way the MEASURABLE inputs lean. Judging against a side the app is not taking
// would produce a number nobody can act on, and an unmeasurable input contributes nothing
// to the lean either — the same rule the scoring follows.
qint32 sideToJudge(const QList<Contribution> &contributions, qint32 compositeDir)
{
    if (compositeDir != 0) {
        return compositeDir;
    }
    const double lean =
        std::accumulate(contributions.cbegin(), contributions.cend(), 0.0,
                        [](double sum, const Contribution &c) {
                            return c.known ? (sum + (c.credit * static_cast<double>(c.dir))) : sum;
                        });
    return (lean > 0.0) ? 1 : ((lean < 0.0) ? -1 : 0);
}

// What the inputs add up to for one side. `out` collects the reasons and the
// measured/unknown counts as the tally walks them, so the signal can report what it was
// built from rather than only the number it produced.
struct Tally {
    double agreeing = 0.0;
    double against = 0.0;
    double measurableWeight = 0.0;
    double totalWeight = 0.0;
};

Tally tallyFor(const QList<Contribution> &contributions, qint32 side, LeadSignal &out)
{
    Tally tally;
    for (const Contribution &c : contributions) {
        tally.totalWeight += c.weight;
        out.reasons << c.reason;
        if (!c.known) {
            ++out.unknowns;
            continue;
        }
        ++out.measured;
        tally.measurableWeight += c.weight;
        if (c.dir == side) {
            tally.agreeing += c.credit;
        } else if (c.dir != 0) {
            tally.against += c.weight;   // a contradiction counts in full
        }
    }
    return tally;
}

// Leverage the EVIDENCE justifies — an upper bound, never a floor, and deliberately
// conservative: the risk budget, the bucket cap and the instrument's ladder are all
// applied after this and the smallest wins.
qint32 leverageFor(LeadGrade grade)
{
    switch (grade) {
    case LeadGrade::Strong:
        return 10;
    case LeadGrade::Fair:
        return 5;
    case LeadGrade::Weak:
        return 2;
    case LeadGrade::None:
        break;
    }
    return 1;
}

} // namespace

QString leadGradeWord(LeadGrade grade)
{
    switch (grade) {
    case LeadGrade::Strong:
        return QStringLiteral("strong");
    case LeadGrade::Fair:
        return QStringLiteral("fair");
    case LeadGrade::Weak:
        return QStringLiteral("weak");
    case LeadGrade::None:
        break;
    }
    return QStringLiteral("none");
}

LeadSignal leadSignal(const LeadInputs &in)
{
    LeadSignal out;
    QList<Contribution> contributions = readContributions(in.reads);
    contributions.append(fieldContribution(in.pulse));

    const qint32 side = sideToJudge(contributions, in.compositeDir);
    const Tally tally = tallyFor(contributions, side, out);

    if ((side == 0) || (out.measured == 0)) {
        out.headline = QStringLiteral("No usable read: %1 of %2 inputs measurable")
                           .arg(out.measured)
                           .arg(out.measured + out.unknowns);
        out.suggestedLeverage = leverageFor(LeadGrade::None);
        return out;
    }

    // Net agreement over what was measurable, then scaled by HOW MUCH was measurable.
    // Rule 2: half the inputs missing means at most half the strength, whatever the
    // available ones say.
    const double net = std::max(0.0, tally.agreeing - tally.against);
    const double share =
        (tally.measurableWeight > 0.0) ? (net / tally.measurableWeight) : 0.0;
    const double coverage =
        (tally.totalWeight > 0.0) ? (tally.measurableWeight / tally.totalWeight) : 0.0;

    QString phaseReason;
    const double phase = phaseFactor(in.symbol, in.now, phaseReason);
    out.reasons << phaseReason;
    QStringList regimeReasons;
    const double regime = regimeFactor(in, regimeReasons);
    out.reasons += regimeReasons;

    out.dir = side;
    out.strength = std::clamp(share * coverage * phase * regime * 100.0, 0.0, 100.0);
    // The grade is about EVIDENCE: "strong" requires broad agreement AND most of the
    // inputs actually measured — a 100% share of two available reads is not strong.
    // The counts are a MAJORITY of the ten inputs, not fixed numbers: adding reads must
    // raise the bar for "strong", or a grade earned from five of five would be handed
    // out for five of ten.
    constexpr qint32 kStrongMinMeasured = 7;
    constexpr qint32 kFairMinMeasured = 4;
    if ((out.strength >= 55.0) && (out.measured >= kStrongMinMeasured)) {
        out.grade = LeadGrade::Strong;
    } else if ((out.strength >= 35.0) && (out.measured >= kFairMinMeasured)) {
        out.grade = LeadGrade::Fair;
    } else if (out.strength > 0.0) {
        out.grade = LeadGrade::Weak;
    }
    out.suggestedLeverage = leverageFor(out.grade);

    out.headline =
        QStringLiteral("%1 %2: strength %3 of 100 (%4), %5 of %6 inputs measured — max x%7")
            .arg(in.symbol,
                 (out.dir > 0) ? QStringLiteral("LONG") : QStringLiteral("SHORT"))
            .arg(out.strength, 0, 'f', 0)
            .arg(leadGradeWord(out.grade))
            .arg(out.measured)
            .arg(out.measured + out.unknowns)
            .arg(out.suggestedLeverage);
    return out;
}

} // namespace trading
