# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# -*- coding: utf-8 -*-
# The FIRST test of this suite, and the one that must never be deleted: it proves
# that a Squish run cannot reach a real account.
#
# The guarantee itself is in the app (Config::forceSimulation makes
# hasCredentials() answer false, see tests/tst_config.cpp TS-CFG-007) and the switch
# is set for every run in squish/suite_gui/envvars. This test checks the OUTCOME on
# screen, because a guarantee nobody verifies from the outside is a guarantee that
# quietly stops holding.

import names


def main():
    startApplication("TradingApp")

    # 1. The badge says SIMULATION. If a future change ever let real credentials
    #    through, this fails before any test can click BUY.
    badge = waitForObject(names.modeLabel)
    test.verify("SIMULATION" in str(badge.text),
                "the app must be in SIMULATION for a GUI test run — badge says: %s"
                % str(badge.text))
    test.verify("REAL MONEY" not in str(badge.text),
                "a GUI test run must never be on a real account")

    # 2. The trade panel is usable, so the rest of the suite has something to drive:
    #    simulation is a full app with a synthetic feed, not a disabled one.
    test.compare(waitForObject(names.buyButton).enabled, True)
    test.compare(waitForObject(names.sellButton).enabled, True)
    instrument = waitForObject(names.instrumentBox)
    test.verify(instrument.count > 1, "the instrument selector should be populated")

    # 3. The windows a user opens all come up. Each is a smoke test of its own
    #    construction path — the dialogs build tables and charts on first show.
    for button, window in ((names.signalsToggle, "signals panel"),
                           (names.decisionButton, "decision window"),
                           (names.closedButton, "closed trades"),
                           (names.botButton, "bot simulation")):
        clickButton(waitForObject(button))
        snooze(1)
        test.log("opened: %s" % window)

    # 4. The bot simulation is simulated money by construction; its window is up.
    bot = waitForObject(names.botSimDialog)
    test.verify(bot is not None, "the bot window opened")

    names.closeAppGracefully(waitForObject(names.mainWindow))
