// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_CONSOLE_ADVISEVIEW_H
#define TRADINGAPP_CONSOLE_ADVISEVIEW_H

#include "domain/DecisionEngine.h"
#include "domain/TradePlan.h"

#include <QString>
#include <QStringList>

// The one-shot advice report (REQ-F-047), built from PLAIN inputs so every line it can print
// is testable without a network or a terminal — the BotConsoleView discipline. The gathering
// main fills one AdviseInput; this turns it into the verdict and its reasons.
namespace trading::console {

struct AdviseInput {
    QString symbol;
    // The composite row for the instrument (absent = the scan never delivered it).
    bool haveRow = false;
    DecisionRow row;
    // The costed plan (absent = no usable close series).
    bool havePlan = false;
    TradePlan plan;
    double price = 0.0;   // the price the plan was built from (0 = unknown)
    // The nine-reads summary for the two indices; empty lines = not applicable here.
    QStringList readLines;
    // Crowd evidence line (empty = nothing measured) and the optional local model's answer
    // (empty = not asked; a named error is a real answer and is printed as such).
    QString crowdLine;
    bool aiAsked = false;
    QString aiLine;
    // Regime.
    bool vixValid = false;
    double vix = 0.0;
    bool fgValid = false;
    double fearGreed = 50.0;
    QStringList eventLines;
    // Sources that could not be gathered in time, BY NAME — absent is never silent.
    QStringList absentSources;
};

struct AdviseVerdict {
    int exitCode = 3;   // 0 = proposal, 2 = reasoned no-trade, 3 = not enough data
    QString text;
};

[[nodiscard]] AdviseVerdict adviseReport(const AdviseInput &in);

} // namespace trading::console

#endif // TRADINGAPP_CONSOLE_ADVISEVIEW_H
