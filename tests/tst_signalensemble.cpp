// Unit tests for the buy/sell signal ensemble (DES-DOM-ENS).

#include "domain/SignalEnsemble.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {

QList<double> trend(qint32 n, double factorEven, double factorOdd)
{
    QList<double> s;
    double p = 100.0;
    for (qint32 i = 0; i < n; ++i) {
        p *= (i % 2 == 0) ? factorEven : factorOdd;
        s.append(p);
    }
    return s;
}

} // namespace

class TestSignalEnsemble : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-ENS-001 @design DES-DOM-ENS
    // @relation(REQ-F-007, scope=function)
    void TS_ENS_001_uptrendBuys()
    {
        const Ensemble e = computeEnsemble(trend(120, 1.004, 1.001), false, 0.0);
        QVERIFY(e.valid);
        QCOMPARE(e.signal, QStringLiteral("BUY"));
        QCOMPARE(e.signalDir, 1);
        QVERIFY(e.confidence > 0.0);
    }

    //! @tstid TS-ENS-002 @design DES-DOM-ENS
    // @relation(REQ-F-007, scope=function)
    void TS_ENS_002_downtrendSells()
    {
        const Ensemble e = computeEnsemble(trend(120, 0.996, 0.999), false, 0.0);
        QVERIFY(e.valid);
        QCOMPARE(e.signal, QStringLiteral("SELL"));
        QCOMPARE(e.signalDir, -1);
    }

    //! @tstid TS-ENS-003 @design DES-DOM-ENS
    // @relation(REQ-F-007, scope=function)
    void TS_ENS_003_shortSeriesInvalid()
    {
        QVERIFY(!computeEnsemble({100.0, 101.0, 102.0}, false, 0.0).valid);
    }

    //! @tstid TS-ENS-004 @design DES-DOM-ENS
    // @relation(REQ-F-007, scope=function)
    void TS_ENS_004_vixHaircutMonotone()
    {
        const double base = 80.0;
        const double calm = applyVixHaircut(base, true, 14.0);
        const double elevated = applyVixHaircut(base, true, 28.0);
        const double panic = applyVixHaircut(base, true, 40.0);
        QVERIFY(calm <= base);
        QVERIFY(elevated <= calm);
        QVERIFY(panic <= elevated);
        QCOMPARE(applyVixHaircut(base, false, 40.0), base);  // no VIX → no trim
    }
};

QTEST_GUILESS_MAIN(TestSignalEnsemble)
#include "tst_signalensemble.moc"
