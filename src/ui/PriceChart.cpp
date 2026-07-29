#include "ui/PriceChart.h"

#include "ChartView.h"

#include <QButtonGroup>
#include <QChart>
#include <QChartView>
#include <QCursor>
#include <QDateTimeAxis>
#include <QFont>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineSeries>
#include <QLocale>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QScatterSeries>
#include <QToolTip>
#include <QValueAxis>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace {
// Decimals appropriate to a price's magnitude (indices ~thousands need 2, forex ~1
// needs 4), so the tooltip reads sensibly across instruments.
QString formatPrice(double v)
{
    const double a = std::abs(v);
    const qint32 dec = (a >= 100.0) ? 2 : ((a >= 10.0) ? 3 : ((a >= 1.0) ? 4 : 5));
    return QString::number(v, 'f', dec);
}

// Add a series to a chart and attach it to both of its axes — the common wiring
// for every series in this widget.
void addToChart(QChart *chart, QAbstractSeries *s, QAbstractAxis *x, QAbstractAxis *y)
{
    chart->addSeries(s);
    static_cast<void>(s->attachAxis(x));
    static_cast<void>(s->attachAxis(y));
}
}  // namespace

// The initialiser list follows PriceChart.h's declaration order — that IS the
// order the members are initialized in (anything else is -Wreorder), and the
// axes/marker items take their chart as parent, so the two charts have to come
// first.
PriceChart::PriceChart(QWidget *parent)
    : QWidget(parent)
    , m_chart(new QChart())
    , m_series(new QLineSeries(this))
    , m_levelSeries(new QLineSeries(this))
    , m_axisX(new QDateTimeAxis(m_chart))
    , m_axisY(new QValueAxis(m_chart))
    , m_changeChart(new QChart())
    , m_changeView(new QChartView(m_changeChart, this))
    , m_changeSeries(new QLineSeries(this))
    , m_changeHp(new QLineSeries(this))
    , m_changeAxisX(new QDateTimeAxis(m_changeChart))
    , m_changeAxisY(new QValueAxis(m_changeChart))
    , m_markerBg(new QGraphicsRectItem(m_chart))
    , m_markerText(new QGraphicsSimpleTextItem(m_chart))
    , m_arrow(new QGraphicsSimpleTextItem(m_chart))
    , m_eventSeries(new QLineSeries(this))
    , m_eventText(new QGraphicsSimpleTextItem(m_chart))
{
    // Keep this window above the others (reinforced by MainWindow); intrinsic to
    // the chart so it holds even if the window is re-parented or re-shown.
    setWindowFlag(Qt::WindowStaysOnTopHint, true);

    
    m_series->setName(QStringLiteral("Price"));

    
    m_chart->addSeries(m_series);
    m_chart->legend()->hide();
    m_chart->setTheme(QChart::ChartThemeDark);
    m_chart->setAnimationOptions(QChart::NoAnimation);
    m_chart->setMargins(QMargins(4, 4, 4, 4));

    
    // Date + time: the chart seeds ~1 month of history, so a time-only label would make
    // a month look like a single day. "dd MMM HH:mm" reads correctly zoomed in or out.
    m_axisX->setFormat(QStringLiteral("dd MMM HH:mm"));
    m_axisX->setTickCount(6);
    m_axisX->setTitleText(QStringLiteral("Time"));
    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    static_cast<void>(m_series->attachAxis(m_axisX));

    
    m_axisY->setLabelFormat(QStringLiteral("%.2f"));
    m_axisY->setTitleText(QStringLiteral("Price"));
    m_chart->addAxis(m_axisY, Qt::AlignRight);
    static_cast<void>(m_series->attachAxis(m_axisY));

    // Horizontal "current price" level line spanning the whole plot.
    
    QPen levelPen(QColor(0xe0, 0xb0, 0x00));  // amber, stands out on the dark theme
    levelPen.setWidthF(1.0);
    levelPen.setStyle(Qt::DashLine);
    m_levelSeries->setPen(levelPen);
    addToChart(m_chart, m_levelSeries, m_axisX, m_axisY);

    // Vertical line marking an imminent economic event (driven by setEventMarker()).
    
    QPen eventPen(QColor(0xff, 0x8c, 0x00));  // orange — distinct from the amber level line
    eventPen.setWidthF(1.6);
    eventPen.setStyle(Qt::DashLine);
    m_eventSeries->setPen(eventPen);
    addToChart(m_chart, m_eventSeries, m_axisX, m_axisY);

    // One factory for every scatter overlay: the trade-entry bullets on the price
    // chart and the strong-move outliers on the change strip below.
    auto makeScatter = [this](QChart *chart, QAbstractAxis *xAxis, QAbstractAxis *yAxis,
                              QColor fill, qreal markerSize, QColor border) {
        auto *s = new QScatterSeries(this);
        s->setMarkerShape(QScatterSeries::MarkerShapeCircle);
        s->setMarkerSize(markerSize);
        s->setColor(fill);
        s->setBorderColor(border);
        addToChart(chart, s, xAxis, yAxis);
        return s;
    };

    // Position entry markers: green for buy/long, red for sell/short.
    m_buyMarkers = makeScatter(m_chart, m_axisX, m_axisY,
                               QColor(0x25, 0xb5, 0x63), 16.0, Qt::white);  // green (matches BUY)
    m_sellMarkers = makeScatter(m_chart, m_axisX, m_axisY,
                                QColor(0xe3, 0x55, 0x55), 16.0, Qt::white);  // red (matches SELL)
    static_cast<void>(connect(m_buyMarkers, &QScatterSeries::hovered, this,
                              [this](QPointF pt, bool on) { showTradeTooltip(pt, true, on); }));
    static_cast<void>(connect(m_sellMarkers, &QScatterSeries::hovered, this,
                              [this](QPointF pt, bool on) { showTradeTooltip(pt, false, on); }));

    auto *view = new ChartView(m_chart, this);
    view->setRenderHint(QPainter::Antialiasing);
    m_view = view;

    // The user's pan/zoom takes over from auto-fitting; double-click restores it.
    static_cast<void>(connect(view, &ChartView::interacted, this, [this] {
        m_autoScale = false;
        updatePriceMarker();
        syncChangeX();          // keep the change strip aligned with the panned/zoomed view
        refreshTradeMarkers();  // re-pin the entry lane to the bottom of the new view
    }));
    static_cast<void>(connect(view, &ChartView::resetRequested, this, [this] {
        m_autoScale = true;
        rescaleAxes();
    }));

    // Right-side price tag: a filled amber box with the price, pinned to the
    // right edge of the plot at the current-price level.
    
    m_markerBg->setPen(Qt::NoPen);
    m_markerBg->setBrush(QColor(0xe0, 0xb0, 0x00));
    m_markerBg->setZValue(20.0);
    
    m_markerText->setBrush(QColor(0x10, 0x10, 0x10));
    m_markerText->setZValue(21.0);
    m_markerBg->hide();
    m_markerText->hide();

    // Prediction-direction arrow (▲ green up / ▼ red down), near the current price.
    
    QFont arrowFont = m_arrow->font();
    arrowFont.setPointSizeF(arrowFont.pointSizeF() + 14.0);
    arrowFont.setBold(true);
    m_arrow->setFont(arrowFont);
    m_arrow->setZValue(22.0);
    m_arrow->hide();

    // Label sitting at the top of the event line (e.g. the event's title).
    
    m_eventText->setBrush(QColor(0xff, 0x8c, 0x00));
    m_eventText->setZValue(22.0);
    m_eventText->hide();

    // Keep the tag glued to the axis as the plot area moves (resize, dock/float).
    static_cast<void>(
        connect(m_chart, &QChart::plotAreaChanged, this, [this] { updatePriceMarker(); }));

    // --- Change strip below the price chart ---------------------------------
    // Shows the per-tick % change with an adaptive ±2σ "strong move" filter, so
    // sharp moves stand out from ordinary noise.
    
    QPen chgPen(QColor(0x4f, 0xa3, 0xff));  // steel blue
    chgPen.setWidthF(1.2);
    m_changeSeries->setPen(chgPen);

    
    m_changeChart->addSeries(m_changeSeries);
    m_changeChart->legend()->hide();
    m_changeChart->setTheme(QChart::ChartThemeDark);
    m_changeChart->setAnimationOptions(QChart::NoAnimation);
    m_changeChart->setMargins(QMargins(4, 0, 4, 4));
    m_changeChart->setTitle(QStringLiteral(
        "Price change per tick + high-pass filter — |Δ| beyond ±2σ = strong move"));
    QFont ctFont = m_changeChart->titleFont();
    ctFont.setPointSizeF(ctFont.pointSizeF() - 1.0);
    m_changeChart->setTitleFont(ctFont);
    m_changeChart->setTitleBrush(QColor(0xb0, 0xb0, 0xb0));
    m_changeSeries->setPen(chgPen);  // re-apply after setTheme(), which resets series pens

    
    m_changeAxisX->setFormat(QStringLiteral("HH:mm:ss"));
    m_changeAxisX->setTickCount(6);
    m_changeAxisX->setLabelsVisible(false);  // time is already labelled on the chart above
    m_changeChart->addAxis(m_changeAxisX, Qt::AlignBottom);
    static_cast<void>(m_changeSeries->attachAxis(m_changeAxisX));

    
    m_changeAxisY->setLabelFormat(QStringLiteral("%+.2f"));
    m_changeAxisY->setTitleText(QStringLiteral("Δ %"));
    m_changeAxisY->setTickCount(3);
    m_changeAxisY->setRange(-0.1, 0.1);
    m_changeChart->addAxis(m_changeAxisY, Qt::AlignRight);
    static_cast<void>(m_changeSeries->attachAxis(m_changeAxisY));

    // Reference lines: 0% baseline and the ±2σ thresholds (dashed amber).
    auto flatLine = [this](QColor c, Qt::PenStyle style, qreal w) {
        auto *s = new QLineSeries(this);
        QPen p(c);
        p.setStyle(style);
        p.setWidthF(w);
        s->setPen(p);
        addToChart(m_changeChart, s, m_changeAxisX, m_changeAxisY);
        return s;
    };
    m_changeZero = flatLine(QColor(0x88, 0x88, 0x88), Qt::SolidLine, 0.8);
    m_changeUpThr = flatLine(QColor(0xe0, 0xb0, 0x00), Qt::DashLine, 1.0);
    m_changeDownThr = flatLine(QColor(0xe0, 0xb0, 0x00), Qt::DashLine, 1.0);

    // High-pass filtered price (violet): removes the slow trend and leaves only
    // the fast component, so sharp moves stand out even inside a drifting market.
    
    QPen hpPen(QColor(0xb5, 0x6c, 0xff));  // violet
    hpPen.setWidthF(1.3);
    m_changeHp->setPen(hpPen);
    addToChart(m_changeChart, m_changeHp, m_changeAxisX, m_changeAxisY);

    // Outlier markers for changes beyond the threshold: green up, red down.
    m_strongUp = makeScatter(m_changeChart, m_changeAxisX, m_changeAxisY,
                             QColor(0x25, 0xb5, 0x63), 9.0, QColor(0x10, 0x10, 0x10));
    m_strongDown = makeScatter(m_changeChart, m_changeAxisX, m_changeAxisY,
                               QColor(0xe3, 0x55, 0x55), 9.0, QColor(0x10, 0x10, 0x10));

    
    m_changeView->setRenderHint(QPainter::Antialiasing);
    m_changeView->setToolTip(QStringLiteral(
        "Per-tick price change (blue, %). Violet = high-pass filter: the slow trend "
        "is removed so only fast moves remain. The dashed band is ±2 standard "
        "deviations of recent changes; points outside it are flagged as strong "
        "up/down moves."));

    // Timeframe buttons: pick the visible window. Short windows reveal the live
    // per-second price changes; 1M shows the whole loaded month.
    auto *tfRow = new QHBoxLayout;
    tfRow->setContentsMargins(6, 4, 6, 2);
    tfRow->setSpacing(4);
    tfRow->addWidget(new QLabel(QStringLiteral("View:"), this));
    auto *tfGroup = new QButtonGroup(this);
    tfGroup->setExclusive(true);
    // View windows (not literal bar sizes): a zoom ladder from a detailed intraday
    // view out to the whole month. "1m" ≈ the last couple hours of 1-minute detail.
    const QList<QPair<QString, qint64>> timeframes = {
        {QStringLiteral("1m"), 2LL * 60 * 60'000},        // ~2 hours (default)
        {QStringLiteral("1h"), 24LL * 60 * 60'000},       // 1 day
        {QStringLiteral("1D"), 7LL * 24 * 60 * 60'000},   // 1 week
        {QStringLiteral("1M"), 0LL},                      // all loaded (~1 month)
    };
    for (const auto &tf : timeframes) {
        auto *b = new QPushButton(tf.first, this);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setMaximumWidth(46);
        b->setChecked(tf.second == m_viewWindowMs);
        const qint64 ms = tf.second;
        static_cast<void>(connect(b, &QPushButton::clicked, this, [this, ms] { setViewWindow(ms); }));
        tfGroup->addButton(b);
        tfRow->addWidget(b);
    }
    tfRow->addStretch();

    // Timeframe row on top, price chart (larger), change strip below (smaller).
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(tfRow);
    layout->addWidget(m_view, 3);
    layout->addWidget(m_changeView, 1);
}

void PriceChart::setViewWindow(qint64 windowMs)
{
    m_viewWindowMs = windowMs;
    m_autoScale = true;   // resume auto-follow at the chosen window
    rescaleAxes();
    recomputeChange();    // re-zoom the change strip to the new visible window
}

void PriceChart::setTitle(const QString &title)
{
    m_chart->setTitle(title);
}

void PriceChart::setHistory(const QList<Candle> &candles)
{
    // A fresh history is a new dataset (startup or an instrument switch), so drop
    // any prior pan/zoom and re-fit the axes to the new price range and scale.
    m_autoScale = true;

    // Drop the previous instrument's entry bullets; onPortfolio re-supplies them.
    m_openBuys.clear();
    m_openSells.clear();
    m_buyMarkers->clear();
    m_sellMarkers->clear();

    m_series->clear();
    QList<QPointF> points;
    points.reserve(candles.size());
    for (const Candle &c : candles) {
        if (!c.timestamp.isValid()) {
            continue;
        }
        points.append(QPointF(static_cast<qreal>(c.timestamp.toMSecsSinceEpoch()), c.close));
    }
    m_series->replace(points);
    if (!points.isEmpty()) {
        m_lastPrice = points.last().y();
    }

    // Build the per-tick % change from consecutive closes. The seed mixes coarse
    // (hourly) context with fine (1-minute) recent bars, so skip the large hourly
    // steps (gap > 5 min) — the change strip should reflect intraday moves only.
    QList<QPointF> changes;
    changes.reserve(points.size());
    for (qsizetype i = 1; i < points.size(); ++i) {
        const QPointF &cur = points[i];
        const QPointF &before = points[i - 1];
        const double prev = before.y();
        const qreal gapMin = (cur.x() - before.x()) / 60000.0;
        if ((prev > 0.0) && (gapMin <= 5.0)) {
            changes.append(QPointF(cur.x(), ((cur.y() - prev) / prev) * 100.0));
        }
    }
    m_changeSeries->replace(changes);

    rescaleAxes();
    recomputeChange();
}

void PriceChart::addPoint(const QDateTime &time, double price)
{
    if (!time.isValid() || (price <= 0.0)) {
        return;
    }
    const auto x = static_cast<qreal>(time.toMSecsSinceEpoch());

    // Remember the newest point's time before appending: the manual-view follow below
    // uses it to tell whether the view was still tracking the live edge.
    qreal prevLastX = x;
    if (m_series->count() > 0) {
        prevLastX = m_series->points().last().x();
    }

    m_series->append(x, price);
    if (m_series->count() > kMaxPoints) {
        m_series->removePoints(0, m_series->count() - kMaxPoints);
    }

    // Per-tick % change relative to the previous price.
    if (m_lastPrice > 0.0) {
        m_changeSeries->append(x, ((price - m_lastPrice) / m_lastPrice) * 100.0);
        if (m_changeSeries->count() > kMaxPoints) {
            m_changeSeries->removePoints(0, m_changeSeries->count() - kMaxPoints);
        }
    }

    m_lastPrice = price;

    // Auto-fit keeps the newest point on-screen on its own; a manually panned/zoomed
    // view does not, so slide it here to keep the current price at the right border.
    if (!m_autoScale && (m_axisX != nullptr)) {
        followLatestInManualView(x, prevLastX);
    }

    rescaleAxes();
    recomputeChange();
}

void PriceChart::followLatestInManualView(qreal newX, qreal prevLastX)
{
    const qreal oldMin = static_cast<qreal>(m_axisX->min().toMSecsSinceEpoch());
    const qreal oldMax = static_cast<qreal>(m_axisX->max().toMSecsSinceEpoch());
    // Nothing to do until the line actually reaches the right border. And if the user
    // has scrolled back into history (the latest data already sits off the right edge),
    // leave the view where they put it rather than yanking it forward.
    if ((newX <= oldMax) || (prevLastX > oldMax)) {
        return;
    }
    const qreal width = oldMax - oldMin;
    if (width <= 0.0) {
        return;
    }
    // Pin the current price to the right border, keeping the user's zoom width.
    const qreal newMax = newX;
    const qreal newMin = newMax - width;
    const QDateTime rangeMin = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(newMin));
    const QDateTime rangeMax = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(newMax));
    m_axisX->setRange(rangeMin, rangeMax);
}

void PriceChart::setOpenTrades(const QList<QPointF> &buys, const QList<QPointF> &sells)
{
    m_openBuys = buys;
    m_openSells = sells;
    rescaleAxes();  // fold entry prices into the y-range, then (re-)plot the bullets
}

void PriceChart::refreshTradeMarkers()
{
    if ((m_axisX == nullptr) || (m_axisY == nullptr)) {
        return;
    }
    const qreal xmin = static_cast<qreal>(m_axisX->min().toMSecsSinceEpoch());
    const qreal xmax = static_cast<qreal>(m_axisX->max().toMSecsSinceEpoch());

    // Entry bullets sit in a fixed lane just above the Time axis — a few percent of
    // the visible price range up from the bottom — so they line up by entry time
    // below the price line instead of cluttering it at their entry price. The real
    // entry price is preserved in m_openBuys/m_openSells for the hover tooltip.
    const double ymin = m_axisY->min();
    const double yrange = m_axisY->max() - ymin;
    const double baseline = ymin + ((yrange > 0.0) ? (yrange * 0.05) : 0.0);

    // Clamp each entry's time into the visible window: trades opened before the
    // chart starts show at the left edge rather than vanish.
    auto laneRow = [xmin, xmax, baseline](const QList<QPointF> &src) {
        QList<QPointF> out;
        out.reserve(src.size());
        for (const QPointF &p : src) {
            out.append(QPointF(qBound(xmin, p.x(), xmax), baseline));
        }
        return out;
    };
    m_buyMarkers->replace(laneRow(m_openBuys));
    m_sellMarkers->replace(laneRow(m_openSells));
}

void PriceChart::showTradeTooltip(QPointF point, bool isBuy, bool entered)
{
    if (!entered) {
        QToolTip::hideText();
        return;
    }
    // Markers now sit at the lane baseline, so point.y() is no longer the entry price.
    // Recover it by matching the hovered (clamped) time back to the source entry that
    // produced this bullet.
    const QList<QPointF> &src = isBuy ? m_openBuys : m_openSells;
    const qreal xmin = static_cast<qreal>(m_axisX->min().toMSecsSinceEpoch());
    const qreal xmax = static_cast<qreal>(m_axisX->max().toMSecsSinceEpoch());
    double price = point.y();
    double best = std::numeric_limits<double>::max();
    for (const QPointF &p : src) {
        const double d = std::abs(qBound(xmin, p.x(), xmax) - point.x());
        if (d < best) {
            best = d;
            price = p.y();
        }
    }
    const QString side = isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");
    const QString text = QStringLiteral("%1 — opened at %2").arg(side, formatPrice(price));
    QToolTip::showText(QCursor::pos(), text, m_view);
}

void PriceChart::rescaleAxes()
{
    const QList<QPointF> pts = m_series->points();
    if (pts.isEmpty()) {
        return;
    }

    // Only auto-fit the axes while the user has not taken over the view.
    if (m_autoScale) {
        qreal xmax = pts.last().x();
        // Keep an imminent event on-screen even while it is still slightly in the
        // future — extend the axis to its time so the line sits just right of "now".
        if ((m_eventMs > 0) && (static_cast<qreal>(m_eventMs) > xmax)) {
            xmax = static_cast<qreal>(m_eventMs) + 30000.0;  // +30s breathing room
        }
        // Sliding window selected by the timeframe buttons (0 = all history).
        qreal xmin = pts.first().x();
        if (m_viewWindowMs > 0) {
            xmin = std::max(xmin, xmax - static_cast<qreal>(m_viewWindowMs));
        }

        // Fit Y to the points inside the visible window only, so a short window zooms
        // in on the recent range and the per-second wiggle becomes visible (rather
        // than being flattened by the whole month's price span).
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        for (const QPointF &p : pts) {
            if ((p.x() < xmin) || (p.x() > xmax)) {
                continue;
            }
            minY = std::min(minY, p.y());
            maxY = std::max(maxY, p.y());
        }
        if (minY > maxY) {  // window landed between points — fall back to the last price
            maxY = pts.last().y();
            minY = maxY;
        }
        double pad = (maxY - minY) * 0.08;
        if (pad <= 0.0) {
            pad = std::max(1.0, maxY * 0.001);
        }
        m_axisY->setRange(minY - pad, maxY + pad);
        const QDateTime rangeMin = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(xmin));
        const QDateTime rangeMax = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(xmax));
        m_axisX->setRange(rangeMin, rangeMax);
    }

    updatePriceMarker();
    syncChangeX();
    refreshTradeMarkers();
}

void PriceChart::updatePriceMarker()
{
    if ((m_levelSeries == nullptr) || (m_markerText == nullptr) || (m_markerBg == nullptr)) {
        return;
    }

    // Stretch the level line across the currently visible time range.
    const qint64 xmin = m_axisX->min().toMSecsSinceEpoch();
    const qint64 xmax = m_axisX->max().toMSecsSinceEpoch();
    if (m_lastPrice > 0.0) {
        m_levelSeries->replace({QPointF(static_cast<qreal>(xmin), m_lastPrice),
                                QPointF(static_cast<qreal>(xmax), m_lastPrice)});
    }

    const QRectF plot = m_chart->plotArea();
    if ((m_lastPrice <= 0.0) || plot.isEmpty()) {
        m_markerBg->hide();
        m_markerText->hide();
        if (m_arrow != nullptr) {
            m_arrow->hide();
        }
        updateEventMarker();
        return;
    }

    // y for the current price (x is irrelevant to the vertical position).
    const QPointF levelPoint(static_cast<qreal>(xmin), m_lastPrice);
    const qreal y = m_chart->mapToPosition(levelPoint, m_series).y();

    m_markerText->setText(QLocale().toString(m_lastPrice, 'f', 2));
    const QRectF tb = m_markerText->boundingRect();
    constexpr qreal padX = 4.0;
    constexpr qreal padY = 2.0;
    const qreal boxW = tb.width() + (padX * 2.0);
    const qreal boxH = tb.height() + (padY * 2.0);
    const qreal left = plot.right();  // hug the right axis

    m_markerBg->setRect(left, y - (boxH / 2.0), boxW, boxH);
    m_markerText->setPos(left + padX, (y - (boxH / 2.0)) + padY);
    m_markerBg->show();
    m_markerText->show();

    // Prediction-direction arrow, just left of the price tag at the price level.
    if (m_arrow != nullptr) {
        if (m_predDir == 0) {
            m_arrow->hide();
        } else {
            const bool up = m_predDir > 0;
            m_arrow->setText(up ? QStringLiteral("▲") : QStringLiteral("▼"));
            m_arrow->setBrush(up ? QColor(0x25, 0xb5, 0x63) : QColor(0xe3, 0x55, 0x55));
            const QRectF ab = m_arrow->boundingRect();
            m_arrow->setPos(left - ab.width() - 8.0, y - (ab.height() / 2.0));
            m_arrow->show();
        }
    }

    updateEventMarker();
}

void PriceChart::setEventMarker(qint64 whenMs, const QString &label)
{
    if ((whenMs == m_eventMs) && (label == m_eventLabel)) {
        return;  // unchanged — avoid a redundant rescale on every price tick
    }
    m_eventMs = whenMs;
    m_eventLabel = label;
    rescaleAxes();  // may extend the time axis to bring a still-future event on-screen
}

void PriceChart::updateEventMarker()
{
    if ((m_eventSeries == nullptr) || (m_eventText == nullptr)) {
        return;
    }
    const QRectF plot = m_chart->plotArea();
    const qint64 xmin = m_axisX->min().toMSecsSinceEpoch();
    const qint64 xmax = m_axisX->max().toMSecsSinceEpoch();
    if ((m_eventMs <= 0) || (m_eventMs < xmin) || (m_eventMs > xmax) || plot.isEmpty()) {
        m_eventSeries->clear();
        m_eventText->hide();
        return;
    }
    // Draw the line spanning the full current price range at the event's time.
    const double ymin = m_axisY->min();
    const double ymax = m_axisY->max();
    m_eventSeries->replace({QPointF(static_cast<qreal>(m_eventMs), ymin),
                            QPointF(static_cast<qreal>(m_eventMs), ymax)});

    // Label just inside the top of the line, nudged left if it would overflow.
    const QPointF top =
        m_chart->mapToPosition(QPointF(static_cast<qreal>(m_eventMs), ymax), m_eventSeries);
    m_eventText->setText(m_eventLabel);
    const qreal w = m_eventText->boundingRect().width();
    qreal x = top.x() + 3.0;
    if ((x + w) > plot.right()) {
        x = std::max(plot.left(), top.x() - w - 3.0);
    }
    m_eventText->setPos(x, top.y() + 2.0);
    m_eventText->setVisible(!m_eventLabel.isEmpty());
}

void PriceChart::setPredictionDirection(qint32 dir)
{
    m_predDir = (dir > 0) ? 1 : ((dir < 0) ? -1 : 0);
    updatePriceMarker();
}

void PriceChart::recomputeChange()
{
    const QList<QPointF> pts = m_changeSeries->points();

    // Scale everything to the currently VISIBLE window so off-screen data (e.g. the
    // coarse hourly seed, whose big steps dwarf intraday ticks) can't flatten the strip.
    const qreal vxmin = static_cast<qreal>(m_axisX->min().toMSecsSinceEpoch());
    const qreal vxmax = static_cast<qreal>(m_axisX->max().toMSecsSinceEpoch());
    auto visible = [vxmin, vxmax](qreal x) { return (x >= vxmin) && (x <= vxmax); };

    // Standard deviation of the visible changes → adaptive "strong move" threshold.
    double sigma = 0.0;
    double maxAbs = 0.0;
    double mean = 0.0;
    qint32 n = 0;
    for (const QPointF &p : pts) {
        if (visible(p.x())) {
            mean += p.y();
            ++n;
        }
    }
    if (n >= 2) {
        mean /= static_cast<double>(n);
        double var = 0.0;
        for (const QPointF &p : pts) {
            if (visible(p.x())) {
                var += (p.y() - mean) * (p.y() - mean);
                maxAbs = std::max(maxAbs, std::abs(p.y()));
            }
        }
        sigma = std::sqrt(var / static_cast<double>(n));
    }
    m_changeThr = 2.0 * sigma;

    // Split out the points beyond ±threshold as strong up / down moves.
    QList<QPointF> up;
    QList<QPointF> down;
    if (m_changeThr > 0.0) {
        for (const QPointF &p : pts) {
            if (p.y() > m_changeThr) {
                up.append(p);
            } else if (p.y() < (-m_changeThr)) {
                down.append(p);
            } else {
                // within the ±threshold band — an ordinary move, not highlighted
            }
        }
    }
    m_strongUp->replace(up);
    m_strongDown->replace(down);

    // First-order high-pass filter of the price series, expressed as % of price.
    // h[n] = a*(h[n-1] + p[n] - p[n-1]); with a flat price the p[n]-p[n-1] term is
    // zero and h decays as a*h[n-1], so the line returns to 0 when nothing moves.
    // Reset across coarse gaps (>5 min) so the hourly seed boundaries aren't mistaken
    // for huge instantaneous moves (which would spike the filter and the Y range).
    constexpr double kHpAlpha = 0.6;
    const QList<QPointF> pricePts = m_series->points();
    QList<QPointF> hp;
    hp.reserve(pricePts.size());
    double h = 0.0;
    for (qsizetype i = 1; i < pricePts.size(); ++i) {
        const QPointF &cur = pricePts[i];
        const QPointF &before = pricePts[i - 1];
        const double p = cur.y();
        const qreal gapMin = (cur.x() - before.x()) / 60000.0;
        if (gapMin > 5.0) {
            h = 0.0;  // don't carry the filter across a coarse sampling gap
        } else {
            h = kHpAlpha * ((h + p) - before.y());
        }
        const double hpct = (p != 0.0) ? ((h / p) * 100.0) : 0.0;
        hp.append(QPointF(cur.x(), hpct));
        if (visible(cur.x())) {
            maxAbs = std::max(maxAbs, std::abs(hpct));
        }
    }
    m_changeHp->replace(hp);

    // Symmetric Y range around 0, sized to the visible data / band (small floor so a
    // dead-flat window still has a sane scale rather than collapsing onto the axis).
    double m = std::max(maxAbs, m_changeThr);
    if (m <= 0.0) {
        m = 0.02;
    }
    m_changeAxisY->setRange(-m * 1.2, m * 1.2);
    syncChangeX();
}

void PriceChart::syncChangeX()
{
    if ((m_changeAxisX == nullptr) || (m_axisX == nullptr)) {
        return;
    }

    // Follow the price chart's (possibly panned) time window exactly.
    const QDateTime visMin = m_axisX->min();
    const QDateTime visMax = m_axisX->max();
    m_changeAxisX->setRange(visMin, visMax);

    // Stretch the baseline and threshold lines across that window.
    const qreal xmin = static_cast<qreal>(m_axisX->min().toMSecsSinceEpoch());
    const qreal xmax = static_cast<qreal>(m_axisX->max().toMSecsSinceEpoch());
    m_changeZero->replace({QPointF(xmin, 0.0), QPointF(xmax, 0.0)});
    m_changeUpThr->replace({QPointF(xmin, m_changeThr), QPointF(xmax, m_changeThr)});
    m_changeDownThr->replace({QPointF(xmin, -m_changeThr), QPointF(xmax, -m_changeThr)});
}
