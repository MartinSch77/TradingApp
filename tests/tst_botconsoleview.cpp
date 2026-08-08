// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The bot-console front end's text, tested WITHOUT a terminal (REQ-F-029, DES-CON-BOT).
//
// The console shows the same money figures the GUI does, so it can be wrong in the same
// ways: a stale mark rendered as live, an absent constituent shown as flat, a table that
// reshuffles between refreshes. A terminal screenshot proves none of that is right. These
// tests do.

#include "console/BotConsoleView.h"

#include <QtTest/QtTest>

using namespace trading;
using namespace trading::console;

class TestBotConsoleView : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-CON-001 @design DES-CON-BOT
    // @relation(REQ-F-029, scope=function)
    //
    // The header carries the two numbers the user asked for — booked P/L and invested — on
    // ONE line, with the sign explicit so a loss reads as a loss without colour.
    void TS_CON_001_theHeaderLeadsWithPnlAndInvested()
    {
        PaperStats s;
        s.realized = -19.22;
        s.invested = 4200.0;
        s.equity = 49980.78;
        s.cash = 45780.78;
        s.openTrades = 3;
        s.closedTrades = 7;
        s.winRate = 42.0;

        const QString header = consoleHeader(s, BotAiMode::Lead, /*armed=*/true);
        QVERIFY(header.contains(QStringLiteral("P/L -19.22 EUR")));
        QVERIFY(header.contains(QStringLiteral("invested 4200.00 EUR")));
        QVERIFY(header.contains(QStringLiteral("3 open")));
        QVERIFY(header.contains(QStringLiteral("ARMED")));
        // One line — a header that wraps is not a header.
        QVERIFY(!header.contains(QChar(u'\n')));
    }

    //! @tstid TS-CON-002 @design DES-CON-BOT
    // @relation(REQ-F-029, REQ-F-035, scope=function)
    //
    // The heavyweight chart encodes direction by BAR SIDE and a signed number, never colour,
    // and an ABSENT constituent is dashed rather than drawn flat — a flat bar would read as
    // "unchanged" for a name whose data simply has not arrived.
    void TS_CON_002_heavyBarsEncodeDirectionWithoutColourAndShowAbsentAsAbsent()
    {
        const QList<HeavyMove> names{
            {QStringLiteral("NVDA"), 2.0, true},    // biggest riser → full-scale bar
            {QStringLiteral("AAPL"), -1.0, true},   // faller → bar on the LEFT of centre
            {QStringLiteral("MSFT"), 0.0, false},   // absent
        };
        const QStringList lines = consoleHeavyBars(QStringLiteral("NSDQ100"), names, /*width=*/10);

        // A title plus one row each.
        QCOMPARE(lines.size(), qsizetype(4));
        QVERIFY(lines.at(0).contains(QStringLiteral("NSDQ100")));

        // NVDA rose: fill is to the RIGHT of the centre bar, and the number is signed +.
        const QString &nvda = lines.at(1);
        QVERIFY(nvda.contains(QStringLiteral("+2.00")));
        QVERIFY(nvda.indexOf(QChar(u'█')) > nvda.indexOf(QChar(u'│')));

        // AAPL fell: fill is to the LEFT of the centre bar.
        const QString &aapl = lines.at(2);
        QVERIFY(aapl.contains(QStringLiteral("-1.00")));
        QVERIFY(aapl.indexOf(QChar(u'█')) < aapl.indexOf(QChar(u'│')));

        // MSFT is absent: an em dash and NO bar, never a flat 0.00%.
        const QString &msft = lines.at(3);
        QVERIFY(msft.trimmed().endsWith(QChar(u'—')));
        QVERIFY(!msft.contains(QChar(u'█')));
        QVERIFY(!msft.contains(QStringLiteral("0.00")));
    }

    //! @tstid TS-CON-003 @design DES-CON-BOT
    // @relation(REQ-F-029, scope=function)
    //
    // The open book keeps a STABLE row per position — sorted by id, the open sequence — so a
    // trade does not jump rows between refreshes on a screen meant to be static. Net is shown
    // beside the costs, because a position green on price can be red once carry is counted.
    void TS_CON_003_openTradesKeepAStableRowPerPosition()
    {
        PaperTrade a;
        a.id = 2;
        a.symbol = QStringLiteral("NSDQ100");
        a.isBuy = true;
        a.stake = 3000.0;
        a.leverage = 5;
        a.openRate = 24000.0;
        a.markRate = 24120.0;
        a.markLive = true;
        a.openTime = QDateTime(QDate(2026, 8, 8), QTime(9, 30));

        PaperTrade b = a;
        b.id = 1;
        b.symbol = QStringLiteral("SPX500");
        b.isBuy = false;

        // The index of the first line containing `needle`, or -1 — an algorithm, not a raw
        // loop, so cppcheck's useStlAlgorithm stays quiet and the .at() below is provably safe.
        const auto rowWith = [](const QStringList &rows, const QString &needle) -> qsizetype {
            const auto it = std::find_if(rows.cbegin(), rows.cend(),
                                         [&](const QString &l) { return l.contains(needle); });
            return (it == rows.cend()) ? -1 : std::distance(rows.cbegin(), it);
        };

        // Given out of order; the view must still show id 1 before id 2.
        const QStringList lines = consoleOpenTrades({a, b});
        const qsizetype spx = rowWith(lines, QStringLiteral("SPX500"));
        const qsizetype ndx = rowWith(lines, QStringLiteral("NSDQ100"));
        QVERIFY(spx > 0 && ndx > 0);
        QVERIFY(spx < ndx);   // id 1 (SPX500) above id 2 (NSDQ100), whatever order they arrived

        // Side is shown by an arrow AND a word, never colour alone.
        QVERIFY(lines.at(spx).contains(QStringLiteral("SELL")));
        QVERIFY(lines.at(ndx).contains(QStringLiteral("BUY")));

        // The empty book says so rather than showing a bare header.
        const QStringList empty = consoleOpenTrades({});
        QVERIFY(std::any_of(empty.cbegin(), empty.cend(),
                            [](const QString &l) { return l.contains(QStringLiteral("none open")); }));
    }

    //! @tstid TS-CON-004 @design DES-CON-BOT
    // @relation(REQ-F-029, scope=function)
    //
    // Closed trades are newest-first and NAME the exit rule, so a reader connects a loss to
    // the rule that caused it — the same reason the record is decomposed by exit reason.
    void TS_CON_004_closedTradesAreNewestFirstAndNameTheExitRule()
    {
        PaperClosedTrade older;
        older.id = 1;
        older.symbol = QStringLiteral("OIL.24-7");
        older.netPnl = -71.35;
        older.reason = CloseReason::SignalFade;
        older.closeTime = QDateTime(QDate(2026, 8, 8), QTime(9, 27));

        PaperClosedTrade newer = older;
        newer.id = 2;
        newer.symbol = QStringLiteral("NSDQ100");
        newer.netPnl = 42.10;
        newer.reason = CloseReason::TakeProfit;
        newer.closeTime = QDateTime(QDate(2026, 8, 8), QTime(10, 5));

        const QStringList lines = consoleClosedTrades({older, newer}, /*limit=*/10);
        const auto rowWith = [](const QStringList &rows, const QString &needle) -> qsizetype {
            const auto it = std::find_if(rows.cbegin(), rows.cend(),
                                         [&](const QString &l) { return l.contains(needle); });
            return (it == rows.cend()) ? -1 : std::distance(rows.cbegin(), it);
        };
        // Newest (NSDQ100, 10:05) appears above older (OIL, 09:27).
        const qsizetype iN = rowWith(lines, QStringLiteral("NSDQ100"));
        const qsizetype iO = rowWith(lines, QStringLiteral("OIL.24-7"));
        QVERIFY(iN > 0);
        QVERIFY(iO > 0);
        QVERIFY(iN < iO);
        // The exit rule is named on the row, and the loss carries its sign.
        const QString &oilRow = lines.at(iO);
        QVERIFY(oilRow.contains(QStringLiteral("-71.35")));
        QVERIFY(!closeReasonWord(CloseReason::SignalFade).isEmpty());
        QVERIFY(oilRow.contains(closeReasonWord(CloseReason::SignalFade)));

        const QStringList none = consoleClosedTrades({}, 10);
        QVERIFY(std::any_of(none.cbegin(), none.cend(), [](const QString &l) {
            return l.contains(QStringLiteral("none closed"));
        }));
    }

    //! @tstid TS-CON-005 @design DES-CON-BOT
    // @relation(REQ-F-029, scope=function)
    //
    // The two indices are drawn SIDE BY SIDE: each output row carries a cell from BOTH
    // columns, and the right column starts at the same offset on every row so a shorter
    // column does not drag it out of alignment.
    void TS_CON_005_indicesAreDrawnSideBySide()
    {
        const QList<HeavyMove> a{{QStringLiteral("NVDA"), 1.0, true},
                                 {QStringLiteral("AAPL"), -0.5, true}};
        const QList<HeavyMove> b{{QStringLiteral("MSFT"), 0.8, true}};   // shorter column

        const QStringList rows = trading::console::consoleHeavyBarsSideBySide(
            QStringLiteral("SPX500"), a, QStringLiteral("NSDQ100"), b, 8);

        // Both titles are on the FIRST row — side by side, not stacked.
        QVERIFY(rows.at(0).contains(QStringLiteral("SPX500")));
        QVERIFY(rows.at(0).contains(QStringLiteral("NSDQ100")));
        // The left index appears before the right on that row.
        QVERIFY(rows.at(0).indexOf(QStringLiteral("SPX500"))
                < rows.at(0).indexOf(QStringLiteral("NSDQ100")));

        // The right column is aligned: its cell starts at the same absolute offset on every
        // row. The title carries no indent while a data row is indented two spaces WITHIN its
        // cell (the single-column format), so MSFT sits exactly two past the NSDQ100 title —
        // which is the proof the cells line up rather than drift.
        const qsizetype titleOffset = rows.at(0).indexOf(QStringLiteral("NSDQ100"));
        const qsizetype msftRow = std::distance(
            rows.cbegin(),
            std::find_if(rows.cbegin(), rows.cend(),
                         [](const QString &l) { return l.contains(QStringLiteral("MSFT")); }));
        QVERIFY(msftRow < rows.size());
        QCOMPARE(rows.at(msftRow).indexOf(QStringLiteral("MSFT")), titleOffset + 2);

        // The left column still has its own rows past where the right ran out (NVDA, AAPL).
        QVERIFY(std::any_of(rows.cbegin(), rows.cend(),
                            [](const QString &l) { return l.contains(QStringLiteral("AAPL")); }));
    }
};

QTEST_MAIN(TestBotConsoleView)
#include "tst_botconsoleview.moc"
