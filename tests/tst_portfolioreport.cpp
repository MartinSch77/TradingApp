// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "console/PortfolioReport.h"
#include "console/SpreadsheetXml.h"

#include <QtTest/QtTest>

using namespace trading::console;

// The portfolio proposal's sheets and the spreadsheet writer (REQ-F-048), pinned headless.
namespace {

PortfolioCandidate candidate(const QString &symbol, double confidence)
{
    PortfolioCandidate c;
    c.symbol = symbol;
    c.row.symbol = symbol;
    c.row.composite = confidence / 100.0;
    c.row.confidence = confidence;
    c.plan.valid = true;
    c.plan.dir = 1;
    c.plan.verdict = QStringLiteral("BUY");
    c.plan.confidence = confidence;
    c.plan.leverage = 5;
    c.plan.slRate = 90.0;
    c.plan.tpRate = 115.0;
    c.plan.pWin = 0.45;
    return c;
}

QStringList sheetCell(const QList<Sheet> &sheets, const QString &name, qsizetype row)
{
    for (const Sheet &s : sheets) {
        if (s.name == name) {
            return s.rows.value(row);
        }
    }
    return {};
}

} // namespace

class TestPortfolioReport : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-ADV-003 @design DES-CON-ADVISE
    // @relation(REQ-F-048, scope=function)
    void TS_ADV_003_theRankingRespectsWhatTheBookAlreadyHolds()
    {
        PortfolioReportInput in;
        in.portfolioKnown = true;
        in.cash = 1000.0;
        in.currency = QStringLiteral("USD");
        // A book concentrated in equity indices: 4000 of 5000 invested in that bucket.
        Position heavy;
        heavy.symbol = QStringLiteral("SPX500");
        heavy.amount = 4000.0;
        Position light;
        light.symbol = QStringLiteral("GOLD");
        light.amount = 1000.0;
        in.positions = {heavy, light};
        // The stronger candidate sits in the CROWDED bucket; the weaker one is diversifying.
        in.candidates = {candidate(QStringLiteral("NSDQ100"), 60.0),
                         candidate(QStringLiteral("EURUSD"), 45.0)};
        in.generatedAt = QStringLiteral("2026-08-10T20:00:00Z");

        const QList<Sheet> sheets = portfolioReportSheets(in);
        QCOMPARE(sheets.size(), 4);

        // Concentration DEMOTES with the reason named: 60-confidence NSDQ100 in a bucket
        // holding 80% of invested money ranks BELOW the diversifying 45-confidence EURUSD.
        const QStringList first = sheetCell(sheets, QStringLiteral("Proposal"), 1);
        const QStringList second = sheetCell(sheets, QStringLiteral("Proposal"), 2);
        QCOMPARE(first.at(1), QStringLiteral("EURUSD"));
        QCOMPARE(second.at(1), QStringLiteral("NSDQ100"));
        QVERIFY(second.at(10).contains(QStringLiteral("equity-index")));
        QVERIFY(second.at(10).contains(QStringLiteral("80%")));

        // The advisory stake is bounded by the cash that EXISTS (6% of equity would be 360,
        // cash is 1000 — the smaller of the two rules, never more than is there).
        QVERIFY(first.at(7).toDouble() <= in.cash);

        // The portfolio sheet carries the positions, the cash and the bucket sums.
        bool bucketRow = false;
        for (const Sheet &s : sheets) {
            if (s.name == QStringLiteral("Portfolio")) {
                for (const QStringList &row : s.rows) {
                    bucketRow = bucketRow
                                || (row.value(0).startsWith(QStringLiteral("bucket:"))
                                    && (row.value(2).toDouble() > 0.0));
                }
            }
        }
        QVERIFY(bucketRow);
    }

    //! @tstid TS-ADV-004 @design DES-CON-ADVISE
    // @relation(REQ-F-048, scope=function)
    void TS_ADV_004_theFileIsHonestAboutAbsenceAndTheXmlIsWellFormed()
    {
        // No portfolio: the report ranks over a flat book and SAYS so.
        PortfolioReportInput in;
        in.portfolioKnown = false;
        in.candidates = {candidate(QStringLiteral("GOLD"), 50.0)};
        in.absentSources << QStringLiteral("web ratings");
        in.generatedAt = QStringLiteral("2026-08-10T20:00:00Z");
        const QList<Sheet> sheets = portfolioReportSheets(in);
        bool flatNote = false;
        bool absentNamed = false;
        for (const Sheet &s : sheets) {
            for (const QStringList &row : s.rows) {
                flatNote = flatNote
                           || row.value(0).contains(QStringLiteral("portfolio unavailable"));
                absentNamed = absentNamed
                              || ((row.value(0) == QStringLiteral("absent"))
                                  && row.value(1).contains(QStringLiteral("web ratings")));
            }
        }
        QVERIFY(flatNote);
        QVERIFY(absentNamed);

        // The writer: numbers typed as numbers, text escaped, tab names bounded — the file
        // must survive a symbol like "S&P<500>" without becoming invalid XML.
        Sheet sheet;
        sheet.name = QStringLiteral("A very long worksheet name beyond Excel's limit");
        sheet.rows.append({QStringLiteral("S&P<500>"), QStringLiteral("42.5")});
        const QString xml = spreadsheetXml({sheet});
        QVERIFY(xml.contains(QStringLiteral("S&amp;P&lt;500&gt;")));
        QVERIFY(xml.contains(QStringLiteral("ss:Type=\"Number\">42.5")));
        QVERIFY(xml.contains(QStringLiteral("mso-application progid=\"Excel.Sheet\"")));
        QVERIFY(!xml.contains(QStringLiteral("A very long worksheet name beyond Excel's limit")));
    }
};

QTEST_GUILESS_MAIN(TestPortfolioReport)
#include "tst_portfolioreport.moc"
