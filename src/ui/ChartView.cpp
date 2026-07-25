#include "ui/ChartView.h"

#include <QChart>
#include <QDateTime>
#include <QDateTimeAxis>
#include <QMouseEvent>
#include <QValueAxis>
#include <QWheelEvent>

#include <cmath>

ChartView::ChartView(QChart *chart, QWidget *parent)
    : QChartView(chart, parent)
{
    setMouseTracking(true);
}

void ChartView::zoomX(double factor, double centerMs)
{
    const QList<QAbstractAxis *> axes = chart()->axes(Qt::Horizontal);
    if (axes.isEmpty()) {
        return;
    }
    auto *ax = qobject_cast<QDateTimeAxis *>(axes.first());
    if (ax == nullptr) {
        return;
    }
    const double lo = static_cast<double>(ax->min().toMSecsSinceEpoch());
    const double hi = static_cast<double>(ax->max().toMSecsSinceEpoch());
    const double nlo = centerMs - (centerMs - lo) * factor;
    const double nhi = centerMs + (hi - centerMs) * factor;
    if ((nhi - nlo) < 1000.0) {  // don't zoom in past ~1 second
        return;
    }
    const QDateTime newMin = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(nlo));
    const QDateTime newMax = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(nhi));
    ax->setRange(newMin, newMax);
}

void ChartView::zoomY(double factor, double centerValue)
{
    const QList<QAbstractAxis *> axes = chart()->axes(Qt::Vertical);
    if (axes.isEmpty()) {
        return;
    }
    auto *ax = qobject_cast<QValueAxis *>(axes.first());
    if (ax == nullptr) {
        return;
    }
    const double lo = ax->min();
    const double hi = ax->max();
    const double nlo = centerValue - (centerValue - lo) * factor;
    const double nhi = centerValue + (hi - centerValue) * factor;
    if ((nhi - nlo) <= 0.0) {
        return;
    }
    ax->setRange(nlo, nhi);
}

void ChartView::wheelEvent(QWheelEvent *event)
{
    const qint32 wheelDelta = event->angleDelta().y();
    if (wheelDelta == 0) {
        QChartView::wheelEvent(event);
        return;
    }
    const double steps = static_cast<double>(wheelDelta) / 120.0;

    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        // Zoom the time axis around the value under the cursor.
        const QPointF value = chart()->mapToValue(event->position(), nullptr);
        zoomX(std::pow(0.8, steps), value.x());  // wheel up zooms in
    } else {
        // Pan the time axis (wheel up scrolls forward in time).
        chart()->scroll(steps * 40.0, 0.0);
    }
    emit interacted();
    event->accept();
}

void ChartView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QRectF plot = chart()->plotArea();
        const QPointF pos = event->position();
        if (pos.x() > plot.right()) {
            // Press started on the right value scale: vertical zoom drag.
            m_yZoom = true;
            m_lastPos = pos;
            setCursor(Qt::SizeVerCursor);
            event->accept();
            return;
        }
        if (plot.contains(pos)) {
            m_panning = true;
            m_lastPos = pos;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
    }
    QChartView::mousePressEvent(event);
}

void ChartView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        const QPointF d = event->position() - m_lastPos;
        m_lastPos = event->position();
        chart()->scroll(-d.x(), d.y());  // content follows the cursor
        emit interacted();
        event->accept();
        return;
    }
    if (m_yZoom) {
        const double dy = event->position().y() - m_lastPos.y();
        m_lastPos = event->position();
        const QList<QAbstractAxis *> axes = chart()->axes(Qt::Vertical);
        if (!axes.isEmpty()) {
            if (auto *ax = qobject_cast<QValueAxis *>(axes.first())) {
                const double axisMin = ax->min();
                const double axisMax = ax->max();
                const double mid = (axisMin + axisMax) / 2.0;
                // Drag down zooms out, drag up zooms in.
                zoomY(std::pow(1.01, dy), mid);
            }
        }
        emit interacted();
        event->accept();
        return;
    }
    QChartView::mouseMoveEvent(event);
}

void ChartView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning || m_yZoom) {
        m_panning = false;
        m_yZoom = false;
        unsetCursor();
        event->accept();
        return;
    }
    QChartView::mouseReleaseEvent(event);
}

void ChartView::mouseDoubleClickEvent(QMouseEvent *event)
{
    emit resetRequested();  // back to auto-fit
    event->accept();
}
