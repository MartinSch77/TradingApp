# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# -*- coding: utf-8 -*-
# The index-heavyweight window (REQ-F-035): ten names per index, side by side, as an
# early read on where SPX500 and NSDQ100 may go.
#
# What this checks that a unit test cannot: the window is reachable from the main
# window, both tables are built with a row per constituent, and the CAVEAT is on
# screen. That last one matters — the view is a stand-in for market breadth, and a
# stand-in presented as the real thing is worse than an honest gap.

import names


def main():
    startApplication("TradingApp")
    test.verify("SIMULATION" in str(waitForObject(names.modeLabel).text),
                "the run must be in simulation")

    clickButton(waitForObject(names.heavyButton))
    # The window is identified by its CONTENT rather than by the dialog object: the
    # class lives in namespace trading::ui and the wrapper reports it differently on
    # different builds, while the objectNames of its widgets are exactly the stable
    # handles REQ-N-007 exists to provide.
    nasdaq = waitForObject(names.nasdaqHeavyTable)
    test.verify(nasdaq is not None, "the heavyweights window opened")

    # 1. Both indices get their own table, each with a row per constituent — ten,
    #    whether or not their prices have arrived yet.
    sp = waitForObject(names.spHeavyTable)
    test.compare(nasdaq.rowCount, 10, "the Nasdaq-100 table lists ten constituents")
    test.compare(sp.rowCount, 10, "the S&P 500 table lists ten constituents")
    test.verify(nasdaq.columnCount >= 3, "name, session move and state")

    # 2. Each side states a summary, and a verdict is drawn from the two together.
    test.verify(len(str(waitForObject(names.nasdaqHeavySummary).text)) > 0,
                "Nasdaq summary: %s" % str(waitForObject(names.nasdaqHeavySummary).text)[:100])
    test.verify(len(str(waitForObject(names.spHeavySummary).text)) > 0,
                "S&P summary: %s" % str(waitForObject(names.spHeavySummary).text)[:100])
    test.verify(len(str(waitForObject(names.heavyVerdict).text)) > 0,
                "a verdict is stated")

    # 3. The honesty rule is ON SCREEN, not in a comment: this is a stand-in for
    #    breadth, and an unread price is shown as unknown rather than as 0.00 %.
    caveat = str(waitForObject(names.heavyCaveat).text)
    test.verify("STAND-IN" in caveat.upper(),
                "the window names itself a stand-in for breadth")
    test.verify("unknown" in caveat,
                "the window states that an unread price is shown as unknown")
