// Unit tests for the trade-script parser and execution semantics (DES-DOM-SCRIPT).

#include "domain/TradeScript.h"

#include <QtTest/QtTest>

#include <array>

using namespace trading;

namespace {

// A fully-specified line, the template most cases below start from. A
// function, not a namespace-scope QString: a static QString with dynamic
// initialization is a cert-err58-cpp finding.
QString fullLine()
{
    return QStringLiteral(
        "GOLD.24-7; SELL @ 2500; FROM 2026-08-03 09:00; TO 2026-08-07 17:30; "
        "SIGNALS; AMOUNT 250; SL 30; TP 60; TRAILING; LEV 20");
}

} // namespace

class TestTradeScript : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-SCRIPT-001 @design DES-DOM-SCRIPT
    // @relation(REQ-F-028, scope=function)
    void TS_SCRIPT_001_parsesFullLine()
    {
        const ScriptParseResult r = parseTradeScript(fullLine());
        QVERIFY(r.ok);
        QCOMPARE(r.entries.size(), 1);
        const ScriptEntry &e = r.entries.first();
        QCOMPARE(e.symbol, QStringLiteral("GOLD.24-7"));
        QVERIFY(!e.isBuy);
        QCOMPARE(e.trigger, 2500.0);
        QCOMPARE(e.from, QDateTime(QDate(2026, 8, 3), QTime(9, 0)));
        QCOMPARE(e.to, QDateTime(QDate(2026, 8, 7), QTime(17, 30)));
        QVERIFY(e.requireSignals);
        QCOMPARE(e.amount, 250.0);
        QCOMPARE(e.slAmount, 30.0);
        QCOMPARE(e.tpAmount, 60.0);
        QVERIFY(e.trailing);
        QCOMPARE(e.leverage, 20);
        QCOMPARE(e.lineNumber, 1);
    }

    //! @tstid TS-SCRIPT-002 @design DES-DOM-SCRIPT
    // @relation(REQ-F-028, scope=function)
    void TS_SCRIPT_002_minimalLineCommentsAndDefaults()
    {
        // Comments (whole-line and trailing), blank lines, a date-only FROM,
        // and every optional field left out.
        const ScriptParseResult r = parseTradeScript(QStringLiteral(
            "# my script\n"
            "\n"
            "SPX500; BUY @ 6100; AMOUNT 500; FROM 2026-08-03  # entry on the dip\n"));
        QVERIFY(r.ok);
        QCOMPARE(r.entries.size(), 1);
        const ScriptEntry &e = r.entries.first();
        QVERIFY(e.isBuy);
        QCOMPARE(e.trigger, 6100.0);
        QCOMPARE(e.from, QDateTime(QDate(2026, 8, 3), QTime(0, 0)));
        QVERIFY(!e.to.isValid());         // no expiry
        QVERIFY(!e.requireSignals);
        QCOMPARE(e.slAmount, 0.0);        // both legs off
        QCOMPARE(e.tpAmount, 0.0);
        QVERIFY(!e.trailing);
        QCOMPARE(e.leverage, 1);          // the safe default
        QCOMPARE(e.lineNumber, 3);        // 1-based, counting comment + blank
    }

    //! @tstid TS-SCRIPT-003 @design DES-DOM-SCRIPT
    // @relation(REQ-F-028, scope=function)
    void TS_SCRIPT_003_allOrNothingRejection()
    {
        // One good line + one bad line: NOTHING loads, and the error names the
        // line, the reason and the content (REQ-F-028's half-loaded-script rule).
        const ScriptParseResult r = parseTradeScript(QStringLiteral(
            "SPX500; BUY @ 6100; AMOUNT 500\n"
            "GOLD.24-7; HOLD @ 2500; AMOUNT 100\n"));
        QVERIFY(!r.ok);
        QVERIFY(r.entries.isEmpty());
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.errors.first().contains(QStringLiteral("line 2")));
        QVERIFY(r.errors.first().contains(QStringLiteral("BUY or SELL")));
    }

    //! @tstid TS-SCRIPT-004 @design DES-DOM-SCRIPT
    // @relation(REQ-F-028, scope=function)
    void TS_SCRIPT_004_fieldValidation()
    {
        // Each rejected line reports its own reason.
        struct Case {
            QString line;
            QString reason;
        };
        const std::array<Case, 10> cases = {
            Case{QStringLiteral("SPX500; BUY @ 6100"), QStringLiteral("AMOUNT is required")},
            {QStringLiteral("SPX500; BUY @ 0; AMOUNT 10"), QStringLiteral("positive")},
            {QStringLiteral("; BUY @ 5; AMOUNT 10"), QStringLiteral("symbol is empty")},
            {QStringLiteral("SPX500; BUY 6100; AMOUNT 10"), QStringLiteral("expected BUY @")},
            {QStringLiteral("SPX500; BUY @ 5; AMOUNT 10; LEV 0"), QStringLiteral("LEV")},
            {QStringLiteral("SPX500; BUY @ 5; AMOUNT 10; BOGUS 1"), QStringLiteral("unknown field")},
            {QStringLiteral("SPX500; BUY @ 5; AMOUNT 10; AMOUNT 20"), QStringLiteral("duplicate")},
            {QStringLiteral("SPX500; BUY @ 5; AMOUNT 10; FROM 2026-08-05; TO 2026-08-04"),
             QStringLiteral("FROM is after TO")},
            {QStringLiteral("SPX500; BUY @ 5; AMOUNT 10; FROM yesterday"), QStringLiteral("yyyy-MM-dd")},
            {QStringLiteral("SPX500; BUY @ 5; AMOUNT 10; SIGNALS now"), QStringLiteral("takes no value")},
        };
        for (const auto &c : cases) {
            const ScriptParseResult r = parseTradeScript(c.line);
            QVERIFY2(!r.ok, qPrintable(c.line));
            QVERIFY2(r.errors.first().contains(c.reason),
                     qPrintable(c.line + QStringLiteral(" -> ") + r.errors.first()));
        }
    }

    //! @tstid TS-SCRIPT-005 @design DES-DOM-SCRIPT
    // @relation(REQ-F-028, scope=function)
    void TS_SCRIPT_005_leverageSnapsToNextLower()
    {
        const QList<qint32> offered = {1, 2, 5, 10, 20};
        QCOMPARE(snapLeverage(10, offered), 10);  // offered as-is
        QCOMPARE(snapLeverage(15, offered), 10);  // next lower
        QCOMPARE(snapLeverage(3, offered), 2);
        QCOMPARE(snapLeverage(100, offered), 20); // above all -> highest offered
        QCOMPARE(snapLeverage(1, {2, 5}), 2);     // below all -> lowest offered
        QCOMPARE(snapLeverage(7, {}), 7);         // nothing known -> unchanged
    }

    //! @tstid TS-SCRIPT-006 @design DES-DOM-SCRIPT
    // @relation(REQ-F-028, scope=function)
    void TS_SCRIPT_006_windowGatesResting()
    {
        ScriptEntry e = parseTradeScript(fullLine()).entries.first();
        const ScriptSignalState agree{-1, -1, true};  // both favour the SELL

        QVERIFY(!scriptEntryShouldRest(e, QDateTime(QDate(2026, 8, 3), QTime(8, 59)), agree));
        QVERIFY(scriptEntryShouldRest(e, QDateTime(QDate(2026, 8, 3), QTime(9, 0)), agree));
        QVERIFY(scriptEntryShouldRest(e, QDateTime(QDate(2026, 8, 7), QTime(17, 30)), agree));
        const QDateTime after(QDate(2026, 8, 7), QTime(17, 31));
        QVERIFY(!scriptEntryShouldRest(e, after, agree));
        QVERIFY(scriptEntryExpired(e, after));       // final: never re-placed

        // No window at all: rests immediately and never expires.
        e.from = QDateTime();
        e.to = QDateTime();
        e.requireSignals = false;
        QVERIFY(scriptEntryShouldRest(e, QDateTime::currentDateTime(), {}));
        QVERIFY(!scriptEntryExpired(e, QDateTime::currentDateTime()));
    }

    //! @tstid TS-SCRIPT-007 @design DES-DOM-SCRIPT
    // @relation(REQ-F-028, scope=function)
    void TS_SCRIPT_007_signalsFlagNeedsBothSourcesAndConfiguredAi()
    {
        ScriptEntry e = parseTradeScript(fullLine()).entries.first();  // SELL, SIGNALS
        const QDateTime inWindow(QDate(2026, 8, 4), QTime(12, 0));

        QVERIFY(scriptEntryShouldRest(e, inWindow, {-1, -1, true}));   // both agree
        QVERIFY(!scriptEntryShouldRest(e, inWindow, {-1, 1, true}));   // AI disagrees
        QVERIFY(!scriptEntryShouldRest(e, inWindow, {1, -1, true}));   // ensemble disagrees
        QVERIFY(!scriptEntryShouldRest(e, inWindow, {-1, 0, true}));   // AI neutral
        // Unconfigured AI = no recommendation = the flagged entry waits
        // rather than trade on half the evidence (REQ-F-028).
        QVERIFY(!scriptEntryShouldRest(e, inWindow, {-1, -1, false}));

        // Without the flag the same states place regardless of the sources.
        e.requireSignals = false;
        QVERIFY(scriptEntryShouldRest(e, inWindow, {1, 1, false}));
    }
};

QTEST_GUILESS_MAIN(TestTradeScript)
#include "tst_tradescript.moc"
