// Unit tests for the technical indicators (DES-DOM-IND).
// Each test function maps 1:1 to a TS id in docs/test_spec.md.

#include "domain/Indicators.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {

// A strictly rising / falling series of `n` points starting at `start`.
QList<double> ramp(qint32 n, double start, double step)
{
    QList<double> s;
    for (qint32 i = 0; i < n; ++i) {
        s.append(start + (step * i));
    }
    return s;
}

} // namespace

class TestIndicators : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-IND-001 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_001_smaWindowAverage()
    {
        const QList<double> s = {1.0, 2.0, 3.0, 4.0, 5.0};
        QCOMPARE(sma(s, 3), (3.0 + 4.0 + 5.0) / 3.0);
        QCOMPARE(sma(s, 5), 3.0);
        QCOMPARE(sma(s, 6), 0.0);  // shorter than the window
    }

    //! @tstid TS-IND-002 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_002_rsiExtremes()
    {
        QCOMPARE(rsi({1.0, 2.0}, 14), -1.0);  // not enough data
        const QList<double> up = ramp(30, 100.0, 1.0);
        QCOMPARE(rsi(up, 14), 100.0);  // only gains
        const QList<double> down = ramp(30, 100.0, -1.0);
        QVERIFY(rsi(down, 14) < 50.0);
    }

    //! @tstid TS-IND-003 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_003_stochasticRangePosition()
    {
        QList<double> s = ramp(20, 100.0, 1.0);
        QCOMPARE(stochasticK(s, 14), 100.0);  // latest = highest of the range
        s.last() = s[s.size() - 14];          // latest = lowest of the range
        QCOMPARE(stochasticK(s, 14), 0.0);
    }

    //! @tstid TS-IND-004 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_004_macdTrendSign()
    {
        QVERIFY(macdHistogram(ramp(80, 100.0, 1.0)) > 0.0);
        QVERIFY(macdHistogram(ramp(80, 200.0, -1.0)) < 0.0);
    }

    //! @tstid TS-IND-005 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_005_bollingerPercentB()
    {
        // Alternating series: the mean sits centrally, the last value (high leg)
        // sits in the upper half of the band.
        QList<double> s;
        for (qint32 i = 0; i < 40; ++i) {
            s.append((i % 2 == 0) ? 100.0 : 102.0);
        }
        const double b = bollingerPercentB(s, 20);
        QVERIFY(b > 0.5);
        QVERIFY(b <= 1.5);
    }

    //! @tstid TS-IND-006 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_006_volatilityConstantVsNoisy()
    {
        const QList<double> flat(30, 100.0);
        QCOMPARE(volatilityPct(flat, 20), 0.0);
        QList<double> noisy;
        for (qint32 i = 0; i < 30; ++i) {
            noisy.append((i % 2 == 0) ? 100.0 : 101.0);
        }
        QVERIFY(volatilityPct(noisy, 20) > 0.0);
    }

    //! @tstid TS-IND-007 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_007_rocPercentChange()
    {
        const QList<double> s = {100.0, 100.0, 100.0, 100.0, 100.0, 110.0};
        QCOMPARE(roc(s, 5), 10.0);  // +10% over 5 bars
    }

    //! @tstid TS-IND-008 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_008_returnsAndMean()
    {
        const QList<double> s = {100.0, 110.0, 99.0};
        const QList<double> r = returnsOf(s);
        QCOMPARE(r.size(), 2);
        QCOMPARE(r[0], 0.10);
        QVERIFY(std::abs(r[1] - (-0.10)) < 1e-12);
        QVERIFY(std::abs(meanReturn(s, 2)) < 1e-12);  // +10% then −10%
    }

    //! @tstid TS-IND-009 @design DES-DOM-IND
    // @relation(REQ-F-005, scope=function)
    void TS_IND_009_emaSeedAndPull()
    {
        const QList<double> s = {100.0, 100.0, 100.0, 200.0, 200.0, 200.0};
        const QList<double> e = emaSeries(s, 3);
        QCOMPARE(e.size(), s.size());
        QCOMPARE(e.first(), 100.0);          // seeded with the first value
        QVERIFY(e.last() > 150.0);           // pulled towards the recent level
        QVERIFY(e.last() < 200.0);           // but lagging it
    }
};

QTEST_GUILESS_MAIN(TestIndicators)
#include "tst_indicators.moc"
