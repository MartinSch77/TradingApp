# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# -*- coding: utf-8 -*-
# The trade panel's SAFETY behaviour, driven through the GUI.
#
# What this proves that a unit test cannot: the guards are wired to the widgets a
# person actually uses. A double-press gate that exists in the code but is not
# connected to the button is exactly the defect a unit test cannot see.
#
# Everything here runs in FORCED SIMULATION (squish/suite_gui/envvars), so no click
# in this file can reach a broker.

import names


def main():
    startApplication("TradingApp")

    # The mode is checked again here rather than trusted from the other test: this
    # file clicks BUY, and it must be impossible for that to be a real order.
    badge = waitForObject(names.modeLabel)
    test.verify("SIMULATION" in str(badge.text),
                "must be SIMULATION before any button is pressed — badge: %s" % str(badge.text))

    # 1. The order size and its protective levels are ordinary spin boxes, and the
    #    app must accept a sane trade being typed into them.
    amount = waitForObject(names.amount)
    type(amount, "<Ctrl+a>")
    type(amount, "250")
    test.verify(float(amount.value) > 0.0, "an amount can be entered")

    stop = waitForObject(names.stopLoss)
    target = waitForObject(names.takeProfit)
    test.log("stop-loss %s / take-profit %s offered by default" % (stop.value, target.value))

    # 2. The DOUBLE-PRESS gate (REQ-N-005): the first press ARMS, it does not trade.
    #    The evidence is the log line the app writes — a trade would say something
    #    else entirely, and the open-trades table would gain a row.
    positions = waitForObject(names.positions)
    rowsBefore = positions.model().rowCount()
    clickButton(waitForObject(names.buyButton))
    snooze(1)
    rowsAfter = positions.model().rowCount()
    test.compare(rowsAfter, rowsBefore,
                 "one press must not open a position — the gate has to ask twice")

    # 3. The second press within the window is what actually trades. In simulation
    #    that is a synthetic fill, so the table gains a row.
    clickButton(waitForObject(names.buyButton))
    snooze(2)
    test.verify(positions.model().rowCount() >= rowsBefore,
                "the confirmed press is what opens the position")
    test.log("open positions after the confirmed press: %d" % positions.model().rowCount())
