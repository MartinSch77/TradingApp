// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_CONFIRMGATE_H
#define TRADINGAPP_DOMAIN_CONFIRMGATE_H

#include <QString>

// The double-press gate that stands in front of every money-moving action (REQ-N-005).
//
// WHY THIS IS A DOMAIN MODULE AND NOT A FEW LINES IN A WINDOW. The rule used to live inline
// in MainWindow::handleQuickKey, which was fine while exactly one surface could place an
// order. A second front end that can trade needs the same rule, and the obvious way to give
// it one — write the four lines again — produces two gates that can drift. A safety
// mechanism with two implementations has, in practice, the weaker of the two.
//
// So the rule is stated once, purely, and both front ends call it. It is also then testable
// without a window, which matters more here than anywhere else in the application: this is
// the check that stands between a stray keypress and a real order.
//
// The rule itself, in full:
//
//   * The FIRST press arms, and names what it armed. It never moves money.
//   * A SECOND press of the SAME action, within the window, commits — and CLEARS the gate,
//     so the next order needs two fresh presses rather than inheriting this one's arming.
//   * A press of a DIFFERENT action re-arms for that action. It never commits, because the
//     thing the user confirmed is not the thing that would be sent.
//   * A same-action press AFTER the window arms again rather than committing. A stale arm
//     must never fire: the user who pressed BUY two minutes ago is not necessarily still
//     looking at the screen.
//
// Nothing here decides whether the order is CORRECT — that is OrderRequestValidator's job
// (REQ-N-009). This gate only answers "did a human ask for this, twice, just now".
namespace trading {

// What is currently armed. An empty action means nothing is.
struct ConfirmGate {
    QString action;
    qint64 armedAtMs = 0;
};

// What a press should do.
struct ConfirmDecision {
    // True only on a valid second press. The caller may move money exactly when this is
    // true, and must not consult anything else in this struct to decide that.
    bool commit = false;
    // What to tell the user. Non-empty when arming, because an arming that is not visible
    // is indistinguishable from a control that did nothing — this project already measured
    // that failure with the quick keys, where a swallowed keypress "seemed dead".
    QString prompt;
    // The gate state the caller must keep. Assigning this back is not optional: a caller
    // that forgets it either never arms or never disarms.
    ConfirmGate next;
};

// `action` identifies what would be sent, precisely enough that two different orders are
// two different strings — "BUY SPX500 x5" rather than "BUY". Two presses that mean
// different things must not combine into one confirmation.
[[nodiscard]] ConfirmDecision confirmPress(const ConfirmGate &gate, const QString &action,
                                           qint64 nowMs, qint64 windowMs);

// The default window. One second is what the quick keys have always used, and it is short
// on purpose: it is long enough for a deliberate double press and too short to be reached
// by an accidental one.
inline constexpr qint64 kConfirmWindowMs = 1000;

} // namespace trading

#endif // TRADINGAPP_DOMAIN_CONFIRMGATE_H
