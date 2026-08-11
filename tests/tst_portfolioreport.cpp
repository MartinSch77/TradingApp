// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "console/PortfolioReport.h"
#include "console/SpreadsheetXml.h"

#include <QtTest/QtTest>

#include <algorithm>

using namespace trading::console;

// The holdings-centric portfolio report and the spreadsheet writer (REQ-F-048), pinned
// headless: what to do with each HELD instrument, and an honest file.
namespace {

// A held long with a signal; the no-signal / short cases are built inline where needed.
HoldingSignal holding(const QString &symbol, double amount, qint32 signalDir, double confidence)
{
    HoldingSignal h;
    h.position.symbol = symbol;
    h.position.amount = amount;
    h.position.isBuy = true;
    h.haveSignal = true;
    h.row.symbol = symbol;
    h.row.dir = signalDir;
    h.row.confidence = confidence;
    h.row.composite = confidence / 100.0 * signalDir;
    return h;
}

QStringList rowOf(const QList<Sheet> &sheets, const QString &name, const QString &firstCell)
{
    for (const Sheet &s : sheets) {
        if (s.name != name) {
            continue;
        }
        const auto it =
            std::find_if(s.rows.cbegin(), s.rows.cend(), [&firstCell](const QStringList &row) {
                return !row.isEmpty() && (row.first() == firstCell);
            });
        if (it != s.rows.cend()) {
            return *it;
        }
    }
    return {};
}

} // namespace

class TestPortfolioReport : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-ADV-003 @design DES-CON-PORTFOLIO
    // @relation(REQ-F-048, scope=function)
    void TS_ADV_003_eachHoldingGetsAVerdictFromItsSignalAndItsSide()
    {
        PortfolioReportInput in;
        in.portfolioKnown = true;
        in.cash = 1000.0;
        in.currency = QStringLiteral("USD");
        in.generatedAt = QStringLiteral("2026-08-11T09:00:00Z");
        // A long the signal SUPPORTS, a long the signal OPPOSES, and one with no data feed.
        HoldingSignal noData;
        noData.position.symbol = QStringLiteral("SOMESTOCK");
        noData.position.amount = 500.0;
        noData.position.isBuy = true;
        noData.note = QStringLiteral("no data feed");
        in.holdings = {holding(QStringLiteral("AAPL"), 2000.0, 1, 62.0),
                       holding(QStringLiteral("TSLA"), 1000.0, -1, 55.0), noData};

        const QList<Sheet> sheets = portfolioReportSheets(in);
        QCOMPARE(sheets.size(), 4);

        // A supporting signal on a long → HOLD/ADD; an opposing one → REDUCE/EXIT; no data →
        // an explicit "hold (no signal)" that names the reason, never an invented verdict.
        const QStringList aapl = rowOf(sheets, QStringLiteral("Holdings"), QStringLiteral("AAPL"));
        QVERIFY(aapl.at(5).contains(QStringLiteral("ADD")) || aapl.at(5) == QStringLiteral("HOLD"));
        QVERIFY(aapl.at(6).contains(QStringLiteral("supports")));
        const QStringList tsla = rowOf(sheets, QStringLiteral("Holdings"), QStringLiteral("TSLA"));
        QVERIFY(tsla.at(5).contains(QStringLiteral("REDUCE")));
        QVERIFY(tsla.at(6).contains(QStringLiteral("opposes")));
        const QStringList none =
            rowOf(sheets, QStringLiteral("Holdings"), QStringLiteral("SOMESTOCK"));
        QVERIFY(none.at(5).contains(QStringLiteral("no signal")));
        QVERIFY(none.at(6).contains(QStringLiteral("no data feed")));

        // The Concentration sheet carries per-bucket invested sums; Data health counts how
        // many holdings had a signal (2 of 3 here).
        const QStringList health =
            rowOf(sheets, QStringLiteral("Data health"), QStringLiteral("with a signal"));
        QCOMPARE(health.at(1), QStringLiteral("2"));
    }

    //! @tstid TS-ADV-004 @design DES-CON-PORTFOLIO
    // @relation(REQ-F-048, scope=function)
    void TS_ADV_004_theFileIsHonestAboutAbsenceAndTheXmlIsWellFormed()
    {
        // No portfolio: the concentration sheet says unavailable rather than inventing zeros.
        PortfolioReportInput bare;
        bare.portfolioKnown = false;
        bare.generatedAt = QStringLiteral("2026-08-11T09:00:00Z");
        const QList<Sheet> sheets = portfolioReportSheets(bare);
        bool unavailable = false;
        for (const Sheet &s : sheets) {
            for (const QStringList &row : s.rows) {
                unavailable = unavailable
                              || (!row.isEmpty()
                                  && row.first().contains(QStringLiteral("portfolio unavailable")));
            }
        }
        QVERIFY(unavailable);

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
