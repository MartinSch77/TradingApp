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

// The portfolio proposal as SHEETS, from plain inputs (REQ-F-048) — pure and testable; the
// gathering main fills one input and writes spreadsheetXml(portfolioReportSheets(in)).
namespace trading::console {

struct PortfolioCandidate {
    QString symbol;
    DecisionRow row;    // the composite and its per-source reads
    TradePlan plan;     // the COSTED verdict; only actionable plans should be handed in
};

struct PortfolioReportInput {
    bool portfolioKnown = false;       // false = no credentials/timeout: proposals over a
                                       // flat book, and the report SAYS so
    QList<Position> positions;
    double cash = 0.0;
    QString currency;
    QList<PortfolioCandidate> candidates;
    QStringList absentSources;         // named absents for the Data health sheet
    QString generatedAt;               // caller-supplied stamp (fixed in tests)
};

[[nodiscard]] QList<Sheet> portfolioReportSheets(const PortfolioReportInput &in);

} // namespace trading::console

#endif // TRADINGAPP_CONSOLE_PORTFOLIOREPORT_H
