#include "domain/TradePlan.h"

#include "domain/Forecasting.h"
#include "domain/Indicators.h"
#include "domain/SignalEnsemble.h"

#include <algorithm>
#include <cmath>

namespace {

// The stop sits this many σ of the horizon's expected move away from the entry.
constexpr double kSlSigmaMult = 1.5;
// Take-profit distance = reward:risk × stop distance.
constexpr double kRewardRisk = 1.5;
// The loss when the stop is hit may consume at most this fraction of the stake.
constexpr double kRiskBudgetFrac = 0.25;
// Monte-Carlo paths for the win probability (matches the signals panel's scale).
constexpr qint32 kMcPaths = 2000;

} // namespace

namespace trading {

double proposedSlFraction(double volPctPerBar, qint32 horizonHours)
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
    qint32 best = 1;
    for (const qint32 step : allowed) {
        if ((step <= cap) && (static_cast<double>(step) <= raw) && (step > best)) {
            best = step;
        }
    }
    return best;
}

TradePlan buildTradePlan(const PlanInput &in)
{
    TradePlan plan;
    if (in.closes.size() < 31) {
        return plan;  // not enough bars for the vol/ensemble reads
    }
    const double price = (in.price > 0.0) ? in.price : in.closes.last();
    if ((price <= 0.0) || (in.invest <= 0.0)) {
        return plan;
    }
    plan.valid = true;

    // --- Direction & confidence: the shared ensemble, VIX haircut applied ----
    const Ensemble ens = computeEnsemble(in.closes, in.vixValid, in.vixChangePct);
    plan.dir = (in.dir != 0) ? in.dir : ens.signalDir;
    double confidence = applyVixHaircut(ens.confidence, in.vixValid, in.vix);
    if (in.eventRisk) {
        confidence *= 0.5;
    }
    plan.confidence = confidence;

    // --- Geometry: stop/target distances from volatility ---------------------
    const double vol = volatilityPct(in.closes, 20);
    const double slFrac = proposedSlFraction(vol, in.horizonHours);
    const double tpFrac = kRewardRisk * slFrac;
    plan.leverage = recommendLeverage(slFrac, kRiskBudgetFrac, in.maxLeverage,
                                      in.leverageSteps);
    plan.marginSwingPct = vol * plan.leverage;

    const double lev = plan.leverage;
    plan.slAmount = in.invest * lev * slFrac;
    plan.tpAmount = in.invest * lev * tpFrac;
    // When no side is actionable, price the levels for the ensemble's lean so
    // the panel still shows a concrete, consistent geometry.
    const qint32 side = (plan.dir != 0) ? plan.dir : ((ens.score >= 0) ? 1 : -1);
    plan.slRate = (side > 0) ? (price * (1.0 - slFrac)) : (price * (1.0 + slFrac));
    plan.tpRate = (side > 0) ? (price * (1.0 + tpFrac)) : (price * (1.0 - tpFrac));

    // --- Win probability: bootstrap Monte-Carlo, TP-before-SL ---------------
    // Three outcomes per path: take-profit struck first (win the TP amount),
    // stop struck first (lose the SL amount), or neither barrier reached within
    // the horizon (the trade expires between them, ≈ flat on average).
    const McOutlook mc =
        monteCarlo(in.closes, price, in.horizonHours, tpFrac, slFrac, kMcPaths);
    plan.pWin = mc.valid ? ((side > 0) ? mc.pWinLong : mc.pWinShort) : 0.0;
    plan.pLose = mc.valid ? ((side > 0) ? mc.pLoseLong : mc.pLoseShort) : 0.0;
    plan.breakeven = 1.0 / (1.0 + kRewardRisk);  // sl / (tp + sl)

    // --- The cost bill over the horizon --------------------------------------
    // Half the spread is paid on opening and half on closing (eToro's own
    // attribution); notional = invest × leverage, so each half-spread costs
    // invest × leverage × spreadPct/2. Rollover scales with units; feeding the
    // display-currency notional through units = notional / price lands the fee
    // in display currency via the same conversion-cancels identity.
    const bool spreadKnown = in.spreadPct > 0.0;
    if (spreadKnown) {
        plan.openCost = in.invest * lev * (in.spreadPct / 100.0) / 2.0;
        plan.closeCost = plan.openCost;
    }
    plan.nights = static_cast<qint32>((std::max(1, in.horizonHours) + 23) / 24);
    if (in.now.isValid()) {
        // Weekend rollover applies once when the holding window includes a
        // Friday night (or starts inside the weekend on 24/7 instruments).
        const QDateTime end = in.now.addSecs(static_cast<qint64>(in.horizonHours) * 3600);
        for (QDateTime t = in.now; t <= end; t = t.addDays(1)) {
            const qint32 dow = t.date().dayOfWeek();
            if ((dow == 5) || (dow == 6) || (dow == 7)) {  // Fri/Sat/Sun
                plan.crossesWeekend = true;
                break;
            }
        }
    }
    if (in.feesKnown && in.fees.isValid()) {
        const double units = (in.invest * lev) / price;
        const double perNight =
            ((side > 0) ? in.fees.buyOvernight : in.fees.sellOvernight) * units;
        const double weekend =
            ((side > 0) ? in.fees.buyWeekend : in.fees.sellWeekend) * units;
        plan.feePerNight = perNight;
        plan.weekendFee = plan.crossesWeekend ? weekend : 0.0;
        // The weekend night replaces one ordinary night in the bill.
        const qint32 ordinaryNights = plan.nights - (plan.crossesWeekend ? 1 : 0);
        plan.expectedCosts += (perNight * std::max(0, ordinaryNights)) + plan.weekendFee;
    }
    plan.expectedCosts += plan.openCost + plan.closeCost;
    plan.costsComplete = spreadKnown && in.feesKnown;

    // --- Expected value & verdict --------------------------------------------
    // Paths that reach neither barrier contribute ≈ 0 (the drift over one day is
    // small next to the barrier distances), so EV = pWin·TP − pLose·SL.
    if (mc.valid) {
        plan.expectedGross = (plan.pWin * plan.tpAmount) - (plan.pLose * plan.slAmount);
    }
    plan.expectedNet = plan.expectedGross - plan.expectedCosts;

    // Of the paths where a barrier IS struck, the fraction that must be wins for
    // the reward:risk to break even is `breakeven`.
    const double decided = plan.pWin + plan.pLose;
    const double condWin = (decided > 0.0) ? (plan.pWin / decided) : 0.0;
    if (plan.dir == 0) {
        plan.verdict = QStringLiteral("STAY OUT");
        plan.verdictReason = QStringLiteral("no clear directional signal");
    } else if (mc.valid && (condWin <= plan.breakeven)) {
        plan.verdict = QStringLiteral("STAY OUT");
        plan.verdictReason =
            QStringLiteral("win probability below the break-even rate");
    } else if (mc.valid && (plan.expectedNet <= 0.0)) {
        plan.verdict = QStringLiteral("STAY OUT");
        plan.verdictReason = QStringLiteral("costs eat the expected edge");
    } else {
        plan.verdict = (plan.dir > 0) ? QStringLiteral("BUY") : QStringLiteral("SELL");
    }

    // --- Risk factor: 1 (low) .. 5 (very high) -------------------------------
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
    return plan;
}

} // namespace trading
