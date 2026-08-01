// Unit tests for the calendar-event heuristics (DES-DOM-EVT).

#include "domain/EventInsight.h"

#include <QtTest/QtTest>

using namespace trading;

class TestEventInsight : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-EVT-001 @design DES-DOM-EVT
    // @relation(REQ-F-020, scope=function)
    void TS_EVT_001_parseNumFormats()
    {
        QCOMPARE(parseNum(QStringLiteral("0.3%")), std::optional<double>(0.3));
        QCOMPARE(parseNum(QStringLiteral("-0.2%")), std::optional<double>(-0.2));
        QCOMPARE(parseNum(QStringLiteral("215K")), std::optional<double>(215.0));
        QVERIFY(!parseNum(QStringLiteral("n/a")).has_value());
    }

    //! @tstid TS-EVT-002 @design DES-DOM-EVT
    // @relation(REQ-F-020, scope=function)
    void TS_EVT_002_impactGuessShape()
    {
        EconomicEvent e;
        e.title = QStringLiteral("CPI m/m");
        e.country = QStringLiteral("USD");
        e.impact = QStringLiteral("High");
        e.forecast = QStringLiteral("0.5%");
        e.previous = QStringLiteral("0.2%");
        const ImpactGuess g = guessImpact(e);
        QVERIFY(!g.text.isEmpty());
        QVERIFY((g.dir >= -1) && (g.dir <= 1));
        // The explainer names the instrument it is scoped to.
        const QString about = eventAbout(e, QStringLiteral("SPX500"));
        QVERIFY(!about.isEmpty());
    }

    //! @tstid TS-EVT-003 @design DES-DOM-EVT
    // @relation(REQ-F-023, scope=function)
    void TS_EVT_003_activityProposal()
    {
        // High-impact event, hotter inflation forecast than previous: bearish
        // direction, and the entry belongs AFTER the print (gap/whipsaw risk).
        EconomicEvent cpi;
        cpi.title = QStringLiteral("CPI m/m");
        cpi.impact = QStringLiteral("High");
        cpi.forecast = QStringLiteral("0.5%");
        cpi.previous = QStringLiteral("0.2%");
        cpi.when = QDateTime::currentDateTime().addSecs(3600);
        const EventProposal hot = proposeActivity(cpi);
        QVERIFY(hot.actionable);
        QCOMPARE(hot.dir, -1);
        QCOMPARE(hot.action, QStringLiteral("SELL"));
        QVERIFY(hot.timing.contains(QStringLiteral("AFTER")));
        QVERIFY(hot.rationale.contains(QStringLiteral("0.5%")));
        QVERIFY(hot.rationale.contains(QStringLiteral("0.2%")));

        // Lower-impact growth data, stronger than previous: bullish, and the
        // consensus positioning may happen BEFORE the release.
        EconomicEvent pmi;
        pmi.title = QStringLiteral("Manufacturing PMI");
        pmi.impact = QStringLiteral("Medium");
        pmi.forecast = QStringLiteral("52.0");
        pmi.previous = QStringLiteral("50.0");
        pmi.when = QDateTime::currentDateTime().addSecs(3600);
        const EventProposal grow = proposeActivity(pmi);
        QVERIFY(grow.actionable);
        QCOMPARE(grow.dir, 1);
        QCOMPARE(grow.action, QStringLiteral("BUY"));
        QVERIFY(grow.timing.contains(QStringLiteral("BEFORE")));

        // No forecast/previous data: the only safe stance is to stay out.
        EconomicEvent blind;
        blind.title = QStringLiteral("FOMC Statement");
        blind.impact = QStringLiteral("High");
        blind.when = QDateTime::currentDateTime().addSecs(3600);
        const EventProposal out = proposeActivity(blind);
        QVERIFY(!out.actionable);
        QCOMPARE(out.dir, 0);
        QCOMPARE(out.action, QStringLiteral("STAY OUT"));
        QVERIFY(!out.rationale.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestEventInsight)
#include "tst_eventinsight.moc"
