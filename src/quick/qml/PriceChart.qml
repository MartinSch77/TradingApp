// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtGraphs
import TradingApp.Cockpit

// The intraday candlestick chart, drawn with Qt Graphs' CustomSeries (new in Qt 6.11).
//
// WHY CustomSeries AND NOT A CANDLESTICK SERIES. Qt Graphs 2D has no candlestick type.
// CustomSeries is the supported way to add one: it is "a scatter graph that lets you access
// custom data for each element", so each bar carries its own {open, high, low, close} map
// and a delegate decides how to draw it. Its documented limitation — elements cannot share
// information, so a custom LINE series is impossible — does not bite here, because a candle
// is by definition independent of its neighbours.
//
// DIRECTION IS ENCODED BY FILL FIRST AND COLOUR SECOND, and that is a correctness decision.
// The trading convention is green-up / red-down, which is the single worst pair for
// deuteranopia: measured with the palette validator, the good and critical steps of this
// project's status palette sit at CVD dE 4.1 — indistinguishable. So this chart uses the
// HOLLOW-CANDLE convention that pre-dates colour screens: an up bar is an outline, a down
// bar is solid. A deuteranope reads direction from the fill; everyone else also gets the
// colour they expect. The confluence meter takes the same approach with glyphs.
Rectangle {
    id: chart

    // [{ open, high, low, close, up }] — already filtered and judged in C++ (domain/Candles).
    required property var candles
    required property real axisMin
    required property real axisMax
    // Non-empty exactly when there is nothing to draw. The QML never decides this: an empty
    // axis and a dead-flat market look identical on screen, so the C++ says which it is.
    required property string note
    // "last 120 of 339 one-minute bars". Shown because a chart that silently draws two hours
    // of a six-hour session, labelled only "1-minute candles", claims something untrue.
    required property string span

    implicitWidth: 560
    implicitHeight: 300
    color: Theme.panel
    radius: Theme.radius
    border.width: 1
    border.color: Theme.gridline

    // CustomSeries is populated imperatively — it has no model property. Rebuilt whole on
    // every change rather than diffed: an intraday session is a few hundred bars, and a
    // partial update that drifts out of step with the model is a chart that quietly shows
    // yesterday's tail.
    function reload() {
        series.clear();
        for (let i = 0; i < chart.candles.length; ++i)
            series.append(chart.candles[i]);
    }

    onCandlesChanged: chart.reload()
    Component.onCompleted: chart.reload()

    Row {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: Theme.gap
        spacing: 8
        z: 2

        Text {
            text: qsTr("Price")
            color: Theme.inkSecondary
            font.pixelSize: 12
            font.bold: true
        }

        Text {
            text: chart.span
            color: Theme.inkMuted
            font.pixelSize: 12
        }
    }

    // The legend. Present because direction is a two-series distinction and identity must
    // never rest on colour alone; it names the fill rule in words.
    Row {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.gap
        spacing: 10
        z: 2

        Row {
            spacing: 4
            Rectangle {
                width: 8
                height: 12
                color: "transparent"
                border.color: Theme.up
                border.width: 1
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: qsTr("up (hollow)")
                color: Theme.inkSecondary
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            spacing: 4
            Rectangle {
                width: 8
                height: 12
                color: Theme.down
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: qsTr("down (solid)")
                color: Theme.inkSecondary
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    // The stated empty state, never an empty frame.
    Text {
        anchors.centerIn: parent
        text: chart.note
        color: Theme.inkMuted
        font.pixelSize: 12
        visible: chart.note !== ""
        z: 2
    }

    GraphsView {
        id: view

        anchors.fill: parent
        anchors.topMargin: Theme.gap + 18
        anchors.margins: Theme.gap
        visible: chart.note === ""

        theme: GraphsTheme {
            colorScheme: GraphsTheme.ColorScheme.Dark
            backgroundColor: "transparent"
            backgroundVisible: false
            // Recessive grid: the data is the subject, the frame is not.
            grid.mainColor: Theme.gridline
            grid.subColor: Theme.gridline
            axisX.mainColor: Theme.gridline
            axisY.mainColor: Theme.gridline
            labelTextColor: Theme.inkMuted
        }

        axisX: ValueAxis {
            min: 0
            max: Math.max(1, chart.candles.length)
            // Minutes are not a quantity worth labelling one by one; the axis exists to give
            // the eye a scale, and the card above the chart carries the actual time.
            tickInterval: Math.max(1, Math.round(chart.candles.length / 6))
            labelsVisible: false
        }

        axisY: ValueAxis {
            min: chart.axisMin
            max: chart.axisMax
            labelDecimals: 2
        }

        CustomSeries {
            id: series

            // One candle. The root spans high-to-low so the wick and the body can be placed
            // in its own coordinates; `data` is the map appended above, plus the `index` the
            // series adds itself.
            //
            // THE CHILDREN ARE ASSIGNED THROUGH `children:`, NOT DECLARED INLINE, and that is
            // load-bearing rather than a style choice. CustomSeries hands the delegate its
            // values through a property named `data` — the name is fixed by the API — and
            // `data` is ALSO Item's default property, the one inline children are collected
            // into. Declaring `property var data` shadows it, so an inline child becomes a
            // binding on the shadowed property, which the series then overwrites with its
            // QVariantMap. The candle would render as nothing at all, with no error. qmllint
            // names it exactly: "Duplicate binding on property 'data'". Qt's own CustomSeries
            // documentation shows the inline form; it does not survive a second child.
            // `children` is a separate list<Item> property and is not shadowed.
            delegate: Item {
                id: bar

                // Set by the series. `final` marks the shadow as deliberate — the base
                // member is unreachable here by design, not by accident.
                final property var data

                // Every binding reads `bar.bar`, never `bar.data` directly. The delegate is
                // CONSTRUCTED before the series assigns `data`, so the first evaluation of
                // each binding sees undefined and throws "Cannot read property 'index' of
                // undefined" — once per property per candle, which on a session of 339 bars
                // is a wall of errors and a chart that renders nothing. The fallback is a
                // degenerate bar at the origin; it is never drawn, because assigning `data`
                // re-evaluates all of these.
                readonly property var bar: bar.data
                    ?? ({ index: 0, open: 0, high: 0, low: 0, close: 0, up: true })

                readonly property real slot: Math.abs(series.mapX(1) - series.mapX(0))
                readonly property real yHigh: series.mapY(bar.bar.high)
                readonly property real yLow: series.mapY(bar.bar.low)
                readonly property real yOpen: series.mapY(bar.bar.open)
                readonly property real yClose: series.mapY(bar.bar.close)
                readonly property color tint: bar.bar.up ? Theme.up : Theme.down

                x: series.mapX(bar.bar.index + 0.5) - (width / 2)
                // Render space may run either way; min/abs keep this correct without
                // depending on which.
                y: Math.min(bar.yHigh, bar.yLow)
                width: Math.max(3, bar.slot * 0.7)
                height: Math.abs(bar.yLow - bar.yHigh)

                children: [
                    // The wick, one pixel wide through the centre.
                    Rectangle {
                        x: (bar.width - width) / 2
                        width: 1
                        y: 0
                        height: bar.height
                        color: bar.tint
                    },

                    // The body. Hollow when the bar closed up, solid when it closed down —
                    // the channel a colour-blind reader actually gets.
                    Rectangle {
                        x: 0
                        width: bar.width
                        y: Math.min(bar.yOpen, bar.yClose) - bar.y
                        // A doji (open == close) has no body at all and would vanish; one
                        // pixel is the honest minimum, and it reads as the flat bar it is.
                        height: Math.max(1, Math.abs(bar.yClose - bar.yOpen))
                        color: bar.bar.up ? "transparent" : Theme.down
                        border.color: bar.tint
                        border.width: 1
                    }
                ]
            }
        }
    }
}
