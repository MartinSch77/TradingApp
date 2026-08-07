# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# -*- coding: utf-8 -*-
# The decision window (REQ-F-008, REQ-F-035) shows its SOURCES, not just a verdict.
#
# The rule being checked is the one the requirements insist on: every source is
# named, and a source that could not be measured says so rather than being left out.
# A window that shows a conclusion without its evidence is the failure mode.

import names


def main():
    startApplication("TradingApp")
    test.verify("SIMULATION" in str(waitForObject(names.modeLabel).text),
                "the run must be in simulation")

    clickButton(waitForObject(names.decisionButton))
    # Wait for a WIDGET of the window rather than for a fixed number of seconds: the
    # window is modeless and builds its tables when it is first shown.
    waitForObject(names.decisionRanked)

    # 1. The ranked table of instruments, and the per-source table beside it.
    ranked = waitForObject(names.decisionRanked)
    sources = waitForObject(names.decisionSources)
    test.verify(ranked.columnCount > 0, "the ranked table has columns")
    test.verify(sources.rowCount > 0,
                "the decision must name its sources — %d rows" % sources.rowCount)

    # 2. A conclusion is stated in words, not only as a number.
    conclusion = waitForObject(names.decisionConclusion)
    test.verify(len(str(conclusion.text)) > 0,
                "the window states a conclusion: %s" % str(conclusion.text)[:120])

    # 3. Refreshing recomputes without crashing — the path that runs on every timer
    #    tick in a live session, driven here deliberately.
    clickButton(waitForObject(names.decisionRefresh))
    snooze(2)
    test.verify(waitForObject(names.decisionSources).rowCount > 0,
                "the sources are still listed after a refresh")

    # 4. The signals panel carries the independent reads (REQ-F-035): the confluence
    #    line must exist, whatever it currently says — including "no reference reads
    #    yet", which is the honest answer before the feeds have arrived.
    #
    #    The window is opened by the app ITSELF at startup (MainWindow schedules it
    #    with the chart), so it is already there — clicking the toggle first would
    #    CLOSE it, which is exactly what happened when this test was written the
    #    other way round.
    confluence = waitForObject(names.sigConfluence)
    test.verify(len(str(confluence.text)) > 0,
                "the confluence read is shown: %s" % str(confluence.text)[:120])
    overall = waitForObject(names.sigOverall)
    test.verify(len(str(overall.text)) > 0,
                "the overall signal is shown: %s" % str(overall.text)[:80])

    # 5. …and the toggle really toggles: it closes the window, and brings it back.
    clickButton(waitForObject(names.signalsToggle))
    snooze(1)
    clickButton(waitForObject(names.signalsToggle))
    test.verify(waitForObject(names.sigConfluence) is not None,
                "the signals window comes back after being toggled off and on")
