// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_LIVEMIRROR_H
#define TRADINGAPP_DOMAIN_LIVEMIRROR_H

#include "domain/Models.h"
#include "domain/Money.h"
#include "domain/OrderRequestValidator.h"
#include "domain/PaperTrader.h"

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QStringList>

// The pure half of the real-money mirror (REQ-F-076): what ONE paper trade becomes as
// ONE real order, and the day-loss ledger the kill switch is tripped on.
//
// Everything that needs a broker, a clock or a window lives in ui/LiveBotExecutor; this
// file has none of those, so the arithmetic that decides how much real money leaves the
// account is testable to the cent without either. Two rules are load-bearing and easy to
// "tidy" wrongly:
//
//  * The stake is min(cap, the paper stake), NEVER the paper stake alone. The paper bot
//    sizes for a 50 000 EUR simulated account (a 6% stake is 3 000 EUR); the cap is what
//    the owner granted per order. The conversion to the order currency and the rounding
//    to cents happen ONCE, through Money::fromDouble (REQ-N-008).
//  * The stop and target are sent as AMOUNTS (eToro's API takes amounts, not rates), and
//    they are the paper trade's own loss-at-stop and gain-at-target SCALED by the stake
//    ratio. A stop amount that is not scaled with the stake lands at a different RATE
//    than the paper trade's — a 250 EUR position with the 3 000 EUR position's stop amount
//    has its stop twelve times as far away, i.e. effectively none.
namespace trading {

// The grant a human arms under. Built by the composition root from Config; the defaults
// here are the owner's own numbers (2026-09-30) so a test needs no Config.
struct LiveMirrorConfig {
    QStringList symbols{QStringLiteral("SPX500"), QStringLiteral("NSDQ100")};   // the fixed scope
    Money maxPerOrder;   // 250 EUR by default at the composition root; INVALID here
    Money maxDailyLoss;   // 250 EUR by default at the composition root; INVALID here
    qint32 maxPositionsPerSymbol = 1;
    qint32 armMinutes = 480;   // one trading day; LiveArm expires it
};

// What one paper trade becomes as a real order, or why not. `code` is stable and
// countable like every other refusal code in this app.
struct LiveOrderPlan {
    bool ok = false;
    // "" | "live-scope" | "live-unresolved" | "live-fx-unknown" | "live-position-cap" |
    // "live-unsized"
    QString code;
    QString why;
    // instrumentId, isBuy, amount (ORDER currency), leverage, SL/TP amounts (order
    // currency) — what EtoroClient::openPosition is given.
    OrderRequest request;
    // The same figures as Money (REQ-N-008): what the validator and the audit see.
    OrderAmounts amounts;
};

struct LiveMirrorInputs {
    PaperTrade trade;
    double eurPerUsd = 0.0;   // 0 = unknown -> refused live-fx-unknown
    Currency accountCurrency = Currency::Usd;
    qint32 openLivePositionsForSymbol = 0;
    // A second ceiling ALREADY in the order currency — the armed session's own per-order
    // cap, fixed at the rate of the moment it was granted (LiveArm). Invalid = none. It
    // exists because the EUR cap is converted at the CURRENT rate: a euro that
    // strengthened since the arming would otherwise make a plan a few cents over the
    // arm's cap, and the guarded send would refuse every order for the rest of the day.
    Money orderCurrencyCap;
};

[[nodiscard]] LiveOrderPlan liveOrderFor(const LiveMirrorInputs &in, const LiveMirrorConfig &cfg);

// Realised live P/L of the day (account currency), and whether it has reached the loss
// cap. A new calendar date starts a new ledger — a daily cap that only resets on
// restart is not a daily cap (the same rule LiveArm applies to its stake ledger).
struct LiveDay {
    QDate date;
    Money realized;
};

// The ledger after booking one closed position's realised P/L at `now`.
[[nodiscard]] LiveDay liveDayAfterClose(const LiveDay &day, const QDateTime &now,
                                        const Money &closedPnl);
// True exactly when today's realised loss is at or beyond the cap: -250.00 against a
// 250.00 cap trips, -249.99 does not, and a ledger of another date is yesterday's news.
// An invalid cap or an invalid ledger never trips — but note the executor refuses to
// ARM without a valid cap, so "invalid cap" cannot become "no cap" in practice.
[[nodiscard]] bool liveDailyLossReached(const LiveDay &day, const QDateTime &now,
                                        const Money &maxDailyLoss);

// The ConfirmGate action string naming the WHOLE grant — scope, both caps and the
// duration — so the two presses of REQ-N-005 confirm exactly what they permit and a
// changed cap is a different action.
[[nodiscard]] QString liveGrantAction(const LiveMirrorConfig &cfg);

}   // namespace trading

#endif   // TRADINGAPP_DOMAIN_LIVEMIRROR_H
