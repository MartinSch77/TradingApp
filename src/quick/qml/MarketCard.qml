// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import TradingApp.Cockpit

// One market card: symbol, price, change, and — the part that is not decoration — its
// FRESHNESS (REQ-F-038).
//
// Every value arrives already formatted from CockpitModel, including the em dash an absent
// reading renders as. This component deliberately does no arithmetic and no formatting: if
// it did, the claim it makes would be untested, since the tests exercise the C++ and not
// this file.
Rectangle {
    id: card

    required property string symbol
    required property string price
    required property string changePct
    required property string freshnessLabel
    // -1 down · 0 absent/flat · +1 up. Drives colour AND the arrow, so direction survives
    // without colour discrimination.
    required property int dir

    // Resolved here rather than by a function on the Theme singleton: a JS function on a
    // `pragma Singleton` is not reliably callable from a binding (measured — it raised
    // "Property 'polarity' of object Theme is not a function"), and a declarative binding is
    // the more QML-idiomatic answer in any case.
    readonly property color polarityColor: card.dir > 0 ? Theme.up
                                         : (card.dir < 0 ? Theme.down : Theme.inkMuted)

    implicitWidth: 180
    implicitHeight: 92
    color: Theme.panel
    radius: Theme.radius
    border.width: 1
    border.color: Theme.gridline

    Column {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: 4

        Text {
            text: card.symbol
            color: Theme.inkSecondary
            font.pixelSize: 12
            font.bold: true
        }

        Text {
            text: card.price
            color: Theme.ink
            font.pixelSize: 20
            font.family: "monospace"
        }

        Row {
            spacing: 6

            // The arrow is the second channel. A reader who cannot separate the green from
            // the red still gets the direction from the glyph.
            Text {
                text: card.dir > 0 ? "↑" : (card.dir < 0 ? "↓" : "—")
                color: card.polarityColor
                font.pixelSize: 13
                font.bold: true
            }

            Text {
                text: card.changePct
                color: card.polarityColor
                font.pixelSize: 13
                font.family: "monospace"

                Behavior on color {
                    ColorAnimation { duration: Theme.anim }
                }
            }

            // Freshness as TEXT, always present. "live" is as explicit as "6m old", so a
            // stale number can never look like a current one.
            Text {
                text: card.freshnessLabel
                color: card.freshnessLabel === "live" ? Theme.inkMuted : Theme.warn
                font.pixelSize: 11
            }
        }
    }
}
