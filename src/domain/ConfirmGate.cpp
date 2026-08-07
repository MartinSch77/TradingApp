// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/ConfirmGate.h"

namespace trading {

ConfirmDecision confirmPress(const ConfirmGate &gate, const QString &action, qint64 nowMs,
                             qint64 windowMs)
{
    ConfirmDecision out;

    const qint64 elapsed = nowMs - gate.armedAtMs;
    // A negative elapsed means the clock moved backwards (an NTP step, a resume from
    // suspend). It is treated as EXPIRED, never as "within the window": the safe answer to
    // "I cannot tell how long ago this was armed" is to make the user press again.
    const bool fresh = (!gate.action.isEmpty()) && (elapsed >= 0) && (elapsed < windowMs);

    if (fresh && (gate.action == action)) {
        out.commit = true;
        // Cleared, not left armed. Otherwise a third press within the window would send a
        // second order having been confirmed once — the gate would count one press per
        // order after the first.
        out.next = ConfirmGate{};
        return out;
    }

    // Everything else arms: a first press, a different action, or a stale one. Note that a
    // stale same-action press lands here rather than committing, which is the whole reason
    // the window is checked before the comparison.
    out.next = ConfirmGate{action, nowMs};
    out.prompt = QStringLiteral("Press again within %1 s to %2.")
                     .arg(static_cast<double>(windowMs) / 1000.0, 0, 'g', 2)
                     .arg(action);
    return out;
}

} // namespace trading
