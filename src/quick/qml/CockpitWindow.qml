// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Window
import TradingApp.Cockpit

// The standalone cockpit's window. Nothing but a frame around the SAME Main.qml the
// QQuickWidget panel inside TradingApp hosts — the two front ends share every pixel of the
// view and every line of CockpitModel, which is the whole point of the exercise: one domain,
// one view-model, two presentations.
Window {
    id: window

    required property var cockpit

    width: 1280
    height: 900
    visible: true
    color: Theme.surface
    title: qsTr("TradingApp — market cockpit (Qt Quick · Qt Graphs)")

    Main {
        anchors.fill: parent
        cockpit: window.cockpit
    }
}
