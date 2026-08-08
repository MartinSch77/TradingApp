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

        Row {
            spacing: 6

            Text {
                text: qsTr("Trade")
                color: Theme.inkSecondary
                font.pixelSize: 12
                font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }

            // The instrument, selectable — the cockpit was stuck on whatever config.json
            // named, which made every other panel a view of one instrument forever.
            ComboBox {
                id: instrumentBox

                objectName: "cockpitInstrument"
                model: ticket.cockpit.instruments
                currentIndex: Math.max(0, ticket.cockpit.instruments.indexOf(
                                              ticket.cockpit.instrument))
                width: 150
                onActivated: ticket.cockpit.selectInstrument(instrumentBox.currentText)

                ToolTip.visible: hovered
                ToolTip.text: qsTr("Switch instrument. Doing so DISARMS a pending "
                                 + "confirmation — the order you confirmed is not the order "
                                 + "that would now be sent.")
            }
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
                onValueChanged: ticket.push()
            }
        }

        Row {
            spacing: 6

            Text {
                text: qsTr("Stop / Target")
                color: Theme.inkSecondary
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }

            // AMOUNTS, not rates — the same convention the Widgets window uses, so the two
            // front ends cannot mean different things by the same number. 0 = no leg.
            SpinBox {
                id: slBox

                objectName: "cockpitStopLoss"
                from: 0
                to: 1000000
                stepSize: 25
                value: 0
                editable: true
                onValueChanged: ticket.push()

                ToolTip.visible: hovered
                ToolTip.text: qsTr("Loss AMOUNT at which the position closes, in account "
                                 + "currency. 0 = no stop.")
            }

            SpinBox {
                id: tpBox

                objectName: "cockpitTakeProfit"
                from: 0
                to: 1000000
                stepSize: 25
                value: 0
                editable: true
                onValueChanged: ticket.push()

                ToolTip.visible: hovered
                ToolTip.text: qsTr("Profit AMOUNT at which the position closes. 0 = no target.")
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

            // "Cancel" alone, sitting beside BUY and SELL, reads as "cancel an order" —
            // which this cannot do; the cockpit has no resting orders. It abandons the
            // half-finished CONFIRMATION, and now says so.
            Button {
                objectName: "cockpitCancel"
                text: qsTr("Disarm")
                enabled: ticket.cockpit.ticketArmed
                onClicked: ticket.cockpit.cancelArm()

                ToolTip.visible: hovered
                ToolTip.text: qsTr("Abandon the pending confirmation. This does NOT cancel "
                                 + "an order — nothing has been sent yet. The first press of "
                                 + "BUY or SELL only arms; the second sends.")
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

    // One place that pushes the ticket, so no editor can forget a field — a setTicket call
    // missing the stop would silently drop the leg AND leave the gate armed for an order
    // that no longer matches what was confirmed.
    function push() {
        ticket.cockpit.setTicket(amountBox.value, leverageBox.value, slBox.value, tpBox.value);
    }

    // Escape abandons an arming. A gate you cannot back out of invites people to click the
    // second press just to clear the first.
    Keys.onEscapePressed: ticket.cockpit.cancelArm()
}
