# -*- coding: utf-8 -*-
# Object map, written by hand against the objectNames the app sets (REQ-N-007).
#
# Every entry below addresses a widget by its objectName, never by its text or its
# position: wording changes with translation and position changes with layout, while
# an objectName only changes when someone renames the member — which
# tools/check_object_names.py then reports.
#
# The names come from the member names in src/ui/*.cpp: `m_buyButton` is
# "buyButton". Adding a widget there and running that checker is the whole workflow.

mainWindow = {"type": "MainWindow", "unnamed": 1, "visible": 1}

# --- the mode badge, which is what every safety assertion reads ---------------
modeBadge = {"container": mainWindow, "type": "QLabel", "objectName": "modeBadge"}

# --- instrument + trade panel -------------------------------------------------
instrumentBox = {"container": mainWindow, "type": "QComboBox", "objectName": "instrumentBox"}
amount = {"container": mainWindow, "type": "QDoubleSpinBox", "objectName": "amount"}
leverage = {"container": mainWindow, "type": "QComboBox", "objectName": "leverage"}
stopLoss = {"container": mainWindow, "type": "QDoubleSpinBox", "objectName": "stopLoss"}
takeProfit = {"container": mainWindow, "type": "QDoubleSpinBox", "objectName": "takeProfit"}
buyButton = {"container": mainWindow, "type": "QPushButton", "objectName": "buyButton"}
sellButton = {"container": mainWindow, "type": "QPushButton", "objectName": "sellButton"}

# --- the windows a GUI test opens --------------------------------------------
signalsToggle = {"container": mainWindow, "type": "QPushButton", "objectName": "signalsToggle"}
decisionButton = {"container": mainWindow, "type": "QPushButton", "objectName": "decisionButton"}
screenerButton = {"container": mainWindow, "type": "QPushButton", "objectName": "screenerButton"}
closedButton = {"container": mainWindow, "type": "QPushButton", "objectName": "closedButton"}
scriptButton = {"container": mainWindow, "type": "QPushButton", "objectName": "scriptButton"}
botButton = {"container": mainWindow, "type": "QPushButton", "objectName": "botButton"}
chartToggle = {"container": mainWindow, "type": "QPushButton", "objectName": "chartToggle"}

# --- open trades --------------------------------------------------------------
positions = {"container": mainWindow, "type": "QTableView", "objectName": "positions"}

# --- the bot simulation window ------------------------------------------------
botSimDialog = {"type": "BotSimDialog", "unnamed": 1, "visible": 1}
botArmButton = {"container": botSimDialog, "type": "QPushButton", "objectName": "armButton"}
botResetButton = {"container": botSimDialog, "type": "QPushButton", "objectName": "resetButton"}
botOpenTable = {"container": botSimDialog, "type": "QTableWidget", "objectName": "openTable"}
botClosedTable = {"container": botSimDialog, "type": "QTableWidget", "objectName": "closedTable"}
