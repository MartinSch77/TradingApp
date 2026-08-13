// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The double-press gate (REQ-N-005, DES-DOM-GATE).
//
// This is the check that stands between a stray keypress and a real order, so it is tested
// harder than its size suggests. Every rule below describes a way the gate could let money
// move on ONE press, which is exactly what it exists to prevent.

#include "domain/ConfirmGate.h"

#include <QtTest/QtTest>

using namespace trading;

namespace {
constexpr qint64 kWindow = 1000;
// QLatin1StringView, not QString: a namespace-scope QString is dynamically initialised
// before main() and an allocation failure there cannot be caught (cert-err58-cpp). These are
// constexpr, so there is no dynamic initialisation at all, and QString converts from them
// implicitly at the call sites.
constexpr auto kBuy = QLatin1StringView("BUY 500.00 at x5");
constexpr auto kSell = QLatin1StringView("SELL 500.00 at x5");
} // namespace

class TestConfirmGate : public QObject
{
    Q_OBJECT;

private slots:

    //! @tstid TS-GATE-001 @design DES-DOM-GATE
    // @relation(REQ-N-005, scope=function)
    //
    // One press never moves money; two of the same, inside the window, do.
    void TS_GATE_001_onePressArmsAndTwoCommit()
    {
        const ConfirmDecision first = confirmPress(ConfirmGate{}, kBuy, 1000, kWindow);
        QVERIFY(!first.commit);
        // The arming is VISIBLE. An arming nobody can see is indistinguishable from a
        // control that did nothing — the failure this project already measured with the
        // quick keys, where a swallowed press "seemed dead".
        QVERIFY(!first.prompt.isEmpty());
        QVERIFY(first.prompt.contains(kBuy));
        QCOMPARE(first.next.action, kBuy);

        const ConfirmDecision second = confirmPress(first.next, kBuy, 1500, kWindow);
        QVERIFY(second.commit);
        // Cleared, NOT left armed: otherwise a third press would send a second order on one
        // press, and the gate would cost two presses only for the first order ever placed.
        QVERIFY(second.next.action.isEmpty());
    }

    //! @tstid TS-GATE-002 @design DES-DOM-GATE
    // @relation(REQ-N-005, scope=function)
    //
    // A STALE arming never fires. The user who pressed BUY two minutes ago is not
    // necessarily still at the screen, so the window is checked BEFORE the actions are
    // compared — the opposite order would commit here.
    void TS_GATE_002_aStaleArmingRearmsInsteadOfCommitting()
    {
        const ConfirmGate armed{kBuy, 1000};

        const ConfirmDecision late = confirmPress(armed, kBuy, 1000 + kWindow, kWindow);
        QVERIFY(!late.commit);                 // exactly at the window is already too late
        QCOMPARE(late.next.armedAtMs, 1000 + kWindow);   // and the clock restarts

        const ConfirmDecision muchLater = confirmPress(armed, kBuy, 500000, kWindow);
        QVERIFY(!muchLater.commit);

        // A clock that moved BACKWARDS (NTP step, resume from suspend) is treated as
        // expired, never as "within the window". The safe answer to "I cannot tell how long
        // ago this was armed" is to make the user press again.
        const ConfirmDecision rewound = confirmPress(armed, kBuy, 200, kWindow);
        QVERIFY(!rewound.commit);
    }

    //! @tstid TS-GATE-003 @design DES-DOM-GATE
    // @relation(REQ-N-005, scope=function)
    //
    // A press of a DIFFERENT action re-arms rather than committing. This is the rule that
    // stops a confirmation being harvested by a different order: pressing BUY then SELL
    // must not sell, and neither must confirming a BUY of 500 send a BUY of 5000.
    void TS_GATE_003_aDifferentActionNeverInheritsTheConfirmation()
    {
        const ConfirmGate armedBuy{kBuy, 1000};

        const ConfirmDecision opposite = confirmPress(armedBuy, kSell, 1200, kWindow);
        QVERIFY(!opposite.commit);
        QCOMPARE(opposite.next.action, kSell);   // armed for the NEW action

        // Same side, different size: still a different order, so still no commit.
        const ConfirmDecision resized =
            confirmPress(armedBuy, QStringLiteral("BUY 5000.00 at x5"), 1200, kWindow);
        QVERIFY(!resized.commit);

        // Same side and size, different leverage: likewise.
        const ConfirmDecision relevered =
            confirmPress(armedBuy, QStringLiteral("BUY 500.00 at x20"), 1200, kWindow);
        QVERIFY(!relevered.commit);

        // An empty gate cannot commit anything, whatever it is asked.
        QVERIFY(!confirmPress(ConfirmGate{}, kBuy, 1000, kWindow).commit);
        // …not even for an empty action, which would otherwise "match" the empty gate.
        QVERIFY(!confirmPress(ConfirmGate{}, QString(), 1000, kWindow).commit);
    }

    //! @tstid TS-GATE-005 @design DES-DOM-GATE
    // @relation(REQ-N-005, scope=function)
    //
    // Two boundaries, exactly on the line rather than clearly past it (Mull
    // mutation-testing pilot, 2026-08-13). elapsed == 0 (the second press landing on
    // the SAME millisecond as the arming) must still be fresh enough to commit — a
    // mutated >= -> > would refuse this one, which is a real case (two presses can
    // land in the same tick on a fast machine or in a test), not a hypothetical one.
    void TS_GATE_005_zeroElapsedStillCommits()
    {
        const ConfirmDecision armed = confirmPress(ConfirmGate{}, kBuy, 1000, kWindow);
        const ConfirmDecision second = confirmPress(armed.next, kBuy, /*nowMs=*/1000, kWindow);
        QVERIFY(second.commit);
    }

    //! @tstid TS-GATE-006 @design DES-DOM-GATE
    // @relation(REQ-N-005, scope=function)
    //
    // The prompt's seconds figure is windowMs DIVIDED by 1000.0, not multiplied (Mull
    // pilot): pinned with a windowMs whose divided and multiplied results are nowhere
    // near each other (2500 ms -> "2.5", not "2.5e+6") so a mutated / -> * cannot pass
    // by coincidence.
    void TS_GATE_006_promptSecondsIsDivisionNotMultiplication()
    {
        const ConfirmDecision armed = confirmPress(ConfirmGate{}, kBuy, 1000, /*windowMs=*/2500);
        QVERIFY(armed.prompt.contains(QStringLiteral("2.5")));
        QVERIFY(!armed.prompt.contains(QStringLiteral("2.5e")));
    }
};

QTEST_MAIN(TestConfirmGate)
#include "tst_confirmgate.moc"
