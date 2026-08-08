// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import TradingApp.Cockpit

// Two read-only panels the user asked for: the closed-trade record over the last 13 weeks,
// and the economic calendar of what is coming (REQ-F-038). Both are evidence, neither trades.
//
// Every value is formatted in C++ (CockpitModel) and tested headless; this file only lays it
// out. Direction and impact never rest on colour — a sign and a word carry them — so the
// panel reads in a monochrome capture and for a colour-blind trader, the same discipline as
// the confluence meter and the market cards.
Row {
    id: root

    required property var cockpit

    spacing: Theme.gap

    // The 13-week closed history: the headline summary, then the most recent trades.
    Rectangle {
        width: (root.width - Theme.gap) / 2
        height: root.height
        color: Theme.panel
        radius: Theme.radius
        border.width: 1
        border.color: Theme.gridline

        Column {
            anchors.fill: parent
            anchors.margins: Theme.gap
            spacing: 4

            Text {
                text: qsTr("Closed history")
                color: Theme.inkSecondary
                font.pixelSize: 12
                font.bold: true
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.cockpit.closedHistory
                color: Theme.ink
                font.pixelSize: 12
            }

            Repeater {
                model: root.cockpit.closedRows

                Row {
                    id: closedRow

                    required property var modelData
                    spacing: 8

                    Text {
                        text: (closedRow.modelData.dir > 0 ? "▲ " : "▼ ") + closedRow.modelData.side
                        color: closedRow.modelData.dir > 0 ? Theme.up : Theme.down
                        font.pixelSize: 11
                        width: 62
                    }
                    Text {
                        text: closedRow.modelData.symbol
                        color: Theme.inkSecondary
                        font.pixelSize: 11
                        width: 84
                    }
                    Text {
                        text: closedRow.modelData.net
                        color: Theme.ink
                        font.pixelSize: 11
                        width: 74
                    }
                    Text {
                        text: closedRow.modelData.closed
                        color: Theme.inkMuted
                        font.pixelSize: 11
                    }
                }
            }
        }
    }

    // The economic calendar: what is ahead, soonest first.
    Rectangle {
        width: (root.width - Theme.gap) / 2
        height: root.height
        color: Theme.panel
        radius: Theme.radius
        border.width: 1
        border.color: Theme.gridline

        Column {
            anchors.fill: parent
            anchors.margins: Theme.gap
            spacing: 4

            Text {
                text: qsTr("Economic calendar — upcoming")
                color: Theme.inkSecondary
                font.pixelSize: 12
                font.bold: true
            }

            Text {
                text: qsTr("none scheduled")
                color: Theme.inkMuted
                font.pixelSize: 12
                visible: root.cockpit.events.length === 0
            }

            Repeater {
                model: root.cockpit.events

                Row {
                    id: evRow

                    required property var modelData
                    spacing: 8

                    Text {
                        text: evRow.modelData.when
                        color: Theme.inkMuted
                        font.pixelSize: 11
                        width: 84
                    }
                    // A high-impact release — the kind the bot SITS OUT (REQ-F-034) — is
                    // flagged with a filled marker AND the word, never colour alone.
                    Text {
                        text: (evRow.modelData.high ? "● " : "○ ") + evRow.modelData.impact
                        color: evRow.modelData.high ? Theme.warn : Theme.inkMuted
                        font.pixelSize: 11
                        width: 70
                    }
                    Text {
                        text: (evRow.modelData.country.length > 0
                               ? evRow.modelData.country + " · " : "") + evRow.modelData.title
                        color: Theme.ink
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}
