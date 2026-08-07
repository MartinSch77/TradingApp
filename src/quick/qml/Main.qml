// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Layouts
import TradingApp.Cockpit

// The cockpit root, hosted in a QQuickWidget inside the existing Widgets window
// (REQ-F-038, DES-UI-COCKPIT).
//
// READ-ONLY BY CONSTRUCTION. There is no order path here and there is deliberately nothing
// to add one to: amount, leverage, SL/TP and the REQ-N-005 double-press stay in the Widgets
// interface, which is the surface that has been GUI-tested, object-name-gated and audited.
// A second place where money can move would double the surface needing that scrutiny while
// halving the attention paid to each.
//
// `cockpit` is the CockpitModel, injected as a required property by CockpitPanel. Every
// string below is already formatted in C++ and covered by tst_cockpitmodel — nothing here
// computes a value.
Rectangle {
    id: root

    // Set by CockpitPanel via setInitialProperties BEFORE the component loads, rather than as
    // a rootContext property. Measured reason: as a context property the value read as NULL
    // in the bindings created for child components ("Cannot read property 'ticks' of null"),
    // while the ones on this file's own objects resolved. A required property is evaluated as
    // part of construction, so the timing is not a question.
    required property var cockpit

    color: Theme.surface

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: Theme.gap

        // The SIMULATION banner is not decoration: it is how a reader knows no order can
        // reach an account. Unmissable by design — a combo box switching to live money
        // would undercut the double-press gate.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 26
            radius: Theme.radius
            color: root.cockpit.simulation ? Theme.reference : Theme.down

            Text {
                anchors.centerIn: parent
                text: root.cockpit.simulation
                      ? qsTr("SIMULATION — read-only view, no order can be placed from here")
                      : qsTr("REAL ACCOUNT — this view is still read-only")
                color: "#ffffff"
                font.pixelSize: 12
                font.bold: true
            }
        }

        Text {
            text: root.cockpit.instrument.length > 0 ? root.cockpit.instrument : qsTr("no instrument")
            color: Theme.ink
            font.pixelSize: 22
            font.bold: true
        }

        // Market cards.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            Repeater {
                model: root.cockpit.cards

                MarketCard {
                    required property var modelData
                    symbol: modelData.symbol
                    price: modelData.price
                    changePct: modelData.changePct
                    freshnessLabel: modelData.freshnessLabel
                    dir: modelData.dir
                }
            }

            Item { Layout.fillWidth: true }
        }

        // Evidence column. The chart belongs beside this and is the next piece of work
        // (QCustomSeries); leaving the space empty is honest until it exists.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.gap

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.panel
                radius: Theme.radius
                border.width: 1
                border.color: Theme.gridline

                Text {
                    anchors.centerIn: parent
                    text: qsTr("price chart — QCustomSeries candlesticks go here")
                    color: Theme.inkMuted
                    font.pixelSize: 12
                    font.italic: true
                }
            }

            ConfluenceMeter {
                Layout.fillHeight: true
                ticks: root.cockpit.ticks
                agreement: root.cockpit.agreement
                evidence: root.cockpit.evidence
                probability: root.cockpit.probability
            }
        }
    }
}
