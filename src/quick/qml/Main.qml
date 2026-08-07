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

        // The price and the evidence side by side — the chart says what happened, the meter
        // says how many independent reads agree about what happens next.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.gap

            // The chart is loaded THROUGH A LOADER so that its absence degrades instead of
            // taking the whole cockpit with it.
            //
            // PriceChart needs Qt Graphs, and Qt Graphs cannot be linked into a process that
            // also links Qt Charts — the two declare seventeen identically-named classes, so
            // the QML type registration goes ambiguous and every Graphs type stops resolving.
            // TradingApp's Widgets UI is built on Qt Charts, so inside THAT process this
            // Loader fails; declared inline instead, the failure would propagate and the
            // entire cockpit would render as a blank rectangle (measured — that is exactly
            // what happened). The standalone TradingCockpit binary links Graphs and not
            // Charts, and there the chart loads.
            Loader {
                id: chartLoader

                Layout.fillWidth: true
                Layout.fillHeight: true

                // setSource WITH initial properties, not a plain `source:`. PriceChart
                // declares its four inputs `required` — deliberately, so a missing injection
                // fails loudly instead of drawing an empty axis — and a Loader driven by
                // `source:` alone constructs the component with nothing set, which trips
                // exactly that guard ("Required property candles was not initialized") and
                // makes a working chart look like a missing module. The initial values
                // satisfy construction; the bindings below take over immediately after, so
                // the chart still follows the model.
                Component.onCompleted: chartLoader.setSource("PriceChart.qml", {
                    candles: root.cockpit.candles,
                    axisMin: root.cockpit.candleMin,
                    axisMax: root.cockpit.candleMax,
                    note: root.cockpit.candleNote,
                    span: root.cockpit.candleSpan
                })

                onStatusChanged: {
                    if (chartLoader.status === Loader.Ready) {
                        chartLoader.item.candles = Qt.binding(() => root.cockpit.candles);
                        chartLoader.item.axisMin = Qt.binding(() => root.cockpit.candleMin);
                        chartLoader.item.axisMax = Qt.binding(() => root.cockpit.candleMax);
                        chartLoader.item.note = Qt.binding(() => root.cockpit.candleNote);
                        chartLoader.item.span = Qt.binding(() => root.cockpit.candleSpan);
                    }
                }

                // Said out loud, with the reason and the way to see the chart. An empty panel
                // would read as "no data", which is a different and wrong claim.
                Rectangle {
                    anchors.fill: parent
                    visible: chartLoader.status === Loader.Error
                    color: Theme.panel
                    radius: Theme.radius
                    border.width: 1
                    border.color: Theme.gridline

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - (4 * Theme.gap)
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: Theme.inkMuted
                        font.pixelSize: 12
                        text: qsTr("Candlestick chart not available in this window.\n\n"
                                 + "It is drawn with Qt Graphs (CustomSeries), and Qt Graphs "
                                 + "cannot share a process with Qt Charts — which this "
                                 + "application's Widgets views use. Run the standalone "
                                 + "TradingCockpit binary to see it.")
                    }
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
