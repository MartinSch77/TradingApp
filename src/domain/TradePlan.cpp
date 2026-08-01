#include "domain/TradePlan.h"

#include "domain/Forecasting.h"
#include "domain/Indicators.h"
#include "domain/SignalEnsemble.h"

#include <QHashFunctions>

#include <algorithm>
#include <numeric>
#include <cmath>
#include <utility>

namespace {

using trading::Ensemble;
using trading::McOutlook;
using trading::PlanInput;
using trading::TradePlan;

// The stop sits this many σ of the horizon's expected move away from the entry.
constexpr double kSlSigmaMult = 1.5;
// Take-profit distance = reward:risk × stop distance.
constexpr double kRewardRisk = 1.5;
// The loss when the stop is hit may consume at most this fraction of the stake.
constexpr double kRiskBudgetFrac = 0.25;
// Monte-Carlo paths for the win probability. 6000 paths put the standard error
// of pWin near ±0.6% — the verdict's noise band shrinks with it. Plans build
// off the GUI thread (REQ-N-006), so the extra paths cost no responsiveness.
constexpr qint32 kMcPaths = 6000;
// A directional call must keep at least this much ensemble confidence AFTER the
// VIX / event-risk trims to be actionable — the trims now gate the verdict
// instead of only shrinking a displayed number.
constexpr double kMinConfidencePct = 20.0;
// The Monte-Carlo win-rate must clear the break-even rate by this many standard
// errors of its own estimate; inside that band the "edge" is sampling noise.
constexpr double kWinRateSigmas = 2.0;
// The expected net edge must be worth at least this fraction of the stake —
// below it, model error dominates and the risk isn't paid for.
constexpr double kMinNetEdgeFrac = 0.0025;

// Deterministic Monte-Carlo seed derived from the plan's own inputs, so two
// plans over identical data draw identical paths (see buildTradePlan). Folding
// the 64-bit hash into 32 bits is deliberate — a seed needs no more — and
// written out, because MSVC /W4 is right to warn about an implicit narrowing
// (C4267). Never returns 0: monteCarlo reads that as "seed me securely".
quint32 planSeed(const QList<double> &closes, double price)
{
    const auto folded = static_cast<quint32>(
        (qHashRange(closes.cbegin(), closes.cend()) ^ qHash(price)) & 0xFFFFFFFFU);
    return (folded == 0U) ? 1U : folded;
}

// Values one plan phase computes for the next: the reference price, the side
// the levels are priced for, the barrier distances and the Monte-Carlo read.
// Plain data handed down the phase chain — never exposed outside this file.
struct PlanContext {
    double price = 0.0;
    qint32 side = 1;      // priced side: plan.dir, or the ensemble's lean when neutral
    double slFrac = 0.0;  // stop distance as a fraction of price
    double tpFrac = 0.0;  // take-profit distance as a fraction of price
    McOutlook mc;
};

// --- Direction & confidence: the shared ensemble, VIX haircut applied -------
void applyDirectionAndConfidence(TradePlan &plan, const PlanInput &in, const Ensemble &ens)
{
    plan.dir = (in.dir != 0) ? in.dir : ens.signalDir;
    double confidence = trading::applyVixHaircut(ens.confidence, in.vixValid, in.vix);
    if (in.eventRisk) {
        confidence *= 0.5;
    }
    plan.confidence = confidence;
}

// --- Geometry: stop/target distances from volatility -------------------------
void applyGeometry(TradePlan &plan, const PlanInput &in, PlanContext &ctx)
{
    const double vol = trading::volatilityPct(in.closes, 20);
    ctx.slFrac = trading::proposedSlFraction(vol, in.horizonHours);
    ctx.tpFrac = kRewardRisk * ctx.slFrac;
    plan.leverage = trading::recommendLeverage(ctx.slFrac, kRiskBudgetFrac, in.maxLeverage,
                                               in.leverageSteps);
    plan.marginSwingPct = vol * plan.leverage;

    const double lev = plan.leverage;
    plan.slAmount = in.invest * lev * ctx.slFrac;
    plan.tpAmount = in.invest * lev * ctx.tpFrac;
    // When no side is actionable, price the levels for the ensemble's lean so
    // the panel still shows a concrete, consistent geometry (ctx.side).
    plan.slRate = (ctx.side > 0) ? (ctx.price * (1.0 - ctx.slFrac))
                                 : (ctx.price * (1.0 + ctx.slFrac));
    plan.tpRate = (ctx.side > 0) ? (ctx.price * (1.0 + ctx.tpFrac))
                                 : (ctx.price * (1.0 - ctx.tpFrac));
}

// --- Win probability: bootstrap Monte-Carlo, TP-before-SL -------------------
// Three outcomes per path: take-profit struck first (win the TP amount),
// stop struck first (lose the SL amount), or neither barrier reached within
// the horizon (the trade expires between them, ≈ flat on average).
// Same inputs → same plan: when the caller didn't pin a seed, derive one
// from the data itself. Two plans built over identical closes/price (the
// decision panel and its ranked-table row, or two refreshes over unchanged
// data) then draw identical paths and can never disagree by sampling noise.
void applyWinProbability(TradePlan &plan, const PlanInput &in, PlanContext &ctx)
{
    const quint32 mcSeed = (in.mcSeed != 0U) ? in.mcSeed : planSeed(in.closes, ctx.price);
    ctx.mc = trading::monteCarlo(in.closes, {.price = ctx.price,
                                             .horizon = in.horizonHours,
                                             .tpFrac = ctx.tpFrac,
                                             .slFrac = ctx.slFrac,
                                             .paths = kMcPaths,
                                             .seed = mcSeed});
    plan.pWin = ctx.mc.valid ? ((ctx.side > 0) ? ctx.mc.pWinLong : ctx.mc.pWinShort) : 0.0;
    plan.pLose = ctx.mc.valid ? ((ctx.side > 0) ? ctx.mc.pLoseLong : ctx.mc.pLoseShort) : 0.0;
    plan.breakeven = 1.0 / (1.0 + kRewardRisk);  // sl / (tp + sl)
}

// Weekend rollover applies once when the holding window includes a Friday
// night (or starts inside the weekend on 24/7 instruments).
bool horizonCrossesWeekend(const QDateTime &now, qint32 horizonHours)
{
    if (!now.isValid()) {
        return false;
    }
    const QDateTime end = now.addSecs(static_cast<qint64>(horizonHours) * 3600);
    for (QDateTime t = now; t <= end; t = t.addDays(1)) {
        const qint32 dow = t.date().dayOfWeek();
        if ((dow == 5) || (dow == 6) || (dow == 7)) {  // Fri/Sat/Sun
            return true;
        }
    }
    return false;
}

// --- The cost bill over the horizon ------------------------------------------
// Half the spread is paid on opening and half on closing (eToro's own
// attribution); notional = invest × leverage, so each half-spread costs
// invest × leverage × spreadPct/2. Rollover scales with units; feeding the
// display-currency notional through units = notional / price lands the fee
// in display currency via the same conversion-cancels identity.
void applyCostBill(TradePlan &plan, const PlanInput &in, const PlanContext &ctx)
{
    const double lev = plan.leverage;
    const bool spreadKnown = in.spreadPct > 0.0;
    if (spreadKnown) {
        plan.openCost = in.invest * lev * (in.spreadPct / 100.0) / 2.0;
        plan.closeCost = plan.openCost;
    }
    plan.nights = static_cast<qint32>((std::max(1, in.horizonHours) + 23) / 24);
    plan.crossesWeekend = horizonCrossesWeekend(in.now, in.horizonHours);
    if (in.feesKnown && in.fees.isValid()) {
        const double units = (in.invest * lev) / ctx.price;
        const double perNight =
            ((ctx.side > 0) ? in.fees.buyOvernight : in.fees.sellOvernight) * units;
        const double weekend =
            ((ctx.side > 0) ? in.fees.buyWeekend : in.fees.sellWeekend) * units;
        plan.feePerNight = perNight;
        plan.weekendFee = plan.crossesWeekend ? weekend : 0.0;
        // The weekend night replaces one ordinary night in the bill.
        const qint32 ordinaryNights = plan.nights - (plan.crossesWeekend ? 1 : 0);
        plan.expectedCosts += (perNight * std::max(0, ordinaryNights)) + plan.weekendFee;
    }
    plan.expectedCosts += plan.openCost + plan.closeCost;
    plan.costsComplete = spreadKnown && in.feesKnown;
}

// --- Expected value -----------------------------------------------------------
// Decided paths win/lose the TP/SL amounts; paths that reach neither barrier
// close at the horizon with their MEASURED mean move (under a directional
// drift that residue is systematically non-zero, so assuming "expires flat"
// would bias the edge).
void applyExpectedValue(TradePlan &plan, const PlanInput &in, const PlanContext &ctx)
{
    if (ctx.mc.valid) {
        const double pExpire = std::max(0.0, 1.0 - plan.pWin - plan.pLose);
        const double expiryRet =
            (ctx.side > 0) ? ctx.mc.expiryRetLong : -ctx.mc.expiryRetShort;
        plan.expectedGross = (plan.pWin * plan.tpAmount) - (plan.pLose * plan.slAmount)
                             + (pExpire * in.invest * plan.leverage * expiryRet);
    }
    plan.expectedNet = plan.expectedGross - plan.expectedCosts;
}

// --- Verdict -------------------------------------------------------------------
// Of the paths where a barrier IS struck, the fraction that must be wins for
// the reward:risk to break even is `breakeven`. The estimated win-rate
// carries sampling noise (finite decisive paths), so an actionable call must
// clear break-even by kWinRateSigmas standard errors, keep enough ensemble
// confidence after the risk trims, and leave a net edge worth the risk.
void applyVerdict(TradePlan &plan, const PlanInput &in, const PlanContext &ctx)
{
    const double decided = plan.pWin + plan.pLose;
    const double condWin = (decided > 0.0) ? (plan.pWin / decided) : 0.0;
    const double decisive = decided * kMcPaths;
    const double winRateSe =
        (decisive > 0.0) ? std::sqrt((condWin * (1.0 - condWin)) / decisive) : 0.0;
    const double minNetEdge = kMinNetEdgeFrac * in.invest;
    if (plan.dir == 0) {
        plan.verdict = QStringLiteral("STAY OUT");
        plan.verdictReason = QStringLiteral("no clear directional signal");
    } else if (plan.confidence < kMinConfidencePct) {
        plan.verdict = QStringLiteral("STAY OUT");
        plan.verdictReason =
            QStringLiteral("signal confidence too low (%1% after the risk trims)")
                .arg(qRound(plan.confidence));
    } else if (ctx.mc.valid && (condWin <= (plan.breakeven + (kWinRateSigmas * winRateSe)))) {
        plan.verdict = QStringLiteral("STAY OUT");
        plan.verdictReason =
            QStringLiteral("win probability not clearly above the break-even rate");
    } else if (ctx.mc.valid && (plan.expectedNet <= 0.0)) {
        plan.verdict = QStringLiteral("STAY OUT");
        plan.verdictReason = QStringLiteral("costs eat the expected edge");
    } else if (ctx.mc.valid && (plan.expectedNet < minNetEdge)) {
        plan.verdict = QStringLiteral("STAY OUT");
        plan.verdictReason =
            QStringLiteral("edge after costs too thin to be worth the risk");
    } else {
        plan.verdict = (plan.dir > 0) ? QStringLiteral("BUY") : QStringLiteral("SELL");
    }
}

// --- Risk factor: 1 (low) .. 5 (very high) -----------------------------------
void applyRiskFactor(TradePlan &plan, const PlanInput &in)
{
    qint32 risk = 1;
    if (plan.marginSwingPct >= 30.0) {
        risk += 2;
        plan.riskNotes << QStringLiteral("x%1 leverage swings ±%2% of the stake per hour")
                              .arg(plan.leverage)
                              .arg(qRound(plan.marginSwingPct));
    } else if (plan.marginSwingPct >= 15.0) {
        risk += 1;
        plan.riskNotes << QStringLiteral("sizeable hourly margin swing (±%1%)")
                              .arg(qRound(plan.marginSwingPct));
    }
    if (in.vixValid && (in.vix >= 25.0)) {
        risk += 1;
        plan.riskNotes << QStringLiteral("elevated VIX (%1)").arg(in.vix, 0, 'f', 1);
    }
    if (in.eventRisk) {
        risk += 1;
        plan.riskNotes << QStringLiteral("high-impact event imminent");
    }
    if (in.fgValid && ((in.fearGreed <= 20.0) || (in.fearGreed >= 80.0))) {
        risk += 1;
        plan.riskNotes << QStringLiteral("crowd sentiment at an extreme (F&G %1)")
                              .arg(qRound(in.fearGreed));
    }
    if (plan.crossesWeekend) {
        plan.riskNotes << QStringLiteral(
            "horizon crosses the weekend: gap risk + the ~3× rollover night");
    }
    plan.riskFactor = std::clamp(risk, 1, 5);
}

} // namespace

namespace trading {

double proposedSlFraction(double volPctPerBar, qint32 horizonHours) noexcept
{
    const double horizon = std::max(1, horizonHours);
    const double frac = kSlSigmaMult * (volPctPerBar / 100.0) * std::sqrt(horizon);
    // Never propose a stop tighter than 0.1% (spread noise) or wider than 50%
    // of price (eToro rejects stops beyond the invested amount anyway).
    return std::clamp(frac, 0.001, 0.5);
}

qint32 recommendLeverage(double slFrac, double riskBudgetFrac, qint32 maxLeverage,
                         const QList<qint32> &steps)
{
    // Loss at the stop = invest × leverage × slFrac; keep it within the budget.
    const double raw = (slFrac > 0.0) ? (riskBudgetFrac / slFrac) : 1.0;
    const qint32 cap = (maxLeverage > 0) ? maxLeverage : 20;
    QList<qint32> allowed = steps;
    if (allowed.isEmpty()) {
        allowed = {1, 2, 5, 10, 20};
    }
    // Fold to the largest offered step that fits both the account cap and the
    // risk-derived maximum; 1 when nothing fits.
    return std::accumulate(allowed.cbegin(), allowed.cend(), 1,
                           [cap, raw](qint32 best, qint32 step) {
                               const bool fits = (step <= cap)
                                                 && (static_cast<double>(step) <= raw);
                               return (fits && (step > best)) ? step : best;
                           });
}

// Each phase below fills its slice of the plan and hands shared intermediate
// values down via PlanContext; the order is load-bearing (geometry needs the
// direction, the verdict needs the Monte-Carlo read and the cost bill).
TradePlan buildTradePlan(const PlanInput &in)
{
    TradePlan plan;
    if (in.closes.size() < 31) {
        return plan;  // not enough bars for the vol/ensemble reads
    }
    PlanContext ctx;
    ctx.price = (in.price > 0.0) ? in.price : in.closes.last();
    if ((ctx.price <= 0.0) || (in.invest <= 0.0)) {
        return plan;
    }
    plan.valid = true;

    const Ensemble ens = computeEnsemble(in.closes, in.vixValid, in.vixChangePct);
    applyDirectionAndConfidence(plan, in, ens);
    ctx.side = (plan.dir != 0) ? plan.dir : ((ens.score >= 0) ? 1 : -1);
    applyGeometry(plan, in, ctx);
    applyWinProbability(plan, in, ctx);
    applyCostBill(plan, in, ctx);
    applyExpectedValue(plan, in, ctx);
    applyVerdict(plan, in, ctx);
    applyRiskFactor(plan, in);
    return plan;
}

} // namespace trading
