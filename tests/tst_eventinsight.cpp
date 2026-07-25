// Unit tests for the calendar-event heuristics (DES-DOM-EVT).

#include "domain/EventInsight.h"

#include <QtTest/QtTest>

using namespace trading;

class TestEventInsight : public QObject
{
    Q_OBJECT
private slots:
    //! @tstid TS-EVT-001 @verifies REQ-F-020 @design DES-DOM-EVT
    void TS_EVT_001_parseNumFormats()
    {
        bool ok = false;
        QCOMPARE(parseNum(QStringLiteral("0.3%"), &ok), 0.3);
        QVERIFY(ok);
        QCOMPARE(parseNum(QStringLiteral("-0.2%"), &ok), -0.2);
        QVERIFY(ok);
        QCOMPARE(parseNum(QStringLiteral("215K"), &ok), 215.0);
        QVERIFY(ok);
        static_cast<void>(parseNum(QStringLiteral("n/a"), &ok));
        QVERIFY(!ok);
    }

    //! @tstid TS-EVT-002 @verifies REQ-F-020 @design DES-DOM-EVT
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
};

QTEST_GUILESS_MAIN(TestEventInsight)
#include "tst_eventinsight.moc"
