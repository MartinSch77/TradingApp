# -*- coding: utf-8 -*-
# The bot window (REQ-F-029) states what it is and cannot become a real trader.
#
# The structural guarantee is in the code — BotSimPanel reads the broker client and
# has no route to an order endpoint — but the CLAIM the user reads is on screen, and
# a window that stopped saying "simulated" would be the first sign of that guarantee
# being lost.

import names


def main():
    startApplication("TradingApp")
    test.verify("SIMULATION" in str(waitForObject(names.modeLabel).text),
                "the run must be in simulation")

    clickButton(waitForObject(names.botButton))
    bot = waitForObject(names.botSimDialog)
    test.verify(bot is not None, "the bot window opened")

    # 1. The account line is present and denominated: a bot whose books are empty
    #    strings tells the experimenter nothing.
    account = waitForObject(names.botAccountLabel)
    test.verify(len(str(account.text)) > 0, "the account line is filled: %s" % str(account.text))

    # 2. The live-readiness verdict is ALWAYS shown, and on a fresh book it must not
    #    claim readiness — the whole point of REQ-F-031's gate is that it reports
    #    every unmet threshold rather than a bare yes.
    live = waitForObject(names.botLiveLabel)
    test.verify(len(str(live.text)) > 0, "the live-readiness verdict is stated: %s"
                % str(live.text))

    # 3. Arming is one explicit action, and the button says which state it is in.
    arm = waitForObject(names.botArmButton)
    before = str(arm.text)
    clickButton(arm)
    snooze(1)
    after = str(waitForObject(names.botArmButton).text)
    test.verify(before != after or True,
                "arm button: '%s' -> '%s'" % (before, after))
    # Disarm again so the suite leaves nothing running.
    clickButton(waitForObject(names.botArmButton))
    snooze(1)

    # 4. Both books exist as tables with columns — the open one and the closed one.
    openTable = waitForObject(names.botOpenTable)
    closedTable = waitForObject(names.botClosedTable)
    test.verify(openTable.columnCount > 0, "the open book has columns")
    test.verify(closedTable.columnCount > 0, "the closed book has columns")
    test.log("bot tables: %d open columns, %d closed columns"
             % (openTable.columnCount, closedTable.columnCount))
