// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import TradingApp.Cockpit

// The confluence meter: nine discrete ticks, one per independent read (REQ-F-038, REQ-F-035).
//
// NOT a gauge arc showing "6/8". Two reasons, and both are correctness rather than taste:
//
//  1. A filled-vs-unfilled arc is a two-slice donut, and the number is the chart. It also
//     cannot express the fact this application is built around — how many reads could be
//     MEASURED at all.
//  2. There are exactly nine discrete reads, so nine discrete marks is the honest form.
//
// State is encoded by GLYPH first and colour second. The measured reason: the status good and
// critical steps sit at CVD dE 4.1 under deuteranopia — indistinguishable — so a row of
// green/amber/grey dots would be unreadable for a red-green-colourblind trader. Shape is not.
Rectangle {
    id: meter

    // [{ glyph, label, state, detail }] — already judged in C++.
    required property var ticks
    required property string agreement
    required property string evidence
    required property string probability

    implicitWidth: 300
    implicitHeight: 300   // the nine-read list below the hero mark needs the room
    color: Theme.panel
    radius: Theme.radius
    border.width: 1
    border.color: Theme.gridline

    function tickColor(state) {
        if (state === "agrees")
            return Theme.up;
        if (state === "disagrees")
            return Theme.down;
        return Theme.inkMuted;
    }

    Column {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: 6

        Text {
            text: qsTr("Confluence")
            color: Theme.inkSecondary
            font.pixelSize: 12
            font.bold: true
        }

        // The nine ticks in a row — the hero mark.
        Row {
            spacing: 5

            Repeater {
                model: meter.ticks

                Text {
                    id: tickGlyph

                    required property var modelData
                    text: tickGlyph.modelData.glyph
                    color: meter.tickColor(tickGlyph.modelData.state)
                    font.pixelSize: 18
                }
            }
        }

        // Agreement AND unmeasurability, because they are different facts.
        Text {
            text: meter.agreement
            color: Theme.ink
            font.pixelSize: 14
            font.bold: true
        }

        Text {
            text: meter.evidence
            color: Theme.inkSecondary
            font.pixelSize: 12
            width: meter.width - (2 * Theme.gap)
            wrapMode: Text.WordWrap
        }

        // The probability line, which says UNCALIBRATED rather than inventing a number.
        Text {
            text: meter.probability
            color: meter.probability.indexOf("UNCALIBRATED") === 0 ? Theme.warn : Theme.ink
            font.pixelSize: 12
            font.family: "monospace"
        }

        Rectangle {
            width: meter.width - (2 * Theme.gap)
            height: 1
            color: Theme.gridline
        }

        // The per-read list: glyph, name and its own number. Identity never rests on colour,
        // because the state is spelled out here in words.
        Repeater {
            model: meter.ticks

            Row {
                id: readRow

                required property var modelData
                spacing: 6

                // The NUMBER behind the read, on hover. Without it "unmeasurable" is a
                // verdict with no evidence: a reader cannot tell a feed that is missing from
                // one that returned something unusable, and those need different fixes.
                HoverHandler {
                    id: readHover
                }

                ToolTip.visible: readHover.hovered
                ToolTip.text: readRow.modelData.state === "unmeasurable"
                              ? qsTr("Not measurable right now: %1.\n\nAn unmeasurable read "
                                   + "NEVER counts as agreement — a \"6 of 9\" built from "
                                   + "absent feeds would be a lie.").arg(
                                        readRow.modelData.detail !== ""
                                        ? readRow.modelData.detail
                                        : qsTr("the series it needs has not arrived"))
                              : qsTr("%1: %2").arg(readRow.modelData.label).arg(
                                    readRow.modelData.detail !== ""
                                    ? readRow.modelData.detail : qsTr("measured"))

                Text {
                    text: readRow.modelData.glyph
                    color: meter.tickColor(readRow.modelData.state)
                    font.pixelSize: 11
                }

                Text {
                    text: readRow.modelData.label
                    color: Theme.inkSecondary
                    font.pixelSize: 11
                }

                Text {
                    text: readRow.modelData.state === "unmeasurable"
                          ? qsTr("unmeasurable") : ""
                    color: Theme.inkMuted
                    font.pixelSize: 11
                    font.italic: true
                }
            }
        }
    }
}
