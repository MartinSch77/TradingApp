// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import TradingApp.Cockpit

// The open book, and the second place in this front end where money can move (REQ-F-038).
//
// Closing is gated exactly like opening, and BY POSITION: the armed action carries the
// position id, so pressing Close on one trade and then on another arms the second rather
// than committing the first. TS-COCKPIT-010 pins that, because the failure it prevents —
// confirming a close and closing a different trade — is silent and unrecoverable.
//
// Every value shown is formatted in C++ (positionToVariant). Nothing here computes.
Rectangle {
    id: book

    required property var cockpit

    implicitWidth: 300
    implicitHeight: 200
    color: Theme.panel
    radius: Theme.radius
    border.width: 1
    border.color: Theme.gridline

    Column {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: 6

        Text {
            text: book.cockpit.positions.length > 0
                  ? qsTr("Open trades (%1)").arg(book.cockpit.positions.length)
                  : qsTr("Open trades")
            color: Theme.inkSecondary
            font.pixelSize: 12
            font.bold: true
        }

        // Stated, not an empty box. "No open trades" and "the book has not loaded" look
        // identical otherwise, and only one of them means you are flat.
        Text {
            text: qsTr("none open")
            color: Theme.inkMuted
            font.pixelSize: 12
            visible: book.cockpit.positions.length === 0
        }

        Repeater {
            model: book.cockpit.positions

            Row {
                id: row

                required property var modelData
                spacing: 8

                Text {
                    // Direction as an arrow AND a word: shape first, colour second.
                    text: (row.modelData.dir > 0 ? "▲ " : "▼ ") + row.modelData.side
                    color: row.modelData.dir > 0 ? Theme.up : Theme.down
                    font.pixelSize: 11
                    width: 62
                }

                Text {
                    text: row.modelData.symbol
                    color: Theme.ink
                    font.pixelSize: 11
                    width: 70
                }

                Text {
                    text: row.modelData.amount + " " + row.modelData.leverage
                    color: Theme.inkSecondary
                    font.pixelSize: 11
                    width: 78
                }

                Text {
                    text: row.modelData.openRate + " → " + row.modelData.markRate
                    color: Theme.inkSecondary
                    font.pixelSize: 11
                    width: 110
                }

                Button {
                    objectName: "cockpitClose_" + row.modelData.id
                    text: qsTr("Close")
                    onClicked: book.cockpit.pressClose(row.modelData.id)
                }
            }
        }
    }
}
