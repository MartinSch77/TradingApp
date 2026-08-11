// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_CONSOLE_PORTFOLIOREPORT_H
#define TRADINGAPP_CONSOLE_PORTFOLIOREPORT_H

#include "SpreadsheetXml.h"
#include "domain/DecisionEngine.h"
#include "domain/Models.h"
#include "domain/TradePlan.h"

#include <QList>
#include <QString>

// The portfolio report as SHEETS, from plain inputs (REQ-F-048) — pure and testable. This is
// HOLDINGS-CENTRIC: it analyses the instruments the account actually holds and recommends what
// to do with each (hold / add / reduce / exit), rather than proposing catalogue buys. The
// gathering main fills one input per held position and writes spreadsheetXml(sheets).
namespace trading::console {

// One held position with whatever signal could be gathered for it. A holding with no data
// feed (a stock the app cannot price) carries haveSignal=false and is reported as such — an
// analysis that cannot see an instrument must say so, not invent a verdict.
struct HoldingSignal {
    Position position;
    bool haveSignal = false;   // a composite (from candles) was computed
    DecisionRow row;           // the technical/composite read, when haveSignal
    bool havePlan = false;
    TradePlan plan;            // the costed plan, when candles allowed one
    QString note;              // e.g. "no candle feed" when haveSignal is false
};

struct PortfolioReportInput {
    bool portfolioKnown = false;   // false = no credentials / not delivered: say so
    double cash = 0.0;
    QString currency;
    QList<HoldingSignal> holdings;
    QStringList absentSources;     // named absents for the Data health sheet
    QString generatedAt;           // caller-supplied stamp (fixed in tests)
};

[[nodiscard]] QList<Sheet> portfolioReportSheets(const PortfolioReportInput &in);

} // namespace trading::console

#endif // TRADINGAPP_CONSOLE_PORTFOLIOREPORT_H
