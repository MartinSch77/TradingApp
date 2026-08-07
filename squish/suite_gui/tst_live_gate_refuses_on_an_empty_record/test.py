# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# -*- coding: utf-8 -*-
# The real-money gate (REQ-F-037 / REQ-N-005) must REFUSE on an empty record, name
# what is missing, and say on screen that live execution is not wired.
#
# What this checks that a unit test cannot: paperLiveReadiness() returning
# ready=False is worth nothing if the window renders it as encouragement. This is
# the one label that could talk somebody into trusting a bot that has measured
# nothing, so its wording is part of the safety property, not decoration.
#
# The record is empty by construction: envvars gives the suite its own
# XDG_CONFIG_HOME, so no developer's bot book is visible and the run starts from
# zero closed trades. That is what makes "not ready" the deterministic answer.
#
# @relation(REQ-F-037, scope=file)
# @relation(REQ-N-005, scope=file)
#
# The markers are for Test Center's repository mapping; see the note in
# tst_lead_signal_states_its_evidence on why a GUI test carries no TS id.

import names


def main():
    startApplication("TradingApp")
    test.verify("SIMULATION" in str(waitForObject(names.modeLabel).text),
                "the run must be in simulation")

    clickButton(waitForObject(names.botButton))
    live = str(waitForObject(names.botLiveLabel).text)
    test.verify(len(live) > 0, "the bot window states a live-money verdict")

    # 1. On a record with nothing in it, the gate REFUSES. The opposite reading —
    #    "criteria met" — must not appear, because an empty record cannot clear a
    #    threshold it was never measured against.
    test.verify("Not ready for real money" in live,
                "the gate refuses on an empty record: %s" % live[:200])
    test.verify("Real-money criteria met" not in live,
                "an empty record never reads as criteria met")

    # 2. It names WHY. A refusal without its blockers cannot be acted on, and would
    #    invite the reader to assume it is a formality.
    tail = live.split("—", 1)
    test.verify(len(tail) > 1 and len(tail[1].strip()) > 0,
                "the refusal names its blockers: %s" % live[:200])

    # 3. The safety fact is on screen in both branches of that label: live execution
    #    is NOT wired in this build. REQ-N-005 is not satisfied by the code alone if
    #    the window lets a reader believe the bot can place a real order.
    test.verify("not wired" in live or "NOT wired" in live,
                "the window states that live execution is not wired: %s" % live[:200])
    test.verify("simulated money" in live,
                "the window states the bot trades simulated money")

    # 4. The record label agrees with the verdict: zero closed trades. Two labels
    #    disagreeing about the same book is the failure this pins.
    record = str(waitForObject(names.botRecordLabel).text)
    test.verify("Record" in record, "the record is stated: %s" % record[:160])
    test.verify(record.count("0 closed") > 0 or " 0 " in record,
                "an untouched book reports no closed trades: %s" % record[:160])

    # 5. And the bot is NOT armed by a test run. envvars sets TRADINGAPP_BOT_AI=off
    #    and does not arm it; an armed bot in a GUI suite would open simulated
    #    positions on live prices while nobody is watching the assertions.
    arm = waitForObject(names.botArmButton)
    test.verify("arm" in str(arm.text).lower(),
                "the arm button still offers to ARM, so the bot is idle: %s"
                % str(arm.text))
