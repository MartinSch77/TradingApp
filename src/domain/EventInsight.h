// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_EVENTINSIGHT_H
#define TRADINGAPP_DOMAIN_EVENTINSIGHT_H

#include "domain/Models.h"

#include <QString>

#include <optional>

// Heuristic read-outs for macro-economic calendar events: how an event tends to
// move the traded instrument and a plain-language description of what it is.
// Pure text/direction logic — the UI maps the direction to colours.
namespace trading {

// Extract the leading numeric value from a feed string like "0.3%", "-0.2%",
// "215K" (suffix ignored — fine for comparing a forecast against its previous).
// nullopt when the string carries no parsable number.
[[nodiscard]] std::optional<double> parseNum(const QString &s);

struct ImpactGuess {
    QString text;   // e.g. "bullish (strong) — stronger data"
    qint32 dir = 0; // +1 bullish, -1 bearish, 0 uncertain/volatile
};

// Best-effort estimate of how an economic event tends to move the traded instrument,
// from the event type and the forecast-vs-previous change. This is a heuristic, not a
// forecast — real reactions depend on the surprise vs. the actual print.
[[nodiscard]] ImpactGuess guessImpact(const EconomicEvent &e);

// A concrete, advisory-only activity proposal around a calendar event: which side
// to trade (if any), whether to act before or after the print, and why — derived
// from the event class, its impact rating and the forecast-vs-previous change.
// Advisory only: it is never executed automatically (REQ-N-005).
struct EventProposal {
    bool actionable = false;  // false → the safe stance is to stay out
    qint32 dir = 0;           // +1 buy / -1 sell / 0 none
    QString action;           // "BUY" / "SELL" / "STAY OUT"
    QString timing;           // when to act, e.g. "after the 14:30 print confirms"
    QString rationale;        // the forecast-vs-previous + impact reasoning
};
[[nodiscard]] EventProposal proposeActivity(const EconomicEvent &e);

// Plain-language explanation of what a calendar event measures and why it moves
// the traded instrument, keyed off the same title cues as guessImpact()
// (unemployment is tested before the general jobs bucket, matching that ordering).
// `symbol` names the instrument in the few asset-specific sentences.
[[nodiscard]] QString eventAbout(const EconomicEvent &e, const QString &symbol);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_EVENTINSIGHT_H
