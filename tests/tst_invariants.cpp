// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Property-based tests over deterministically-generated inputs: no new test
// framework, just Qt Test driving a fixed-seed PRNG loop (std::mt19937,
// std::uniform_*_distribution). Each slot below states one invariant that must
// hold for EVERY generated case, not just a hand-picked example — the point is
// that a single counter-example anywhere in the generated space fails the
// test, which a handful of example-based cases cannot promise.
//
// These are complementary to, not a replacement for, the example-based suites
// (tst_papertrader.cpp, tst_tradeplan.cpp, tst_predictionledger.cpp,
// tst_indexconfluence.cpp, tst_confirmgate.cpp): those pin specific measured
// scenarios and regressions; these pin the general shape those scenarios must
// always obey.

#include "domain/ConfirmGate.h"
#include "domain/IndexConfluence.h"
#include "domain/PaperTrader.h"
#include "domain/PredictionLedger.h"
#include "domain/TradePlan.h"

#include <QtTest/QtTest>
#include <QTimeZone>

#include <cmath>
#include <random>

using namespace trading;

namespace {
// One fixed seed for the whole file: a failure must be reproducible from the
// test name alone, not from whichever sequence the run happened to draw.
constexpr quint32 kSeed = 20260812U;
constexpr qint32 kIterations = 300;

[[nodiscard]] bool finiteAndNotNegative(double v)
{
    return std::isfinite(v) && (v >= 0.0);
}

// A short, varied but always-valid closes series for TradePlan/PaperTrader
// inputs: a random walk around 100 with a small per-bar volatility.
[[nodiscard]] QList<double> randomCloses(std::mt19937 &rng, qint32 bars)
{
    std::normal_distribution<double> step(0.0, 0.6);
    QList<double> closes;
    double price = 100.0;
    for (qint32 i = 0; i < bars; ++i) {
        price = std::max(1.0, price + step(rng));
        closes.append(price);
    }
    return closes;
}
} // namespace

class TestInvariants : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-INV-001 @design DES-DOM-PAPER
    // @relation(REQ-F-031, scope=function)
    //
    // Position size is never negative, NaN or infinite — including at the
    // degenerate inputs (zero/negative rate, zero/negative risk-per-stake,
    // zero equity) that a real feed or a mis-set config can actually produce.
    void TS_INV_001_positionSizeNeverNegativeNanOrInfinite()
    {
        std::mt19937 rng(kSeed);
        // Stake is an amount of money invested — never negative in any real caller.
        // Leverage and rate keep their degenerate (<=0) values on purpose: paperUnits
        // is documented to clamp/guard those itself (std::max(1, leverage), rate<=0 -> 0).
        std::uniform_real_distribution<double> stakeDist(0.0, 100000.0);
        std::uniform_int_distribution<qint32> leverageDist(-5, 50);
        std::uniform_real_distribution<double> rateDist(-10.0, 100000.0);
        std::uniform_real_distribution<double> equityDist(0.0, 1000000.0);
        std::uniform_real_distribution<double> riskPerStakeDist(-1.0, 5.0);

        for (qint32 i = 0; i < kIterations; ++i) {
            const double stake = stakeDist(rng);
            const qint32 leverage = leverageDist(rng);
            const double rate = rateDist(rng);
            const double units = paperUnits(stake, leverage, rate);
            QVERIFY(finiteAndNotNegative(units) || (units == 0.0));
            QVERIFY(!std::isnan(units));
            QVERIFY(!std::isinf(units));

            BookState book;
            book.equity = equityDist(rng);
            book.cash = book.equity;
            BotConfig cfg;
            const double riskPerStake = riskPerStakeDist(rng);
            const double sized = paperStakeFor(book, cfg, riskPerStake);
            QVERIFY(!std::isnan(sized));
            QVERIFY(!std::isinf(sized));
            QVERIFY(sized >= 0.0);
        }
    }

    //! @tstid TS-INV-002 @design DES-DOM-PAPER
    // @relation(REQ-F-031, scope=function)
    //
    // The stake this book is granted room for never lets the PROJECTED total
    // portfolio risk (existing openRisk plus the new stake's own loss-at-stop)
    // exceed maxPortfolioRiskFraction × equity — the "real governor" the header
    // comment on BotConfig::maxPortfolioRiskFraction describes.
    void TS_INV_002_stakeRoomNeverExceedsPortfolioRiskBudget()
    {
        std::mt19937 rng(kSeed + 1);
        std::uniform_real_distribution<double> equityDist(1000.0, 200000.0);
        std::uniform_real_distribution<double> riskPerStakeDist(0.001, 2.0);
        const BotConfig defaults;
        // A WELL-FORMED book never already carries more risk than its own budget —
        // every stake it holds was itself sized by this same function. Testing with
        // a pre-existing openRisk beyond the budget would test an unreachable state,
        // not a real property.
        std::uniform_real_distribution<double> openRiskFracDist(
            0.0, defaults.maxPortfolioRiskFraction * 0.95);

        for (qint32 i = 0; i < kIterations; ++i) {
            BookState book;
            book.equity = equityDist(rng);
            book.cash = book.equity;   // margin is never the binding limit here
            book.openRisk = book.equity * openRiskFracDist(rng);

            BotConfig cfg;
            // Isolate the risk-budget rule: switch off the other two caps this
            // property is not about (TS-PAPER-008/012/014 do the same).
            cfg.maxInvestedEur = 0.0;
            cfg.maxExposureFraction = 1.0;

            const double riskPerStake = riskPerStakeDist(rng);
            const double stake = paperStakeFor(book, cfg, riskPerStake);
            QVERIFY(stake >= 0.0);

            const double projectedRisk = book.openRisk + (stake * riskPerStake);
            const double budget = cfg.maxPortfolioRiskFraction * book.equity;
            // A cent of floating-point slack, not a loophole: the sizing itself
            // works in doubles and this only guards against rounding noise.
            QVERIFY(projectedRisk <= budget + 0.01);
        }
    }

    //! @tstid TS-INV-003 @design DES-DOM-PAPER
    // @relation(REQ-F-031, scope=function)
    //
    // BUY/SELL symmetry: a short candidate that mirrors a long one exactly
    // (same confidence, same volatility, opposite direction) gets mirrored
    // geometry — the same stop/target DISTANCES from the fill, the opposite
    // side — never a different risk profile just because it is a sell.
    void TS_INV_003_buySellGeometryIsMirrored()
    {
        std::mt19937 rng(kSeed + 2);
        std::uniform_real_distribution<double> confDist(15.0, 95.0);

        for (qint32 i = 0; i < kIterations; ++i) {
            const QList<double> closes = randomCloses(rng, 40);
            const double confidence = confDist(rng);
            const double bid = closes.last() - 0.05;
            const double ask = closes.last() + 0.05;

            CandidateInput longIn;
            longIn.symbol = QStringLiteral("SYM");
            longIn.dir = +1;
            longIn.confidence = confidence;
            longIn.closes = closes;
            longIn.bid = bid;
            longIn.ask = ask;
            longIn.marketOpen = true;
            longIn.quoteLive = true;

            CandidateInput shortIn = longIn;
            shortIn.dir = -1;

            const BotConfig cfg;
            const EntrySignal longSig = buildEntrySignal(longIn, cfg);
            const EntrySignal shortSig = buildEntrySignal(shortIn, cfg);
            if (!longSig.valid || !shortSig.valid) {
                continue;   // too little data to size at all — nothing to compare
            }

            QVERIFY(longSig.isBuy);
            QVERIFY(!shortSig.isBuy);
            QCOMPARE(longSig.leverage, shortSig.leverage);
            QVERIFY(qFuzzyCompare(longSig.confidence, shortSig.confidence));

            // Distances from the fill, as fractions, must match: a long's stop
            // sits BELOW its fill by the same fraction a short's sits ABOVE.
            const double longSlFrac =
                std::abs(longSig.fillRate - longSig.slRate) / longSig.fillRate;
            const double shortSlFrac =
                std::abs(shortSig.fillRate - shortSig.slRate) / shortSig.fillRate;
            const double longTpFrac =
                std::abs(longSig.fillRate - longSig.tpRate) / longSig.fillRate;
            const double shortTpFrac =
                std::abs(shortSig.fillRate - shortSig.tpRate) / shortSig.fillRate;
            QVERIFY(std::abs(longSlFrac - shortSlFrac) < 1e-9);
            QVERIFY(std::abs(longTpFrac - shortTpFrac) < 1e-9);

            // And the sides point the right way: a long's target is ABOVE its
            // fill and its stop BELOW; a short is the mirror image.
            QVERIFY(longSig.tpRate > longSig.fillRate);
            QVERIFY(longSig.slRate < longSig.fillRate);
            QVERIFY(shortSig.tpRate < shortSig.fillRate);
            QVERIFY(shortSig.slRate > shortSig.fillRate);
        }
    }

    //! @tstid TS-INV-004 @design DES-DOM-LEDGER
    // @relation(REQ-F-037, scope=function)
    //
    // predictionToJson followed by predictionFromJson must reproduce every
    // field of the original Prediction exactly — a ledger whose own
    // serialization loses precision would silently corrupt the record it
    // exists to keep honest.
    void TS_INV_004_predictionJsonRoundTripPreservesValue()
    {
        std::mt19937 rng(kSeed + 3);
        std::uniform_int_distribution<qint32> dirDist(-1, 1);
        std::uniform_real_distribution<double> strengthDist(0.0, 100.0);
        std::uniform_int_distribution<qint32> countDist(0, 12);
        std::uniform_real_distribution<double> priceDist(0.01, 50000.0);
        std::uniform_int_distribution<qint32> regimeDist(0, 4);
        std::uniform_int_distribution<qint64> secsDist(0, 2'000'000'000);

        for (qint32 i = 0; i < kIterations; ++i) {
            Prediction p;
            p.at = QDateTime::fromSecsSinceEpoch(secsDist(rng), QTimeZone::UTC);
            p.symbol = QStringLiteral("SYM-%1").arg(i);
            p.dir = dirDist(rng);
            p.strength = strengthDist(rng);
            p.measured = countDist(rng);
            p.unknowns = countDist(rng);
            p.price = priceDist(rng);
            p.regime = static_cast<Regime>(regimeDist(rng));
            p.taken = (dirDist(rng) != 0);
            p.refusal = p.taken ? QString() : QStringLiteral("no-signal");
            p.priorMoveDir = dirDist(rng);
            p.vwapSide = dirDist(rng);

            const QJsonObject json = predictionToJson(p);
            const std::optional<Prediction> back = predictionFromJson(json);
            QVERIFY(back.has_value());
            QCOMPARE(back->at, p.at);
            QCOMPARE(back->symbol, p.symbol);
            QCOMPARE(back->dir, p.dir);
            QCOMPARE(back->strength, p.strength);
            QCOMPARE(back->measured, p.measured);
            QCOMPARE(back->unknowns, p.unknowns);
            QCOMPARE(back->price, p.price);
            QCOMPARE(static_cast<qint32>(back->regime), static_cast<qint32>(p.regime));
            QCOMPARE(back->taken, p.taken);
            QCOMPARE(back->refusal, p.refusal);
            QCOMPARE(back->priorMoveDir, p.priorMoveDir);
            QCOMPARE(back->vwapSide, p.vwapSide);
        }
    }

    //! @tstid TS-INV-005 @design DES-DOM-PLAN
    // @relation(REQ-F-010, scope=function)
    //
    // A trade plan's probabilities are always in [0, 1] — pWin, pLose and
    // breakeven are measured Monte-Carlo frequencies, and a frequency outside
    // that range is not a rounding slip, it is a broken model.
    void TS_INV_005_planProbabilitiesStayInUnitRange()
    {
        std::mt19937 rng(kSeed + 4);
        std::uniform_int_distribution<qint32> dirDist(-1, 1);
        std::uniform_real_distribution<double> investDist(10.0, 50000.0);
        std::uniform_int_distribution<qint32> horizonDist(1, 168);

        for (qint32 i = 0; i < kIterations; ++i) {
            PlanInput in;
            in.closes = randomCloses(rng, 60);
            in.price = in.closes.last();
            in.dir = dirDist(rng);
            in.invest = investDist(rng);
            in.maxLeverage = 20;
            in.horizonHours = horizonDist(rng);
            in.spreadPct = 0.05;
            in.mcSeed = static_cast<quint32>(i + 1);   // deterministic, never 0

            const TradePlan plan = buildTradePlan(in);
            if (!plan.valid) {
                continue;
            }
            QVERIFY(plan.pWin >= 0.0);
            QVERIFY(plan.pWin <= 1.0);
            QVERIFY(plan.pLose >= 0.0);
            QVERIFY(plan.pLose <= 1.0);
            QVERIFY(plan.breakeven >= 0.0);
            QVERIFY(plan.breakeven <= 1.0);
            // Win, lose and "expires between" partition the outcome space.
            QVERIFY(plan.pWin + plan.pLose <= 1.0 + 1e-9);
        }
    }

    //! @tstid TS-INV-006 @design DES-DOM-PLAN
    // @relation(REQ-F-010, scope=function)
    //
    // Costs reduce a plan's net edge and never increase it: expectedNet is
    // always expectedGross minus expectedCosts (never gross plus costs), and
    // widening the spread — holding every other input fixed — can only ever
    // lower or hold the expected net, never raise it.
    void TS_INV_006_costsNeverIncreaseExpectedNet()
    {
        std::mt19937 rng(kSeed + 5);
        std::uniform_real_distribution<double> investDist(100.0, 20000.0);
        std::uniform_real_distribution<double> spreadDist(0.0, 3.0);

        for (qint32 i = 0; i < kIterations; ++i) {
            PlanInput base;
            base.closes = randomCloses(rng, 60);
            base.price = base.closes.last();
            base.dir = +1;
            base.invest = investDist(rng);
            base.maxLeverage = 20;
            base.horizonHours = 24;
            base.feesKnown = false;   // isolate the spread's own effect
            base.mcSeed = static_cast<quint32>(i + 1000);

            const double lowSpread = spreadDist(rng);
            const double highSpread = lowSpread + spreadDist(rng);   // >= lowSpread

            PlanInput lowIn = base;
            lowIn.spreadPct = lowSpread;
            PlanInput highIn = base;
            highIn.spreadPct = highSpread;

            const TradePlan lowPlan = buildTradePlan(lowIn);
            const TradePlan highPlan = buildTradePlan(highIn);
            if (!lowPlan.valid || !highPlan.valid) {
                continue;
            }

            // The algebraic identity holds regardless of sign (a credit fee can
            // make costs negative; the subtraction itself must still be exact).
            QVERIFY(std::abs((lowPlan.expectedGross - lowPlan.expectedCosts) -
                             lowPlan.expectedNet) < 1e-6);
            QVERIFY(std::abs((highPlan.expectedGross - highPlan.expectedCosts) -
                             highPlan.expectedNet) < 1e-6);

            // A wider spread never has a smaller cost bill (fees are switched
            // off above, so the spread is the only thing that moved)...
            QVERIFY(highPlan.expectedCosts >= lowPlan.expectedCosts - 1e-6);
            // ...and the same gross expectation minus a bigger bill is never a
            // bigger net.
            if (std::abs(highPlan.expectedGross - lowPlan.expectedGross) < 1e-6) {
                QVERIFY(highPlan.expectedNet <= lowPlan.expectedNet + 1e-6);
            }
        }
    }

    //! @tstid TS-INV-007 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    //
    // A read that could not be measured never counts as agreement, and taking
    // evidence AWAY (turning a known read back to unknown) can never INCREASE
    // how many reads confluenceFor counts as agreeing.
    void TS_INV_007_missingMeasurementNeverIncreasesEvidence()
    {
        std::mt19937 rng(kSeed + 6);
        // +1/-1 only: a known-but-neutral read (dir == 0) is a real, distinct
        // outcome confluenceFor deliberately counts as neither met nor against
        // nor unknown, which this property is not about — TS-CONF's own suite
        // covers that case. Isolating known-vs-unknown here keeps the count
        // exact: every known read lands in met or against, every unknown one
        // in unknown, so met+against+unknown == 9 always.
        std::uniform_int_distribution<qint32> sideDist(0, 1);   // 0 -> -1, 1 -> +1
        std::uniform_int_distribution<qint32> knownDist(0, 1);
        std::bernoulli_distribution coin(0.5);

        for (qint32 i = 0; i < kIterations; ++i) {
            const qint32 dir = coin(rng) ? 1 : -1;
            auto randomRead = [&]() {
                Read r;
                r.known = (knownDist(rng) == 1);
                r.dir = r.known ? ((sideDist(rng) == 1) ? 1 : -1) : 0;
                return r;
            };

            IndexReads reads;
            reads.futuresLead = randomRead();
            reads.futuresMomentum = randomRead();
            reads.volatility = randomRead();
            reads.yields = randomRead();
            reads.curve = randomRead();
            reads.participation = randomRead();
            reads.aboveVwap = randomRead();
            reads.upDownVolume = randomRead();
            reads.structure = randomRead();

            const Confluence baseline = confluenceFor(reads, dir);
            QCOMPARE(baseline.met + baseline.against + baseline.unknown, 9);

            // Now take ONE known read away (flip it to unknown) and recompute.
            // Whatever it was previously counted as, evidence must not go up.
            Read *const fields[] = {&reads.futuresLead, &reads.futuresMomentum,
                                    &reads.volatility,   &reads.yields,
                                    &reads.curve,        &reads.participation,
                                    &reads.aboveVwap,    &reads.upDownVolume,
                                    &reads.structure};
            qint32 knownIndex = -1;
            for (qint32 f = 0; f < 9; ++f) {
                if (fields[f]->known) {
                    knownIndex = f;
                    break;
                }
            }
            if (knownIndex < 0) {
                continue;   // every read was already unknown — nothing to take away
            }
            fields[knownIndex]->known = false;
            fields[knownIndex]->dir = 0;
            const Confluence reduced = confluenceFor(reads, dir);
            QCOMPARE(reduced.unknown, baseline.unknown + 1);
            QVERIFY(reduced.met <= baseline.met);
            QVERIFY(reduced.against <= baseline.against);
            QVERIFY((reduced.met + reduced.against) <= (baseline.met + baseline.against));
        }
    }

    //! @tstid TS-INV-008 @design DES-DOM-GATE
    // @relation(REQ-N-005, scope=function)
    //
    // A rejected press (anything other than a valid, same-action, in-window
    // second press) never commits — which is the one fact the caller relies on
    // to keep an unconfirmed order off the broker path: it never consults
    // anything else in ConfirmDecision to decide whether to place the order.
    void TS_INV_008_rejectedPressNeverCommits()
    {
        std::mt19937 rng(kSeed + 7);
        std::uniform_int_distribution<qint32> actionDist(0, 3);
        std::uniform_int_distribution<qint64> nowDist(0, 10'000);
        std::uniform_int_distribution<qint64> gapDist(0, 5'000);
        constexpr qint64 kWindow = 1000;
        const QStringList actions{QStringLiteral("BUY 100.00 at x1"),
                                  QStringLiteral("SELL 100.00 at x1"),
                                  QStringLiteral("BUY 500.00 at x5"),
                                  QStringLiteral("CLOSE 42")};

        for (qint32 i = 0; i < kIterations; ++i) {
            ConfirmGate gate;
            gate.action = actions[actionDist(rng)];
            gate.armedAtMs = nowDist(rng);

            const QString pressAction = actions[actionDist(rng)];
            const qint64 gap = gapDist(rng);
            const qint64 nowMs = gate.armedAtMs + gap;

            const ConfirmDecision decision =
                confirmPress(gate, pressAction, nowMs, kWindow);

            const bool sameAction = (pressAction == gate.action);
            // Strict less-than, matching confirmPress's own "elapsed < windowMs": a
            // press landing exactly on the window's edge is stale, not fresh.
            const bool inWindow = (gap < kWindow);
            const bool armed = !gate.action.isEmpty();

            if (armed && sameAction && inWindow) {
                QVERIFY(decision.commit);
            } else {
                QVERIFY(!decision.commit);
            }
            // A commit clears the gate, so the next press needs two fresh
            // presses rather than inheriting this one's arming.
            if (decision.commit) {
                QVERIFY(decision.next.action.isEmpty());
            }
        }

        // A stray first-ever press (nothing armed at all) never commits.
        for (qint32 i = 0; i < kIterations; ++i) {
            const QString pressAction = actions[actionDist(rng)];
            const ConfirmDecision decision =
                confirmPress(ConfirmGate{}, pressAction, nowDist(rng), kWindow);
            QVERIFY(!decision.commit);
        }
    }
};

QTEST_GUILESS_MAIN(TestInvariants)
#include "tst_invariants.moc"
