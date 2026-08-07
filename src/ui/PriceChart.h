// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_PRICECHART_H
#define TRADINGAPP_PRICECHART_H

#include "domain/Models.h"

#include <QList>
#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QAbstractAxis)
QT_FORWARD_DECLARE_CLASS(QChart)
QT_FORWARD_DECLARE_CLASS(QChartView)
QT_FORWARD_DECLARE_CLASS(QColor)
QT_FORWARD_DECLARE_CLASS(QHBoxLayout)
QT_FORWARD_DECLARE_CLASS(QLineSeries)
QT_FORWARD_DECLARE_CLASS(QScatterSeries)
QT_FORWARD_DECLARE_CLASS(QDateTimeAxis)
QT_FORWARD_DECLARE_CLASS(QValueAxis)
QT_FORWARD_DECLARE_CLASS(QGraphicsRectItem)
QT_FORWARD_DECLARE_CLASS(QGraphicsSimpleTextItem)

// A live time-vs-price line chart for one instrument.
// Seed it with recent candles, then feed it live ticks via addPoint().
class PriceChart : public QWidget
{
    Q_OBJECT
public:
    explicit PriceChart(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setHistory(const QList<Candle> &candles);
    void addPoint(const QDateTime &time, double price);
    // Entry bullets for the currently-open trades (green = buy, red = sell). Points are
    // QPointF(msSinceEpoch, price): the time positions the bullet horizontally, while
    // the price is kept only for the hover tooltip — bullets are drawn in a fixed lane
    // just above the Time axis, not at their entry price. Entries older than the visible
    // window are clamped to the left edge so they still show.
    void setOpenTrades(const QList<QPointF> &buys, const QList<QPointF> &sells);
    // Show a directional arrow for the current prediction: +1 up, -1 down, 0 none.
    void setPredictionDirection(qint32 dir);
    // Set the visible time window (a sliding "last N" window that follows live data).
    // 0 = show all loaded history. Drives the 1m/1h/1D/1M timeframe buttons; the Y axis
    // auto-fits to the window so short windows reveal the per-second price wiggle.
    void setViewWindow(qint64 windowMs);
    // Mark an economic event with a vertical line + label at its time (the caller
    // shows it only while the event is imminent, e.g. 10 min before to 5 min after).
    // whenMs is the event time in ms since epoch; pass whenMs <= 0 to clear it.
    void setEventMarker(qint64 whenMs, const QString &label);

private:
    // The constructor is an orchestrator (the MainWindow::buildUi pattern): the
    // builders below each assemble one cohesive piece, in construction order.
    void buildPriceChart();          // price series + dark theme + both axes
    void buildLevelAndEventLines();  // dashed current-price and event-time lines
    void buildTradeMarkers();        // buy/sell entry bullets + hover tooltips
    void buildChartView();           // the view + pan/zoom interaction wiring
    void buildOverlayItems();        // price tag, prediction arrow, event label
    void buildChangeStrip();         // the Δ%-per-tick strip below the price chart
    [[nodiscard]] QHBoxLayout *buildTimeframeRow();  // 1m/1h/1D/1M window buttons
    void buildLayout(QHBoxLayout *tfRow);            // stack row + both charts
    // One factory for every scatter overlay: the trade-entry bullets on the price
    // chart and the strong-move outliers on the change strip below.
    QScatterSeries *makeScatter(QChart *chart, QAbstractAxis *xAxis, QAbstractAxis *yAxis,
                                QColor fill, qreal markerSize, QColor border);
    void rescaleAxes();
    // The view has stopped auto-fitting (the user panned or zoomed): bring
    // everything that depends on the visible window in line with where they put it.
    void applyManualView();
    // The plot rectangle moved or resized (window resize, dock/float).
    void applyPlotGeometry();
    // Feeds the three data series the currently visible window only, decimated to
    // roughly one point per screen pixel (see decimateWindow in the .cpp). The
    // full-resolution data lives in m_prices/m_changes/m_hp; QtCharts never sees
    // it. This is a pure performance measure: handing the series their full
    // 40 000 points costs ~2.4 us per point on every single tick (measured), so a
    // live tick took ~100 ms of GUI-thread time once the buffers had filled up.
    // Must run after the axes have their final range — it decimates to that range.
    void refreshVisibleSeries();
    // Derives the per-tick change and high-pass samples for one new price point
    // from its predecessor. In one place because the live path (addPoint) and the
    // seeding path (setHistory) have to apply the same rule: the high-pass filter
    // is recursive, so its incremental state is only correct if nothing else
    // appends to m_hp by another route.
    void appendDerived(const QPointF &prev, const QPointF &cur);
    // In a manually panned/zoomed view (auto-fit off), slide the time window left so
    // the newest point stays at the right border — but only while the view is still
    // following the live tail (see addPoint). newX = newest point time (ms since
    // epoch); prevLastX = the previous newest, to detect if we were at the edge.
    void followLatestInManualView(qreal newX, qreal prevLastX);
    // Keeps the current-price line and its right-hand price tag in sync.
    void updatePriceMarker();
    // Decimals the price axis (and the price tag on it) shows for `price`: the app's own
    // priceDecimals() rule, so the labels follow whichever instrument is on screen —
    // an index in the thousands does not need what a forex pair does — with a floor of
    // three, so a tick never reads as a price the instrument never had (a fixed 2 turned
    // every EURUSD label into "1.15").
    [[nodiscard]] static qint32 priceAxisDecimals(double price);
    // Apply that precision to the Y axis' printf label format. `reference` is a price
    // from the range being shown (0 = none yet, use the floor).
    void applyPriceAxisFormat(double reference);
    // Recompute the per-tick change series' strong-move filter (±2σ threshold,
    // highlighted outliers) and rescale its Y axis. Called when data changes.
    void recomputeChange();
    // Cheap sync of the change strip's X range / baseline lines to the price
    // chart's current (possibly panned) time window.
    void syncChangeX();
    // Re-plot the open-trade bullets in a fixed lane just above the Time axis,
    // clamping each entry's time into the visible window so entries older than the
    // chart still appear (at the left edge).
    void refreshTradeMarkers();
    // Tooltip shown while hovering an entry bullet (side + opened price).
    void showTradeTooltip(QPointF point, bool isBuy, bool entered);
    // (Re)draw the event line across the current plot, or hide it when there is no
    // event set or its time is outside the visible window.
    void updateEventMarker();

    QChart *m_chart = nullptr;
    QChartView *m_view = nullptr;
    QLineSeries *m_series = nullptr;
    QLineSeries *m_levelSeries = nullptr;              // horizontal line at last price
    QScatterSeries *m_buyMarkers = nullptr;           // long entry points (green)
    QScatterSeries *m_sellMarkers = nullptr;          // short entry points (red)
    QList<QPointF> m_openBuys;                         // true (time,price) buy entries
    QList<QPointF> m_openSells;                        // true (time,price) sell entries
    QDateTimeAxis *m_axisX = nullptr;
    QValueAxis *m_axisY = nullptr;

    // Change strip below the price chart: per-tick % change with a ±2σ "strong
    // move" filter (dashed threshold lines + highlighted outlier markers).
    QChart *m_changeChart = nullptr;
    QChartView *m_changeView = nullptr;
    QLineSeries *m_changeSeries = nullptr;   // per-tick % change
    QLineSeries *m_changeHp = nullptr;       // high-pass filtered price (fast moves only)
    QLineSeries *m_changeZero = nullptr;     // 0% baseline
    QLineSeries *m_changeUpThr = nullptr;    // +2σ threshold
    QLineSeries *m_changeDownThr = nullptr;  // -2σ threshold
    QScatterSeries *m_strongUp = nullptr;    // changes above +2σ (green)
    QScatterSeries *m_strongDown = nullptr;  // changes below -2σ (red)
    QDateTimeAxis *m_changeAxisX = nullptr;
    QValueAxis *m_changeAxisY = nullptr;
    double m_changeThr = 0.0;                // current ±2σ threshold (percent)
    QGraphicsRectItem *m_markerBg = nullptr;           // right-side price tag background
    QGraphicsSimpleTextItem *m_markerText = nullptr;   // right-side price tag label
    QGraphicsSimpleTextItem *m_arrow = nullptr;        // prediction direction arrow

    // Imminent-event marker: a vertical line at the event's time with a label.
    QLineSeries *m_eventSeries = nullptr;
    QGraphicsSimpleTextItem *m_eventText = nullptr;
    qint64 m_eventMs = 0;   // event time (ms since epoch); 0 = no active event
    QString m_eventLabel;

    // Full-resolution data, ascending in x, each capped at kMaxPoints. The series
    // above hold a decimated view of the visible window instead — plain QList
    // scans over these cost tens of nanoseconds per point, whereas every point
    // handed to a QXYSeries is re-laid-out by QtCharts on every change.
    QList<QPointF> m_prices;   // (msSinceEpoch, price)
    QList<QPointF> m_changes;  // (msSinceEpoch, change since the previous point in %)
    QList<QPointF> m_hp;       // (msSinceEpoch, high-pass filtered price in % of price)
    // Running state h of the high-pass filter, i.e. its value after the last
    // point in m_hp — that is what makes appending a sample O(1) instead of a
    // rebuild over the whole series.
    double m_hpState = 0.0;

    double m_lastPrice = 0.0;
    qint32 m_predDir = 0;  // current prediction direction: +1 up, -1 down, 0 none
    bool m_autoScale = true;  // false once the user pans/zooms; reset on double-click
    // Visible window (ms) shown by the timeframe buttons; 0 = all loaded history.
    // Default ~2h so the recent 1-minute detail is on screen (per-second wiggle is
    // there too — wheel-zoom in for it); the buttons step out from there.
    qint64 m_viewWindowMs = 2LL * 60 * 60'000;  // 2 hours

    // Room for ~1 month of seeded history plus many hours of live ticks, so the
    // accumulating live tail doesn't trim the older history off the left edge.
    static constexpr qint32 kMaxPoints = 40000;
};

#endif // TRADINGAPP_PRICECHART_H
