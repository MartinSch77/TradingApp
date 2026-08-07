// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_ORDERREQUESTVALIDATOR_H
#define TRADINGAPP_DOMAIN_ORDERREQUESTVALIDATOR_H

#include "domain/Models.h"
#include "domain/Money.h"

#include <QList>
#include <QString>

// The last pure check before real money moves (REQ-N-009).
//
// The double-press gate of REQ-N-005 keeps a human in the loop; it does not make the
// request CORRECT. This validator does one job: given a request and the facts about
// the instrument and the account, it says whether the request is sendable — and when
// it is not, it REFUSES rather than repairs. Silently clamping a leverage or moving a
// stop is how an order that a person authorised turns into a different order than the
// one they saw.
//
// Every refusal carries a stable machine-readable code beside its sentence. That is
// deliberate and matches the bot's refusal codes: a reason only a human can read
// cannot be counted, and the count is what tells a maintainer which check actually
// fires in practice.
namespace trading {

// What the validator needs to know that the request itself does not carry. Anything
// left at its default is UNKNOWN, and an unknown fact disables its own check rather
// than inventing a bound — the same rule the bot's cost gate follows.
struct OrderContext {
    // The account's own currency. The order currency must equal it: eToro rejects a
    // EUR order on a USD account at execution time, i.e. after it looked accepted.
    Currency accountCurrency = Currency::Invalid;
    Currency orderCurrency = Currency::Invalid;
    // The instrument being traded, resolved. `maxUnitsPerOrder` (from the eligibility
    // endpoint) caps the units; 0 = unknown.
    Instrument instrument;
    // The leverage steps this instrument actually offers — a LADDER, not a range: an
    // instrument offering 1/2/5/20 does not sell x8, and asking for it produces an
    // order eToro accepts and then rejects.
    QList<qint32> leverageLadder;
    // The current market rate, needed to judge which side a stop, a target or a
    // trigger is on. 0 = unknown, which disables those three checks (a market order
    // placed without a live quote is refused by its own check instead).
    double marketRate = 0.0;
    // Bounds the account itself imposes, as exact amounts. An invalid amount = no
    // bound.
    Money minStake;
    Money maxStakePerOrder;
    // What today has already committed, and the cap the armed session was granted
    // under (REQ-N-009's arming). Both invalid = no daily bound.
    Money committedToday;
    Money maxStakePerDay;
};

// The exact amounts a request will be sent with (REQ-N-008), as one value: they travel
// together through every check, and a signature that takes them separately invites the
// call that passes a stop where a target belongs.
struct OrderAmounts {
    Money stake;
    Money stopLoss;
    Money takeProfit;
};

// One reason a request is not sendable. `code` is stable and countable; `message` is
// the sentence a user sees.
struct OrderProblem {
    QString code;
    QString message;
};

struct OrderValidation {
    QList<OrderProblem> problems;

    [[nodiscard]] bool ok() const { return problems.isEmpty(); }
    // Every code, in the order the checks ran — for logging and for tests that assert
    // WHICH check fired rather than merely that something did.
    [[nodiscard]] QStringList codes() const;
    // The problems as one line, for a message box or a log entry.
    [[nodiscard]] QString summary() const;
};

// The whole check. `amounts` carries the exact figures the request will be sent with
// (REQ-N-008), `request` the rest. Taking the amounts as Money rather than reading
// OrderRequest's doubles is the point: a figure that never was an exact number of
// minor units must not reach this function looking like one, so the caller has to have
// converted it deliberately.
[[nodiscard]] OrderValidation validateOrderRequest(const OrderRequest &request,
                                                   const OrderAmounts &amounts,
                                                   const OrderContext &context);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_ORDERREQUESTVALIDATOR_H
