// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

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

    //! @tstid TS-EVT-004 @design DES-DOM-EVT
    // @relation(REQ-F-023, scope=function)
    void TS_EVT_004_everyEventFamilyExplainsItselfAndPicksASide()
    {
        // The plain-language explainer is what a reader sees INSTEAD of a title they
        // may not know ("PPI m/m"), so every family it claims to cover has to answer
        // with its own text rather than the generic fallback.
        const auto about = [](const QString &title) {
            EconomicEvent e;
            e.title = title;
            return eventAbout(e, QStringLiteral("SPX500"));
        };
        QVERIFY(about(QStringLiteral("FOMC Rate Decision")).contains(QStringLiteral("rates")));
        QVERIFY(about(QStringLiteral("Core PPI y/y")).contains(QStringLiteral("inflation")));
        QVERIFY(about(QStringLiteral("Unemployment Rate")).contains(QStringLiteral("jobless")));
        QVERIFY(about(QStringLiteral("Nonfarm Payrolls")).contains(QStringLiteral("Jobs")));
        QVERIFY(about(QStringLiteral("GDP q/q")).contains(QStringLiteral("Gross Domestic")));
        QVERIFY(about(QStringLiteral("Retail Sales m/m")).contains(QStringLiteral("consumers")));
        QVERIFY(about(QStringLiteral("ISM Services PMI")).contains(QStringLiteral("50")));
        QVERIFY(about(QStringLiteral("Consumer Confidence")).contains(QStringLiteral("optimistic")));
        // …and an unknown release says so honestly, naming the instrument it may move.
        const QString unknown = about(QStringLiteral("Wholesale Inventories"));
        QVERIFY(unknown.contains(QStringLiteral("SPX500")));
        QVERIFY(unknown.contains(QStringLiteral("surprise")));

        // The direction each family implies, from the same forecast-versus-previous
        // comparison: hotter inflation is bearish, more unemployment is bearish,
        // stronger growth is bullish — and an unchanged forecast has no side at all.
        const auto sideOf = [](const QString &title, const QString &forecast,
                               const QString &previous) {
            EconomicEvent e;
            e.title = title;
            e.impact = QStringLiteral("High");
            e.forecast = forecast;
            e.previous = previous;
            e.when = QDateTime::currentDateTime().addSecs(3600);
            return proposeActivity(e);
        };
        QCOMPARE(sideOf(QStringLiteral("Unemployment Rate"), QStringLiteral("4.5%"),
                        QStringLiteral("4.1%")).dir, -1);
        QCOMPARE(sideOf(QStringLiteral("Unemployment Rate"), QStringLiteral("3.8%"),
                        QStringLiteral("4.1%")).dir, 1);
        QCOMPARE(sideOf(QStringLiteral("Retail Sales m/m"), QStringLiteral("1.2%"),
                        QStringLiteral("0.4%")).dir, 1);
        QCOMPARE(sideOf(QStringLiteral("CPI m/m"), QStringLiteral("0.2%"),
                        QStringLiteral("0.5%")).dir, 1);      // cooling inflation: risk-on
        const EventProposal steady = sideOf(QStringLiteral("CPI m/m"), QStringLiteral("0.3%"),
                                            QStringLiteral("0.3%"));
        QCOMPARE(steady.dir, 0);
        QVERIFY(steady.rationale.contains(QStringLiteral("steady")));
        // A family the direction rules do not cover still warns about the swings
        // rather than inventing a side.
        const EventProposal odd = sideOf(QStringLiteral("Wholesale Inventories"),
                                         QStringLiteral("0.9%"), QStringLiteral("0.1%"));
        QCOMPARE(odd.dir, 0);
        QVERIFY(odd.rationale.contains(QStringLiteral("swings")));
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
