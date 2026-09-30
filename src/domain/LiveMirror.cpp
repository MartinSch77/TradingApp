// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/LiveMirror.h"

#include <algorithm>
#include <cmath>

namespace trading {

namespace {

LiveOrderPlan refuse(const QString &code, const QString &why)
{
    LiveOrderPlan plan;
    plan.code = code;
    plan.why = why;
    return plan;
}

QString moneyPlain(const Money &m)
{
    return QStringLiteral("%1 %2").arg(m.toDouble(), 0, 'f', 2).arg(currencyCode(m.currency()));
}

// The paper trade's loss at its stop (or gain at its target) in the FX-free identity the
// simulation books in: units × |open − level|, where units = stake × leverage / open. It
// is denominated in the stake's own currency, which is what makes scaling it by the
// stake RATIO below land in the order currency.
double paperMoveTo(const PaperTrade &trade, double level)
{
    if (level <= 0.0) {
        return 0.0;   // no such level on the paper trade: no such protection live either
    }
    return paperUnits(trade.stake, trade.leverage, trade.openRate)
           * std::fabs(trade.openRate - level);
}

// The stake in the ORDER currency: min(cap, paper stake) in EUR, converted once. The
// account is USD in practice; a EUR account needs no rate at all, and any other currency
// is unrepresentable in Money, so there is no third branch to get wrong.
Money liveStakeFor(const LiveMirrorInputs &in, const LiveMirrorConfig &cfg, QString *why)
{
    const Money paperStake = Money::fromDouble(in.trade.stake, Currency::Eur);
    const Money capped = std::min(cfg.maxPerOrder, paperStake);
    if (in.accountCurrency == Currency::Eur) {
        return capped;
    }
    if (in.eurPerUsd <= 0.0) {
        *why = QStringLiteral("the EUR/USD rate is not known yet, so a %1 stake cannot be "
                              "priced in the account's currency")
                   .arg(moneyPlain(capped));
        return {};
    }
    return Money::fromDouble(capped.toDouble() / in.eurPerUsd, Currency::Usd);
}

}   // namespace

LiveOrderPlan liveOrderFor(const LiveMirrorInputs &in, const LiveMirrorConfig &cfg)
{
    const PaperTrade &trade = in.trade;
    // The structural refusals first — scope, count, id — and the transient one (no FX
    // rate yet) last, so a log of refusals reads as "what this instrument IS" before
    // "what the feeds have not delivered yet".
    if (!cfg.symbols.contains(trade.symbol)) {
        return refuse(QStringLiteral("live-scope"),
                      QStringLiteral("%1 is outside the real-money scope (%2)")
                          .arg(trade.symbol, cfg.symbols.join(QStringLiteral("+"))));
    }
    if (in.openLivePositionsForSymbol >= cfg.maxPositionsPerSymbol) {
        return refuse(QStringLiteral("live-position-cap"),
                      QStringLiteral("%1 already has %2 live position(s), the cap is %3")
                          .arg(trade.symbol)
                          .arg(in.openLivePositionsForSymbol)
                          .arg(cfg.maxPositionsPerSymbol));
    }
    if (trade.instrumentId == 0) {
        // Unlike the paper open, which prices itself off a candle and needs no venue id,
        // a REAL order is placed BY id — there is nothing to send without one.
        return refuse(QStringLiteral("live-unresolved"),
                      QStringLiteral("the venue's instrument id for %1 is not known, and a "
                                     "real order is placed by id")
                          .arg(trade.symbol));
    }
    // A cap that is not set, or set to zero, sends nothing: 0 is the safe reading of a
    // missing number here, never "no cap". The paper stake has to be positive too, or the
    // ratio below is undefined.
    if (!cfg.maxPerOrder.isPositive() || !cfg.maxDailyLoss.isValid() || (trade.stake <= 0.0)) {
        return refuse(QStringLiteral("live-unsized"),
                      QStringLiteral("no positive per-order cap and daily-loss cap are set, "
                                     "or the paper stake is not positive (%1)")
                          .arg(trade.stake, 0, 'f', 2));
    }
    QString fxWhy;
    const Money stake = liveStakeFor(in, cfg, &fxWhy);
    if (!stake.isValid()) {
        return refuse(QStringLiteral("live-fx-unknown"), fxWhy);
    }

    LiveOrderPlan plan;
    plan.ok = true;
    // The stop/target AMOUNTS are the paper trade's own loss-at-stop and gain-at-target
    // scaled by live-stake ÷ paper-stake, so the live position's stop and target sit at
    // the SAME RATES as the simulated one's. A paper trade without a stop (slRate 0)
    // gets none live either — stated in `why` rather than invented.
    const double ratio = stake.toDouble() / trade.stake;
    const Currency ccy = stake.currency();
    plan.amounts.stake = stake;
    plan.amounts.stopLoss = Money::fromDouble(paperMoveTo(trade, trade.slRate) * ratio, ccy);
    plan.amounts.takeProfit = Money::fromDouble(paperMoveTo(trade, trade.tpRate) * ratio, ccy);
    plan.request.instrumentId = trade.instrumentId;
    plan.request.isBuy = trade.isBuy;
    plan.request.amount = plan.amounts.stake.toDouble();
    plan.request.leverage = static_cast<double>(trade.leverage);
    plan.request.stopLossAmount = plan.amounts.stopLoss.toDouble();
    plan.request.takeProfitAmount = plan.amounts.takeProfit.toDouble();
    plan.request.triggerRate = 0.0;   // a MARKET order: the paper trade is already open
    plan.why = QStringLiteral("%1 %2 %3 at x%4 (paper stake %5 EUR, cap %6): stop %7, "
                              "target %8%9")
                   .arg(trade.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"), trade.symbol,
                        moneyPlain(stake))
                   .arg(trade.leverage)
                   .arg(trade.stake, 0, 'f', 2)
                   .arg(moneyPlain(cfg.maxPerOrder), moneyPlain(plan.amounts.stopLoss),
                        moneyPlain(plan.amounts.takeProfit),
                        (trade.slRate <= 0.0) ? QStringLiteral(" — the paper trade has NO "
                                                               "stop, so the live position "
                                                               "gets none")
                                              : QString());
    return plan;
}

LiveDay liveDayAfterClose(const LiveDay &day, const QDateTime &now, const Money &closedPnl)
{
    const QDate today = now.toUTC().date();
    LiveDay next = day;
    if (next.date != today) {
        next.date = today;
        next.realized = closedPnl.isValid() ? Money::zero(closedPnl.currency()) : Money();
    }
    if (!closedPnl.isValid()) {
        return next;   // an unknown result books nothing; the executor logs that it was unknown
    }
    next.realized = next.realized.isValid() ? (next.realized + closedPnl) : closedPnl;
    return next;
}

bool liveDailyLossReached(const LiveDay &day, const QDateTime &now, const Money &maxDailyLoss)
{
    if (!maxDailyLoss.isValid() || !day.realized.isValid() || (day.date != now.toUTC().date())) {
        return false;
    }
    // "<=" on purpose: a loss EXACTLY at the cap is the cap reached. Mixed currencies
    // compare as unordered, which makes this false — the executor then still has the
    // per-order cap and the arm's own stake ledger, and the mismatch is visible in the
    // audit's currency codes.
    return day.realized <= -maxDailyLoss;
}

QString liveGrantAction(const LiveMirrorConfig &cfg)
{
    return QStringLiteral("ARM REAL MONEY: %1, max %2 per order, %3 daily loss, %4 min")
        .arg(cfg.symbols.join(QStringLiteral("+")), moneyPlain(cfg.maxPerOrder),
             moneyPlain(cfg.maxDailyLoss))
        .arg(cfg.armMinutes);
}

}   // namespace trading
