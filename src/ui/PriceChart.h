#ifndef TRADINGAPP_PRICECHART_H
#define TRADINGAPP_PRICECHART_H

#include "domain/Models.h"

#include <QList>
#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QChart)
QT_FORWARD_DECLARE_CLASS(QChartView)
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
    void rescaleAxes();
    // In a manually panned/zoomed view (auto-fit off), slide the time window left so
    // the newest point stays at the right border — but only while the view is still
    // following the live tail (see addPoint). newX = newest point time (ms since
    // epoch); prevLastX = the previous newest, to detect if we were at the edge.
    void followLatestInManualView(qreal newX, qreal prevLastX);
    // Keeps the current-price line and its right-hand price tag in sync.
    void updatePriceMarker();
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
    void showTradeTooltip(const QPointF &point, bool isBuy, bool entered);
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

    double m_lastPrice = 0.0;
    qint32 m_predDir = 0;  // current prediction direction: +1 up, -1 down, 0 none
    bool m_autoScale = true;  // false once the user pans/zooms; reset on double-click
    // Visible window (ms) shown by the timeframe buttons; 0 = all loaded history.
    // Default ~2h so the recent 1-minute detail is on screen (per-second wiggle is
    // there too — wheel-zoom in for it); the buttons step out from there.
    qint64 m_viewWindowMs = 2 * 60 * 60'000;  // 2 hours

    // Room for ~1 month of seeded history plus many hours of live ticks, so the
    // accumulating live tail doesn't trim the older history off the left edge.
    static constexpr qint32 kMaxPoints = 40000;
};

#endif // TRADINGAPP_PRICECHART_H
