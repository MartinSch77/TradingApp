// Unit tests for the statistical forecasting models (DES-DOM-FC).

#include "domain/Forecasting.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {

QList<double> ramp(qint32 n, double start, double step)
{
    QList<double> s;
    for (qint32 i = 0; i < n; ++i) {
        s.append(start + (step * i));
    }
    return s;
}

// Multiplicative uptrend with tiny alternating noise: strong drift for the
// Monte-Carlo direction tests, enough variance for the bootstrap to resample.
QList<double> upDrift(qint32 n)
{
    QList<double> s;
    double p = 100.0;
    for (qint32 i = 0; i < n; ++i) {
        p *= (i % 2 == 0) ? 1.004 : 1.001;
        s.append(p);
    }
    return s;
}

} // namespace

class TestForecasting : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-FC-001 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_001_regressionPerfectLine()
    {
        const Regression r = linRegForecast(ramp(40, 100.0, 1.0), 30);
        QVERIFY(r.valid);
        QVERIFY(r.r2 > 0.999);
        QVERIFY(r.slopePct > 0.0);  // ~1 point/bar on a ~115 mean → ~0.87%/bar
    }

    //! @tstid TS-FC-002 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_002_regressionFlatSeries()
    {
        const Regression r = linRegForecast(QList<double>(40, 100.0), 30);
        QVERIFY(std::abs(r.slopePct) < 1e-9);
    }

    //! @tstid TS-FC-003 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_003_knnBounds()
    {
        const Knn k = knnForecast(upDrift(120), 10, 5);
        QVERIFY(k.k > 0);
        QVERIFY(k.agree >= 0.0);
        QVERIFY(k.agree <= 1.0);
    }

    //! @tstid TS-FC-004 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_004_hurstTrendingVsAlternating()
    {
        // Persistent: two up bars, one down — runs build up. Anti-persistent:
        // strict alternation reverses every bar.
        QList<double> persistent;
        double p = 100.0;
        for (qint32 i = 0; i < 120; ++i) {
            p *= (i % 3 == 2) ? 0.999 : 1.002;
            persistent.append(p);
        }
        QList<double> alternating;
        p = 100.0;
        for (qint32 i = 0; i < 120; ++i) {
            p *= (i % 2 == 0) ? 1.002 : 0.998;
            alternating.append(p);
        }
        QVERIFY(hurstExponent(persistent) > hurstExponent(alternating));
    }

    //! @tstid TS-FC-005 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_005_monteCarloValidity()
    {
        QVERIFY(!monteCarlo({100.0, 101.0}, 100.0, 3, 0.01, 0.01, 200).valid);
        QVERIFY(monteCarlo(upDrift(80), 100.0, 5, 0.01, 0.01, 200).valid);
    }

    //! @tstid TS-FC-006 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_006_monteCarloDriftDirection()
    {
        const QList<double> s = upDrift(120);
        const McOutlook o = monteCarlo(s, s.last(), 24, 0.01, 0.01, 2000);
        QVERIFY(o.valid);
        QVERIFY(o.pWinLong > o.pWinShort);       // drift favours the long side
        QVERIFY(o.pWinLong + o.pLoseLong <= 1.0 + 1e-9);
        QVERIFY(o.pWinShort + o.pLoseShort <= 1.0 + 1e-9);
        QVERIFY(o.p5 <= o.p95);
    }

    //! @tstid TS-FC-007 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_007_sigmoidShape()
    {
        QCOMPARE(sigmoid(0.0), 0.5);
        QVERIFY(sigmoid(10.0) > 0.99);
        QVERIFY(sigmoid(-10.0) < 0.01);
        QVERIFY(sigmoid(1.0) > sigmoid(0.5));
    }

    //! @tstid TS-FC-008 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_008_monteCarloExpiryResidue()
    {
        // Barriers far wider than 5 bars of ≤0.4% drift can reach: every path
        // expires between them, and the measured mean final move must carry the
        // drift's sign — the residue the trade-plan EV now prices in.
        const QList<double> s = upDrift(120);
        const McOutlook o = monteCarlo(s, s.last(), 5, 0.20, 0.20, 2000);
        QVERIFY(o.valid);
        QCOMPARE(o.pWinLong + o.pLoseLong, 0.0);  // nothing decided
        QVERIFY(o.expiryRetLong > 0.0);           // residue follows the up-drift
        QVERIFY(o.expiryRetShort > 0.0);          // same paths, short's barriers
        // Without barriers there are no expiry statistics.
        const McOutlook nb = monteCarlo(s, s.last(), 5, 0.0, 0.0, 500);
        QCOMPARE(nb.expiryRetLong, 0.0);
        QCOMPARE(nb.expiryRetShort, 0.0);
    }
};

QTEST_GUILESS_MAIN(TestForecasting)
#include "tst_forecasting.moc"
