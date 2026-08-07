// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_CHARTVIEW_H
#define TRADINGAPP_CHARTVIEW_H

#include <QChartView>
#include <QPointF>

// A QChartView with interactive navigation:
//   * mouse wheel            -> pan the time (x) axis
//   * Ctrl + mouse wheel     -> zoom the time (x) axis around the cursor
//   * left-click drag        -> pan the view (both axes)
//   * drag on the right scale -> zoom the value (y) axis
//   * double-click           -> reset to auto-fit
class ChartView : public QChartView
{
    Q_OBJECT
public:
    explicit ChartView(QChart *chart, QWidget *parent = nullptr);

signals:
    void interacted();       // user panned or zoomed -> suspend auto-fit
    void resetRequested();   // user double-clicked -> resume auto-fit

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void zoomX(double factor, double centerMs);
    void zoomY(double factor, double centerValue);

    bool m_panning = false;
    bool m_yZoom = false;
    QPointF m_lastPos;
};

#endif // TRADINGAPP_CHARTVIEW_H
