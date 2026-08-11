// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_CONSOLE_SPREADSHEETXML_H
#define TRADINGAPP_CONSOLE_SPREADSHEETXML_H

#include <QList>
#include <QString>
#include <QStringList>

// A minimal SpreadsheetML 2003 writer (REQ-F-048): ONE self-contained XML file Excel and
// LibreOffice open natively, multiple named sheets, no zip library and no new third-party
// dependency — the deliberate trade against real .xlsx, documented in docs/crowd-ai.md's
// style: what ships is what the supply chain already covers. Cells that parse as numbers
// are typed Number so spreadsheet maths works on them; everything else stays String.
namespace trading::console {

struct Sheet {
    QString name;                 // worksheet tab name (Excel limit: 31 chars, enforced)
    QList<QStringList> rows;      // first row is typically the header
};

[[nodiscard]] QString spreadsheetXml(const QList<Sheet> &sheets);

} // namespace trading::console

#endif // TRADINGAPP_CONSOLE_SPREADSHEETXML_H
