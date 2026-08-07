// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import TradingApp.Cockpit

// The trade ticket (REQ-F-038, REQ-N-005) — the one place in this front end where money
// can move.
//
// THIS FILE DECIDES NOTHING. Every rule is in CockpitModel and is checked by
// tst_cockpitmodel with no window and no GPU: whether the ticket is blocked and why,
// whether a press arms or commits, and what disarms it. A money-moving state machine
// expressed in bindings would arm and disarm at times no test could observe, which is
// precisely the wrong place to be clever.
//
// The double-press gate is carried over from the Widgets window rather than reinvented:
// both call trading::confirmPress. The first press arms and SAYS so; the second, within a
// second, sends. Editing the amount or the leverage disarms, because the order confirmed is
// then not the order that would be sent.
Rectangle {
    id: ticket

    required property var cockpit

    implicitWidth: 300
    implicitHeight: 190
    color: Theme.panel
    radius: Theme.radius
    border.width: 1
    // The border is the armed indicator too — a second channel beside the text below, so
    // the state does not rest on reading one line.
    border.color: ticket.cockpit.ticketArmed ? Theme.warn : Theme.gridline

    Column {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: 8

        Text {
            text: qsTr("Trade")
            color: Theme.inkSecondary
            font.pixelSize: 12
            font.bold: true
        }

        Row {
            spacing: 6

            Text {
                text: qsTr("Amount")
                color: Theme.inkSecondary
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }

            SpinBox {
                id: amountBox

                objectName: "cockpitAmount"
                from: 0
                to: 1000000
                stepSize: 50
                value: 500
                editable: true
                // The model is told on every change, which is also what DISARMS the gate:
                // an armed order that quietly changed size would be the worst kind of bug
                // this ticket could have.
                onValueChanged: ticket.cockpit.setTicket(amountBox.value, leverageBox.value)
            }
        }

        Row {
            spacing: 6

            Text {
                text: qsTr("Leverage")
                color: Theme.inkSecondary
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }

            SpinBox {
                id: leverageBox

                objectName: "cockpitLeverage"
                from: 1
                to: 30
                value: 5
                editable: true
                onValueChanged: ticket.cockpit.setTicket(amountBox.value, leverageBox.value)
            }
        }

        Row {
            spacing: 8

            Button {
                objectName: "cockpitBuy"
                text: qsTr("BUY")
                enabled: ticket.cockpit.ticketBlocked === ""
                onClicked: ticket.cockpit.press(true)
            }

            Button {
                objectName: "cockpitSell"
                text: qsTr("SELL")
                enabled: ticket.cockpit.ticketBlocked === ""
                onClicked: ticket.cockpit.press(false)
            }

            Button {
                objectName: "cockpitCancel"
                text: qsTr("Cancel")
                enabled: ticket.cockpit.ticketArmed
                onClicked: ticket.cockpit.cancelArm()
            }
        }

        // The prompt and the blocking reason, always visible. An arming nobody can see is
        // indistinguishable from a control that did nothing — this project measured exactly
        // that with the quick keys, where a swallowed press "seemed dead".
        Text {
            width: ticket.width - (2 * Theme.gap)
            wrapMode: Text.WordWrap
            text: ticket.cockpit.ticketPrompt !== ""
                  ? ticket.cockpit.ticketPrompt
                  : ticket.cockpit.ticketBlocked
            color: ticket.cockpit.ticketArmed ? Theme.warn : Theme.inkMuted
            font.pixelSize: 12
            font.bold: ticket.cockpit.ticketArmed
        }
    }

    // Escape abandons an arming. A gate you cannot back out of invites people to click the
    // second press just to clear the first.
    Keys.onEscapePressed: ticket.cockpit.cancelArm()
}
