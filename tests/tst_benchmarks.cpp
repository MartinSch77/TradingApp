// Performance benchmarks over the domain hot paths (REQ-N-006). QBENCHMARK
// reports wall-clock per iteration in the JUnit output and on the console —
// run them from the Debug AND the release build (./build_all.sh release) to
// see what optimisation buys; tools/profile.sh drills into the hotspots.

#include "domain/DecisionEngine.h"
#include "domain/Forecasting.h"
#include "domain/Indicators.h"
#include "domain/TradePlan.h"

#include <QtTest/QtTest>

#include <cmath>

using namespace trading;

namespace {
// Deterministic pseudo-random walk: benchmarks must not jitter with the seed.
QList<double> walk(qint32 n)
{
    QList<double> s;
    s.reserve(n);
    double p = 5000.0;
    for (qint32 i = 0; i < n; ++i) {
        p *= 1.0 + (0.001 * std::sin(static_cast<double>(i) * 0.7))
             + (0.0004 * std::cos(static_cast<double>(i) * 2.3));
        s.append(p);
    }
    return s;
}
}  // namespace

class TestBenchmarks : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees markers
private slots:
    //! @tstid TS-PERF-001 @design DES-DOM-FC
    // @relation(REQ-N-006, scope=function)
    void TS_PERF_001_monteCarloBenchmark()
    {
        const QList<double> s = walk(240);
        McOutlook mc;
        QBENCHMARK {
            mc = monteCarlo(s, {.price = s.last(), .horizon = 3, .tpFrac = 0.01,
                                .slFrac = 0.01, .paths = 1200});
        }
        QVERIFY(mc.valid);
    }

    //! @tstid TS-PERF-002 @design DES-DOM-PLAN
    // @relation(REQ-N-006, scope=function)
    void TS_PERF_002_tradePlanBenchmark()
    {
        PlanInput in;
        in.closes = walk(240);
        in.invest = 3750.0;
        in.maxLeverage = 20;
        in.spreadPct = 0.05;
        in.horizonHours = 24;
        TradePlan plan;
        QBENCHMARK {
            plan = buildTradePlan(in);
        }
        QVERIFY(plan.valid);
    }

    //! @tstid TS-PERF-003 @design DES-DOM-DEC
    // @relation(REQ-N-006, scope=function)
    void TS_PERF_003_decisionRowsBenchmark()
    {
        // A realistic decision scan: ~25 instruments with full close series.
        MarketSnapshot m;
        for (qint32 i = 0; i < 25; ++i) {
            ScreenerRow r;
            r.symbol = QStringLiteral("SYM%1").arg(i);
            r.closes = walk(240);
            r.lastPrice = r.closes.last();
            r.maxLeverage = 20;
            r.ok = true;
            m.screenerRows << r;
            m.intradayBySymbol.insert(r.symbol, walk(120));
        }
        m.fgValid = true;
        m.fearGreed = 55.0;
        QList<DecisionRow> rows;
        QBENCHMARK {
            rows = computeDecisionRows(m);
        }
        QCOMPARE(rows.size(), 25);
    }
};

QTEST_GUILESS_MAIN(TestBenchmarks)
#include "tst_benchmarks.moc"
