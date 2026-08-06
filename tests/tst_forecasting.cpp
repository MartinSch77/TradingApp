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
        QVERIFY(!monteCarlo({100.0, 101.0},
                            {.price = 100.0, .horizon = 3, .tpFrac = 0.01, .slFrac = 0.01,
                             .paths = 200})
                     .valid);
        QVERIFY(monteCarlo(upDrift(80),
                           {.price = 100.0, .horizon = 5, .tpFrac = 0.01, .slFrac = 0.01,
                            .paths = 200})
                    .valid);
    }

    //! @tstid TS-FC-006 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_006_monteCarloDriftDirection()
    {
        const QList<double> s = upDrift(120);
        const McOutlook o = monteCarlo(s, {.price = s.last(), .horizon = 24, .tpFrac = 0.01,
                                           .slFrac = 0.01, .paths = 2000});
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
        const McOutlook o = monteCarlo(s, {.price = s.last(), .horizon = 5, .tpFrac = 0.20,
                                           .slFrac = 0.20, .paths = 2000});
        QVERIFY(o.valid);
        QCOMPARE(o.pWinLong + o.pLoseLong, 0.0);  // nothing decided
        QVERIFY(o.expiryRetLong > 0.0);           // residue follows the up-drift
        QVERIFY(o.expiryRetShort > 0.0);          // same paths, short's barriers
        // Without barriers there are no expiry statistics.
        const McOutlook nb = monteCarlo(s, {.price = s.last(), .horizon = 5, .paths = 500});
        QCOMPARE(nb.expiryRetLong, 0.0);
        QCOMPARE(nb.expiryRetShort, 0.0);
    }
    //! @tstid TS-FC-009 @design DES-DOM-FC
    // @relation(REQ-F-006, scope=function)
    void TS_FC_009_everyRefusalPathAnswersItsOwnGuard()
    {
        // The guards, one at a time: each of these returns the "nothing measured"
        // answer, and each must do so for its OWN reason. A forecast that quietly
        // reports 0.5 or a zero slope for an input it could not read is worse than one
        // that says nothing, because the caller cannot tell the two apart.
        QList<double> rising;
        for (int i = 0; i < 60; ++i) {
            rising.append(100.0 + (0.5 * i));
        }

        // Regression: too few points asked for, and more asked for than exist.
        QVERIFY(!linRegForecast(rising, 2).valid);
        QVERIFY(!linRegForecast(rising, rising.size() + 1).valid);
        QVERIFY(linRegForecast(rising, 20).valid);
        // A single repeated x cannot happen here, but a flat series can: the slope is
        // zero and the fit is still valid — "no trend" is a measurement.
        const QList<double> flat(40, 100.0);
        const Regression flatFit = linRegForecast(flat, 20);
        QVERIFY(flatFit.valid);
        QCOMPARE(flatFit.slopePct, 0.0);

        // kNN: window too small, series too short for window+k+3, and a series whose
        // returns cannot fill one window.
        QVERIFY(knnForecast(rising, 2, 5).k == 0);
        QVERIFY(knnForecast(rising, 20, 40).k == 0);
        QVERIFY(knnForecast({100.0, 101.0, 102.0}, 3, 2).k == 0);
        QVERIFY(knnForecast(rising, 5, 5).k > 0);
        // A series containing a non-positive value contributes a 0.0 return rather
        // than a division by zero, and the forecast still comes out.
        QList<double> withZero = rising;
        withZero[10] = 0.0;
        QVERIFY(knnForecast(withZero, 5, 5).k > 0);

        // Hurst: fewer than 20 returns is undecidable (0.5 = "no answer"), and a
        // perfectly flat series has neither range nor deviation to divide by.
        QCOMPARE(hurstExponent({100.0, 101.0, 102.0}), 0.5);
        QCOMPARE(hurstExponent(QList<double>(60, 100.0)), 0.5);
        QVERIFY(hurstExponent(rising) != 0.5);

        // Monte Carlo: each of the four refusals separately.
        const McParams ok{.price = 100.0, .horizon = 5, .paths = 100, .seed = 7U};
        QVERIFY(!monteCarlo({100.0, 101.0}, ok).valid);
        McParams noPrice = ok;
        noPrice.price = 0.0;
        QVERIFY(!monteCarlo(rising, noPrice).valid);
        McParams noHorizon = ok;
        noHorizon.horizon = 0;
        QVERIFY(!monteCarlo(rising, noHorizon).valid);
        McParams noPaths = ok;
        noPaths.paths = 0;
        QVERIFY(!monteCarlo(rising, noPaths).valid);
        // A seed of 0 means "seed from the system" — still a real run, just not a
        // reproducible one, which is why every other test passes a seed.
        McParams unseeded = ok;
        unseeded.seed = 0U;
        QVERIFY(monteCarlo(rising, unseeded).valid);
    }
};

QTEST_GUILESS_MAIN(TestForecasting)
#include "tst_forecasting.moc"
