// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_LEADSIGNAL_H
#define TRADINGAPP_DOMAIN_LEADSIGNAL_H

#include "domain/IndexConfluence.h"
#include "domain/Models.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

// ONE indication, assembled from every independent read the app can obtain, for the
// question a leveraged trade actually asks: is there enough agreement RIGHT NOW to
// justify size (REQ-F-036)?
//
// The premise is the one REQ-F-035 already states and this makes operational: an
// index is not a thing that moves on its own, and no single indicator predicts it.
// What predicts it — as far as anything does — is several INDEPENDENT things saying
// the same thing at the same time:
//
//   * the constituent field: are the ten heavyweights moving together, and is the
//     move broad or carried by one name (`heavyweightPulse`);
//   * the independent reads: futures leadership, volatility DIRECTION, the 10-year
//     yield, participation, the opening range (`confluenceFor`);
//   * the clock: an opening burst, a macro release, a policy window or the quiet
//     middle of the session are different worlds for the same setup;
//   * the regime: an elevated VIX and an imminent high-impact event both mean the
//     next print can overrule any structure that exists now.
//
// THREE RULES KEEP IT HONEST, and they are the reason this is worth having at all:
//
//  1. An input that could not be MEASURED contributes nothing — never a zero, never
//     a neutral guess. `unknowns` counts them and the window shows them, so a
//     "strong" signal built from two available feeds is impossible to mistake for
//     one built from six.
//  2. Strength is capped by what was measured. Half the inputs missing means at most
//     half the strength, whatever the available ones say.
//  3. It NEVER raises the leverage the risk rules allow. `suggestedLeverage` is an
//     upper bound derived from conviction, to be intersected with the per-trade risk
//     budget, the bucket cap and the instrument's own ladder — never a floor.
namespace trading {

// How much size the reads justify. The wording is deliberately about EVIDENCE, not
// about confidence: "strong" means many independent things agree, not that the trade
// will work.
enum class LeadGrade : quint8 {
    None = 0,    // nothing measurable, or the reads contradict each other
    Weak,        // a lean, not a case
    Fair,        // several independent reads agree
    Strong       // broad agreement across independent reads AND a broad constituent field
};

[[nodiscard]] QString leadGradeWord(LeadGrade grade);

// What the reads say, and what they were built from.
struct LeadSignal {
    qint32 dir = 0;             // +1 long, −1 short, 0 = no side
    double strength = 0.0;      // 0..100, and CAPPED by how much was measurable
    LeadGrade grade = LeadGrade::None;
    qint32 measured = 0;        // inputs that could be read
    qint32 unknowns = 0;        // inputs that could not — shown, never counted
    // The highest leverage the EVIDENCE justifies. An upper bound only: the risk
    // budget, the correlation-bucket cap and the instrument's ladder all still apply,
    // and the smallest of them wins.
    qint32 suggestedLeverage = 1;
    QStringList reasons;        // one line per input, including the unmeasurable ones
    QString headline;           // the one sentence the window shows

    [[nodiscard]] bool actionable() const { return (dir != 0) && (grade >= LeadGrade::Fair); }
};

// Everything the signal is computed from. Anything left at its default is UNKNOWN and
// is reported as such rather than assumed benign — the distinction this whole module
// exists to preserve.
struct LeadInputs {
    QString symbol;                 // the index instrument the signal is for
    IndexReads reads;               // REQ-F-035's independent reads
    HeavyweightPulse pulse;         // its own index's constituent field
    QDateTime now;                  // for the session phase; invalid = unknown clock
    // The regime, as the decision engine already measures it.
    bool vixValid = false;
    double vix = 0.0;
    bool eventRisk = false;         // a high-impact release is imminent
    // The volatility term structure. Never a direction — an inverted curve reduces
    // what any structure justifies, exactly as an elevated level does.
    TermStructure term;
    // The app's own composite for this instrument, which is what the signal is asked
    // to CONFIRM or contradict. 0 = no call, in which case the signal reports the
    // side the reads themselves lean to.
    qint32 compositeDir = 0;
    double compositeConfidence = 0.0;
};

// The signal. Deterministic, pure, and safe on every input — an empty LeadInputs
// yields grade None with every reason naming what was missing.
[[nodiscard]] LeadSignal leadSignal(const LeadInputs &in);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_LEADSIGNAL_H
