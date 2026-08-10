// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "console/AdviseView.h"

#include <QtTest/QtTest>

using namespace trading::console;

// The one-shot advice report (REQ-F-047): every line the console can print, pinned headless.
class TestAdviseView : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-ADV-001 @design DES-CON-ADVISE
    // @relation(REQ-F-047, scope=function)
    void TS_ADV_001_aProposalCarriesTheGeometryTheEvidenceAndTheDisclaimer()
    {
        AdviseInput in;
        in.symbol = QStringLiteral("SPX500");
        in.havePlan = true;
        in.plan.valid = true;
        in.plan.dir = 1;
        in.plan.verdict = QStringLiteral("BUY");
        in.plan.confidence = 61.0;
        in.plan.leverage = 5;
        in.plan.slRate = 4950.0;
        in.plan.tpRate = 5075.0;
        in.plan.slAmount = 50.0;
        in.plan.tpAmount = 75.0;
        in.plan.pWin = 0.44;
        in.plan.breakeven = 0.40;
        in.plan.riskFactor = 2;
        in.plan.openCost = 3.0;
        in.plan.closeCost = 3.0;
        in.plan.nights = 2;
        in.price = 5000.0;
        in.haveRow = true;
        in.row.dir = 1;
        in.row.confidence = 61.0;
        in.row.haveTech = true;
        in.row.techLabel = QStringLiteral("BUY");
        in.row.techConf = 58.0;
        in.row.haveRating = false;   // an absent source is NAMED, never skipped
        in.vixValid = true;
        in.vix = 15.2;
        in.aiAsked = true;
        in.aiLine = QStringLiteral("BUY (62%) — trend and rating agree");
        in.readLines << QStringLiteral("futures lead: up (both indices)");
        in.crowdLine = QStringLiteral("Crowd evidence SPX500 (experimental): score …");

        const AdviseVerdict v = adviseReport(in);
        QCOMPARE(v.exitCode, 0);
        QVERIFY(v.text.startsWith(QStringLiteral("PROPOSAL: BUY SPX500 x5")));
        QVERIFY(v.text.contains(QStringLiteral("stop 4950.00, target 5075.00")));
        QVERIFY(v.text.contains(QStringLiteral("P(win) 44%")));
        QVERIFY(v.text.contains(QStringLiteral("technical ensemble: BUY (58)")));
        QVERIFY(v.text.contains(QStringLiteral("web rating: absent")));
        QVERIFY(v.text.contains(QStringLiteral("VIX: 15.2")));
        QVERIFY(v.text.contains(QStringLiteral("read: futures lead")));
        QVERIFY(v.text.contains(QStringLiteral("local model: BUY (62%)")));
        QVERIFY(v.text.contains(QStringLiteral("Crowd evidence")));
        QVERIFY(v.text.contains(QStringLiteral("advisory only")));
        QVERIFY(v.text.contains(QStringLiteral("links no order path")));
    }

    //! @tstid TS-ADV-002 @design DES-CON-ADVISE
    // @relation(REQ-F-047, scope=function)
    void TS_ADV_002_noTradeAndNoDataAreDistinctReasonedAndScriptable()
    {
        // A reasoned STAY OUT: exit code 2, the plan's own reason on the first line.
        AdviseInput out;
        out.symbol = QStringLiteral("GOLD");
        out.havePlan = true;
        out.plan.valid = true;
        out.plan.dir = 0;
        out.plan.verdict = QStringLiteral("STAY OUT");
        out.plan.verdictReason = QStringLiteral("net edge below the cost bill");
        const AdviseVerdict stayOut = adviseReport(out);
        QCOMPARE(stayOut.exitCode, 2);
        QVERIFY(stayOut.text.startsWith(QStringLiteral("NO TRADE for GOLD")));
        QVERIFY(stayOut.text.contains(QStringLiteral("net edge below the cost bill")));

        // Nothing gathered: exit code 3, and every missing source NAMED — absent is not
        // silence, and an empty report must not read as a calm market.
        AdviseInput bare;
        bare.symbol = QStringLiteral("NSDQ100");
        bare.absentSources << QStringLiteral("price series") << QStringLiteral("web rating");
        const AdviseVerdict noData = adviseReport(bare);
        QCOMPARE(noData.exitCode, 3);
        QVERIFY(noData.text.startsWith(QStringLiteral("NOT ENOUGH DATA for NSDQ100")));
        QVERIFY(noData.text.contains(QStringLiteral("absent: price series")));
        QVERIFY(noData.text.contains(QStringLiteral("absent: web rating")));
        QVERIFY(noData.text.contains(QStringLiteral("VIX: absent")));
        // The model not asked is shown as "not consulted" in the decision-sources block —
        // never as a fabricated answer.
        QVERIFY(noData.text.contains(QStringLiteral("AI (local model): not consulted")));
    }
};

QTEST_GUILESS_MAIN(TestAdviseView)
#include "tst_adviseview.moc"
