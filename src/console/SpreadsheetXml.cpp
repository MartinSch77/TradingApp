// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpreadsheetXml.h"

namespace trading::console {

namespace {

QString escaped(QString text)
{
    text.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    text.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    text.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    text.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return text;
}

QString cellXml(const QString &value)
{
    bool numeric = false;
    static_cast<void>(value.toDouble(&numeric));
    const QString type = numeric ? QStringLiteral("Number") : QStringLiteral("String");
    return QStringLiteral("<Cell><Data ss:Type=\"%1\">%2</Data></Cell>")
        .arg(type, escaped(value));
}

} // namespace

QString spreadsheetXml(const QList<Sheet> &sheets)
{
    QString out = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<?mso-application progid=\"Excel.Sheet\"?>\n"
        "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\"\n"
        "          xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\">\n");
    for (const Sheet &sheet : sheets) {
        out += QStringLiteral("<Worksheet ss:Name=\"%1\"><Table>\n")
                   .arg(escaped(sheet.name.left(31)));
        for (const QStringList &row : sheet.rows) {
            out += QStringLiteral("<Row>");
            for (const QString &cell : row) {
                out += cellXml(cell);
            }
            out += QStringLiteral("</Row>\n");
        }
        out += QStringLiteral("</Table></Worksheet>\n");
    }
    out += QStringLiteral("</Workbook>\n");
    return out;
}

} // namespace trading::console
