// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/OrderRequestValidator.h"

#include <algorithm>
#include <cmath>

namespace trading {

namespace {

void refuse(OrderValidation &out, const QString &code, const QString &message)
{
    out.problems.append(OrderProblem{code, message});
}

// The amounts: exact, positive, and in the account's currency. An invalid Money here
// is not a formality — it is what an addition across currencies or a non-finite
// intermediate produces (REQ-N-008), and it must never be sent.
void checkAmounts(const OrderRequest &request, const OrderAmounts &amounts,
                  const OrderContext &ctx, OrderValidation &out)
{
    const Money &stake = amounts.stake;
    if (!stake.isValid()) {
        refuse(out, QStringLiteral("stake-invalid"),
               QStringLiteral("The stake is not a valid amount (mixed currencies or a "
                              "non-finite value)."));
    } else if (!stake.isPositive()) {
        refuse(out, QStringLiteral("stake-not-positive"),
               QStringLiteral("The stake must be greater than zero (%1).").arg(stake.toString()));
    }
    if (stake.isValid() && (ctx.accountCurrency != Currency::Invalid)
        && (stake.currency() != ctx.accountCurrency)) {
        refuse(out, QStringLiteral("stake-currency"),
               QStringLiteral("The stake is in %1, the account holds %2.")
                   .arg(currencyCode(stake.currency()), currencyCode(ctx.accountCurrency)));
    }
    // A stop or target of zero means "none" in OrderRequest, so only a NEGATIVE or
    // invalid one is a defect. A negative protective amount would be sent as a
    // positive number by the broker's own parsing — that is the silent case.
    for (const auto &pair : {std::pair{QStringLiteral("stop-loss"), amounts.stopLoss},
                             std::pair{QStringLiteral("take-profit"), amounts.takeProfit}}) {
        if (!pair.second.isValid()) {
            refuse(out, QStringLiteral("%1-invalid").arg(pair.first),
                   QStringLiteral("The %1 amount is not a valid amount.").arg(pair.first));
        } else if (pair.second.isNegative()) {
            refuse(out, QStringLiteral("%1-negative").arg(pair.first),
                   QStringLiteral("The %1 amount must not be negative (%2).")
                       .arg(pair.first, pair.second.toString()));
        }
    }
    // OrderRequest still carries doubles because that is what the broker's JSON
    // speaks. They must agree with the exact amounts to the cent, or two different
    // orders exist: the one that was validated and the one that gets sent.
    if (stake.isValid()
        && (std::llround(request.amount * 100.0) != stake.minorUnits())) {
        refuse(out, QStringLiteral("stake-mismatch"),
               QStringLiteral("The request's amount (%1) is not the validated stake (%2).")
                   .arg(request.amount, 0, 'f', 2)
                   .arg(stake.toString()));
    }
}

void checkCurrencies(const OrderContext &ctx, OrderValidation &out)
{
    if (ctx.accountCurrency == Currency::Invalid) {
        refuse(out, QStringLiteral("account-currency-unknown"),
               QStringLiteral("The account currency is not known — an order cannot be priced "
                              "against it."));
        return;
    }
    if (ctx.orderCurrency == Currency::Invalid) {
        refuse(out, QStringLiteral("order-currency-unknown"),
               QStringLiteral("The order currency is not set."));
        return;
    }
    if (ctx.orderCurrency != ctx.accountCurrency) {
        // Measured on a real account: eToro ACCEPTS the request and rejects it at
        // execution, so this is exactly the class of error a local check must catch.
        refuse(out, QStringLiteral("order-currency"),
               QStringLiteral("The order currency (%1) must be the account currency (%2).")
                   .arg(currencyCode(ctx.orderCurrency), currencyCode(ctx.accountCurrency)));
    }
}

void checkInstrument(const OrderRequest &request, const Money &stake, const OrderContext &ctx,
                     OrderValidation &out)
{
    const qint64 id = (request.instrumentId != 0) ? request.instrumentId
                                                  : ctx.instrument.instrumentId;
    if (id == 0) {
        refuse(out, QStringLiteral("instrument-unresolved"),
               QStringLiteral("No instrument id — the instrument has not been resolved."));
    } else if (ctx.instrument.isValid() && (request.instrumentId != 0)
               && (request.instrumentId != ctx.instrument.instrumentId)) {
        refuse(out, QStringLiteral("instrument-mismatch"),
               QStringLiteral("The request names instrument %1 while the context describes %2.")
                   .arg(request.instrumentId)
                   .arg(ctx.instrument.instrumentId));
    }
    // The per-order unit cap (eligibility endpoint, e.g. GOLD = 20 units). Units are
    // notional / rate, and both are needed — unknown either way disables the check
    // rather than guessing a bound.
    if ((ctx.instrument.maxUnitsPerOrder > 0.0) && (ctx.marketRate > 0.0) && stake.isValid()) {
        const double units = (stake.toDouble() * request.leverage) / ctx.marketRate;
        if (units > ctx.instrument.maxUnitsPerOrder) {
            refuse(out, QStringLiteral("units-over-cap"),
                   QStringLiteral("%1 units exceeds this instrument's per-order maximum of %2 "
                                  "(eToro accepts such an order and rejects it at execution).")
                       .arg(units, 0, 'f', 2)
                       .arg(ctx.instrument.maxUnitsPerOrder, 0, 'f', 2));
        }
    }
}

void checkLeverage(const OrderRequest &request, const OrderContext &ctx, OrderValidation &out)
{
    if (!std::isfinite(request.leverage) || (request.leverage < 1.0)) {
        refuse(out, QStringLiteral("leverage-invalid"),
               QStringLiteral("Leverage must be a finite value of at least 1 (got %1).")
                   .arg(request.leverage, 0, 'f', 2));
        return;
    }
    if (ctx.leverageLadder.isEmpty()) {
        return;   // unknown ladder disables its own check
    }
    const auto asked = static_cast<qint32>(std::llround(request.leverage));
    if ((std::fabs(request.leverage - static_cast<double>(asked)) > 1e-9)
        || !ctx.leverageLadder.contains(asked)) {
        QStringList steps;
        for (const qint32 step : ctx.leverageLadder) {
            steps.append(QStringLiteral("x%1").arg(step));
        }
        refuse(out, QStringLiteral("leverage-not-offered"),
               QStringLiteral("This instrument does not offer x%1 — it offers %2.")
                   .arg(request.leverage, 0, 'f', 2)
                   .arg(steps.join(QStringLiteral(", "))));
    }
}

// Which side a protective level sits on. Stops and targets are given to eToro as
// AMOUNTS, not rates, so the wrong-side error cannot be seen in the numbers alone: it
// shows up as a stop-loss amount that exceeds the stake (a loss bigger than the money
// at risk closes nothing) or as a take-profit of zero.
void checkProtection(const OrderAmounts &amounts, OrderValidation &out)
{
    const Money &stake = amounts.stake;
    const Money &stopLoss = amounts.stopLoss;
    const Money &takeProfit = amounts.takeProfit;
    if (!stake.isPositive() || !stopLoss.isValid() || !takeProfit.isValid()) {
        return;   // already refused above; do not pile a second reason on the same fault
    }
    if (stopLoss.isPositive() && (stopLoss > stake)) {
        refuse(out, QStringLiteral("stop-over-stake"),
               QStringLiteral("A stop-loss of %1 exceeds the %2 at risk — it could never be "
                              "reached, so the position would have no stop at all.")
                   .arg(stopLoss.toString(), stake.toString()));
    }
    if (takeProfit.isPositive() && stopLoss.isPositive() && (takeProfit < stopLoss)) {
        // Not a broker error, a strategy one: risking more than the reward is a losing
        // geometry, and REQ-F-011's 1.5 reward:risk exists precisely to avoid it. It is
        // refused rather than warned about, because the request is authorised once.
        refuse(out, QStringLiteral("reward-below-risk"),
               QStringLiteral("The take-profit (%1) is smaller than the stop-loss (%2) — the "
                              "trade risks more than it can win.")
                   .arg(takeProfit.toString(), stopLoss.toString()));
    }
}

void checkTrigger(const OrderRequest &request, const OrderContext &ctx, OrderValidation &out)
{
    if (!std::isfinite(request.triggerRate) || (request.triggerRate < 0.0)) {
        refuse(out, QStringLiteral("trigger-invalid"),
               QStringLiteral("The trigger rate must be a finite, non-negative value."));
        return;
    }
    if (!request.isLimit()) {
        // A MARKET order needs a live rate to be priced at all; without one the app
        // does not know what it is buying.
        if (ctx.marketRate <= 0.0) {
            refuse(out, QStringLiteral("no-market-rate"),
                   QStringLiteral("A market order needs a live rate; none is known for this "
                                  "instrument."));
        }
        return;
    }
    if (ctx.marketRate <= 0.0) {
        return;   // a limit order is priced off its own trigger; no live rate needed
    }
    // eToro's "mit" order fills at "the trigger rate or better", so a trigger on the
    // wrong side of the market fills IMMEDIATELY at the current price — the user asked
    // to wait and got a market order. Refuse it and say why.
    const bool wrongSide = request.isBuy ? (request.triggerRate > ctx.marketRate)
                                         : (request.triggerRate < ctx.marketRate);
    if (wrongSide) {
        refuse(out, QStringLiteral("trigger-wrong-side"),
               QStringLiteral("A %1 limit at %2 is already better than the market (%3), so it "
                              "would fill at once rather than wait.")
                   .arg(request.isBuy ? QStringLiteral("buy") : QStringLiteral("sell"))
                   .arg(request.triggerRate, 0, 'f', 4)
                   .arg(ctx.marketRate, 0, 'f', 4));
    }
}

void checkCaps(const Money &stake, const OrderContext &ctx, OrderValidation &out)
{
    if (!stake.isValid()) {
        return;
    }
    if (ctx.minStake.isValid() && (stake < ctx.minStake)) {
        refuse(out, QStringLiteral("below-min-stake"),
               QStringLiteral("The stake %1 is below this account's minimum of %2.")
                   .arg(stake.toString(), ctx.minStake.toString()));
    }
    if (ctx.maxStakePerOrder.isValid() && (stake > ctx.maxStakePerOrder)) {
        refuse(out, QStringLiteral("over-order-cap"),
               QStringLiteral("The stake %1 exceeds the per-order cap of %2.")
                   .arg(stake.toString(), ctx.maxStakePerOrder.toString()));
    }
    if (ctx.maxStakePerDay.isValid()) {
        const Money committed =
            ctx.committedToday.isValid() ? ctx.committedToday : Money::zero(stake.currency());
        const Money after = committed + stake;
        // An invalid sum here means the currencies did not match, which is itself a
        // refusal rather than a passed cap check.
        if (!after.isValid() || (after > ctx.maxStakePerDay)) {
            refuse(out, QStringLiteral("over-day-cap"),
                   QStringLiteral("This order would take today's committed total to %1, over "
                                  "the daily cap of %2.")
                       .arg(after.isValid() ? after.toString()
                                            : QStringLiteral("an unknown amount"),
                            ctx.maxStakePerDay.toString()));
        }
    }
}

} // namespace

QStringList OrderValidation::codes() const
{
    QStringList out;
    out.reserve(problems.size());
    for (const OrderProblem &problem : problems) {
        out.append(problem.code);
    }
    return out;
}

QString OrderValidation::summary() const
{
    if (ok()) {
        return QStringLiteral("valid");
    }
    QStringList lines;
    lines.reserve(problems.size());
    for (const OrderProblem &problem : problems) {
        lines.append(QStringLiteral("%1 (%2)").arg(problem.message, problem.code));
    }
    return lines.join(QStringLiteral(" "));
}

OrderValidation validateOrderRequest(const OrderRequest &request, const OrderAmounts &amounts,
                                     const OrderContext &context)
{
    OrderValidation out;
    // Every check runs: a request with three faults should report three, because the
    // person fixing it would otherwise learn about them one round-trip at a time.
    checkCurrencies(context, out);
    checkAmounts(request, amounts, context, out);
    checkInstrument(request, amounts.stake, context, out);
    checkLeverage(request, context, out);
    checkProtection(amounts, out);
    checkTrigger(request, context, out);
    checkCaps(amounts.stake, context, out);
    return out;
}

} // namespace trading
