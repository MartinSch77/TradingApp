// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_DECISIONLOG_H
#define TRADINGAPP_DOMAIN_DECISIONLOG_H

#include <QDateTime>
#include <QString>

namespace trading {

// The bot's reasoning, per instrument it CONSIDERED, written to a file (REQ-F-029).
//
// Why a file and not just the window: the window's log is in memory and dies with the
// process. The bot is meant to be left running for weeks on a Pi, and the question that
// matters afterwards — "why was GOLD never traded on Tuesday?" — cannot be answered by a
// log that was discarded at the last restart. `prediction-ledger.jsonl` records the same
// decisions for MEASUREMENT (machine-readable, one JSON object per row); this file exists
// to be READ BY A PERSON, one line per instrument, saying what happened and why.
//
// Every instrument that reached a verdict gets a line, including — especially — the ones
// that were refused. A log of trades taken explains nothing about the twenty-five
// instruments the bot looked at and left alone.
struct DecisionNote {
    QDateTime at;
    QString symbol;
    qint32 dir = 0;              // +1 long, -1 short, 0 no side
    double confidence = 0.0;     // the composite's conviction, 0..100
    double leadStrength = 0.0;   // the combined indication's strength, 0..100
    qint32 leadMeasured = 0;     // how many independent reads could be measured
    qint32 leadUnknowns = 0;     // …and how many could not
    bool traded = false;
    QString code;                // refusal code; EMPTY when traded
    QString why;                 // the sentence the deciding rule produced

    // Filled only when traded, so a reader can check the geometry against the reason.
    double stake = 0.0;
    qint32 leverage = 1;
    double fillRate = 0.0;
    double slRate = 0.0;
    double tpRate = 0.0;
    double openCost = 0.0;

    [[nodiscard]] bool isValid() const { return at.isValid() && !symbol.isEmpty(); }
};

// One line, human first. Pure so it can be tested without touching a disk.
[[nodiscard]] QString decisionLine(const DecisionNote &note);

// The header written once when the file is created, so the format explains itself to
// whoever opens it in six months without this source to hand.
[[nodiscard]] QString decisionLogHeader();

// Append one note. Creates the file with its header when absent. Returns false when the
// write failed — a caller may report that, but must never let it stop a trade decision:
// the log describes the bot, it does not govern it.
bool appendDecision(const QString &path, const DecisionNote &note);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_DECISIONLOG_H
