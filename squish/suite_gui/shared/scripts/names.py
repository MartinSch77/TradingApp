# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# -*- coding: utf-8 -*-
# Object map, written by hand against the objectNames the app sets (REQ-N-007).
#
# Every entry addresses a widget by its objectName, never by its text or its position:
# wording changes with translation and position changes with layout, while an
# objectName only changes when someone renames the member — which
# tools/check_object_names.py then reports.
#
# The names come from the member names in src/ui/*.cpp: `m_buyButton` is "buyButton".
# Adding a widget there and running that checker is the whole workflow.
#
# VERIFIED against a running app (Squish 9.2.2), not guessed: the first version of
# this file invented `modeBadge` and the run failed with "Object 'modeBadge' not
# found", because the app calls it `modeLabel`. An object map is only worth having
# when it addresses what actually exists.

import time

mainWindow = {"type": "MainWindow", "unnamed": 1, "visible": 1}

# --- the mode label, which is what every safety assertion reads ---------------
modeLabel = {"container": mainWindow, "type": "QLabel", "objectName": "modeLabel"}
titleLabel = {"container": mainWindow, "type": "QLabel", "objectName": "titleLabel"}
priceLabel = {"container": mainWindow, "type": "QLabel", "objectName": "priceLabel"}
cashLabel = {"container": mainWindow, "type": "QLabel", "objectName": "cashLabel"}

# --- instrument + trade panel -------------------------------------------------
instrumentBox = {"container": mainWindow, "type": "QComboBox", "objectName": "instrumentBox"}
amount = {"container": mainWindow, "type": "QDoubleSpinBox", "objectName": "amount"}
leverage = {"container": mainWindow, "type": "QComboBox", "objectName": "leverage"}
stopLoss = {"container": mainWindow, "type": "QDoubleSpinBox", "objectName": "stopLoss"}
takeProfit = {"container": mainWindow, "type": "QDoubleSpinBox", "objectName": "takeProfit"}
trailingStop = {"container": mainWindow, "type": "QCheckBox", "objectName": "trailingStop"}
buyButton = {"container": mainWindow, "type": "QPushButton", "objectName": "buyButton"}
sellButton = {"container": mainWindow, "type": "QPushButton", "objectName": "sellButton"}
closeButton = {"container": mainWindow, "type": "QPushButton", "objectName": "closeButton"}
limitBuyButton = {"container": mainWindow, "type": "QPushButton", "objectName": "limitBuyButton"}
limitSellButton = {"container": mainWindow, "type": "QPushButton",
                   "objectName": "limitSellButton"}
openCost = {"container": mainWindow, "type": "QLabel", "objectName": "openCost"}

# --- the windows a GUI test opens --------------------------------------------
signalsToggle = {"container": mainWindow, "type": "QPushButton", "objectName": "signalsToggle"}
decisionButton = {"container": mainWindow, "type": "QPushButton", "objectName": "decisionButton"}
screenerButton = {"container": mainWindow, "type": "QPushButton", "objectName": "screenerButton"}
closedButton = {"container": mainWindow, "type": "QPushButton", "objectName": "closedButton"}
scriptButton = {"container": mainWindow, "type": "QPushButton", "objectName": "scriptButton"}
botButton = {"container": mainWindow, "type": "QPushButton", "objectName": "botButton"}
chartToggle = {"container": mainWindow, "type": "QPushButton", "objectName": "chartToggle"}

# --- open trades and the log --------------------------------------------------
positions = {"container": mainWindow, "type": "QTableView", "objectName": "positions"}
pendingTable = {"container": mainWindow, "type": "QTableWidget", "objectName": "pendingTable"}
log = {"container": mainWindow, "type": "QTextEdit", "objectName": "log"}
openTradesSummary = {"container": mainWindow, "type": "QLabel",
                     "objectName": "openTradesSummary"}

# --- the signals panel --------------------------------------------------------
signalsWindow = {"objectName": "signalsWindow", "visible": 1}
sigOverall = {"type": "QLabel", "objectName": "sigOverall", "visible": 1}
sigConfluence = {"type": "QLabel", "objectName": "sigConfluence", "visible": 1}
sigLocalAi = {"type": "QLabel", "objectName": "sigLocalAi", "visible": 1}

# --- the decision window ------------------------------------------------------
decisionDialog = {"objectName": "decisionDialog", "visible": 1}
# Addressed WITHOUT a container: these objectNames are unique in the app, and a
# container clause only helps where a name repeats. It hurt here — the dialog is
# modeless and built lazily, so the container had to resolve before its children
# existed and the lookup failed with "Could not match properties: container".
decisionRanked = {"type": "QTableWidget", "objectName": "decisionRanked", "visible": 1}
decisionSources = {"type": "QTableWidget", "objectName": "decisionSources", "visible": 1}
decisionRefresh = {"type": "QPushButton", "objectName": "decisionRefresh", "visible": 1}
decisionConclusion = {"type": "QLabel", "objectName": "decisionConclusion", "visible": 1}

# --- the closed-trades window -------------------------------------------------
closedDialog = {"objectName": "closedDialog", "visible": 1}
closedTable = {"container": closedDialog, "type": "QTableWidget", "objectName": "closedTable"}
closedRefresh = {"container": closedDialog, "type": "QPushButton", "objectName": "closedRefresh"}
closedSummary = {"container": closedDialog, "type": "QLabel", "objectName": "closedSummary"}

# --- the bot simulation window ------------------------------------------------
botSimDialog = {"type": "BotSimDialog", "unnamed": 1, "visible": 1}
botArmButton = {"container": botSimDialog, "type": "QPushButton", "objectName": "armButton"}
botResetButton = {"container": botSimDialog, "type": "QPushButton", "objectName": "resetButton"}
botTrainButton = {"container": botSimDialog, "type": "QPushButton", "objectName": "trainButton"}
botAiModeBox = {"container": botSimDialog, "type": "QComboBox", "objectName": "aiModeBox"}
botOpenTable = {"container": botSimDialog, "type": "QTableWidget", "objectName": "openTable"}
botClosedTable = {"container": botSimDialog, "type": "QTableWidget", "objectName": "closedTable"}
botAccountLabel = {"container": botSimDialog, "type": "QLabel", "objectName": "accountLabel"}
botLiveLabel = {"container": botSimDialog, "type": "QLabel", "objectName": "liveLabel"}
botRecordLabel = {"container": botSimDialog, "type": "QLabel", "objectName": "recordLabel"}
botReasonLabel = {"container": botSimDialog, "type": "QLabel", "objectName": "reasonLabel"}
botLog = {"container": botSimDialog, "type": "QTextEdit", "objectName": "log"}

# --- the index-heavyweight window (REQ-F-035) --------------------------------
heavyButton = {"container": mainWindow, "type": "QPushButton", "objectName": "heavyButton"}
# Addressed by TYPE, like botSimDialog: a top-level custom dialog is matched on its
# class by the Qt wrapper, and an objectName-only lookup for the window itself did
# not resolve (its children match by objectName perfectly well).
heavyweightsPanel = {"type": "HeavyweightsPanel", "visible": 1}
nasdaqHeavyTable = {"type": "QTableWidget", "objectName": "nasdaqHeavyTable", "visible": 1}
spHeavyTable = {"type": "QTableWidget", "objectName": "spHeavyTable", "visible": 1}
nasdaqHeavySummary = {"type": "QLabel", "objectName": "nasdaqHeavySummary", "visible": 1}
spHeavySummary = {"type": "QLabel", "objectName": "spHeavySummary", "visible": 1}
heavyVerdict = {"type": "QLabel", "objectName": "heavyVerdict", "visible": 1}
heavyCaveat = {"type": "QLabel", "objectName": "heavyCaveat", "visible": 1}
# The combined indication per index (REQ-F-036): the nine reads, the constituent
# field, the session phase and the regime, scored together.
nasdaqLeadSignal = {"type": "QLabel", "objectName": "nasdaqLeadSignal", "visible": 1}
spLeadSignal = {"type": "QLabel", "objectName": "spLeadSignal", "visible": 1}


def closeAppGracefully(win):
    """Every test.py's LAST call, so the AUT exits main() normally instead of
    being torn down by squishrunner between test cases.

    Takes the ALREADY-RESOLVED mainWindow object (waitForObject(names.mainWindow)
    in the caller) rather than resolving it here: waitForObject/snooze are names
    squishrunner injects into the TEST SCRIPT's own global namespace, not into a
    module it imports — calling waitForObject from inside this file itself failed
    with "NameError: name 'waitForObject' is not defined" the first time this was
    tried, which is how that distinction was found.

    WHY THIS EXISTS (GitHub issue #15 / RISK-001): squishrunner's test-case
    teardown TERMINATES an AUT started via startApplication() rather than asking
    it to quit gracefully. tools/squish_run.sh's own investigation record shows a
    manual `kill -TERM` sent to a live Coco-instrumented AUT mid-suite neither
    dumped a coverage report NOR terminated the process — something in Squish's
    hooked Qt runtime intercepts/swallows SIGTERM before it reaches Coco's or the
    default handler. Coco's CoverageScanner runtime writes its execution report
    from an atexit handler by default (Coco manual, "Control of execution report
    generation"), which fires only on a NORMAL end of the process — never on a
    forced kill.
    MainWindow::closeEvent (src/ui/MainWindow.cpp) closes the chart and signals
    windows too, so this one call unwinds every top-level window; with Qt's
    default quitOnLastWindowClosed that lets QApplication::exec() return, main()
    return, and the C++ runtime run its NORMAL exit() sequence — the one Coco's
    atexit hook is registered against. By the time squishrunner's own teardown
    looks for the AUT to stop, it has already exited on its own, and any .csexe
    an instrumented build wrote by then is real, not inferred.
    """
    win.close()
    # time.sleep, not snooze(): same reason as above — this module has no
    # squish-injected globals. Lets the event loop unwind (all top-level windows
    # close -> exec() returns -> main() returns -> exit()) rather than have
    # squishrunner's teardown catch the process mid-shutdown.
    time.sleep(2)
