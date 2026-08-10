// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_HEAVYWEIGHTSPANEL_H
#define TRADINGAPP_UI_HEAVYWEIGHTSPANEL_H

#include "domain/IndexConfluence.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>

QT_FORWARD_DECLARE_CLASS(QChart)
QT_FORWARD_DECLARE_CLASS(QChartView)
QT_FORWARD_DECLARE_CLASS(QLineSeries)
QT_FORWARD_DECLARE_CLASS(QValueAxis)
class QHideEvent;
class QLabel;
class QShowEvent;
class QTableWidget;
class QTimer;

namespace trading::ui {

class LeadGauge;

// The top-ten constituents of the Nasdaq-100 and the S&P 500, side by side, as an
// EARLY read on where the two indices may go (REQ-F-035).
//
// The idea is the one a trading desk uses: an index is not a thing that moves on its
// own, it is the weighted sum of its members, and the biggest members move it. When
// nine of the ten heavyweights are already up while the index has not followed, the
// index usually follows. When one name is carrying the whole move, it usually does
// not.
//
// Two honesty rules this window inherits from the reads behind it:
//
//  * A constituent whose series could not be fetched is shown as UNKNOWN, never as
//    0.0 %. A grid of zeros would look like a flat market rather than an absent feed.
//  * The summary is labelled a STAND-IN for market breadth. Real breadth is the
//    advance/decline line over all 500 members with their weights, which this app
//    does not fetch — the ten biggest names are the honest approximation, and the
//    window says so rather than implying more.
//
// It costs no new feed: the series are the ones MarketFeeds::fetchReferenceSeries
// already pulls for the confluence reads.
class HeavyweightsPanel : public QDialog
{
    Q_OBJECT;   // ";" so tree-sitter/moc see the first slot's marker

public:
    explicit HeavyweightsPanel(QWidget *parent = nullptr);

    // Fed from the same reference series the confluence reads use, keyed by ticker.
    void setReferenceSeries(const QHash<QString, QList<double>> &series);
    // The other two books the combined indication needs (REQ-F-035/036): the volume bars
    // keyed by TICKER, and the app's own per-instrument series keyed by APP SYMBOL. The
    // second is what the futures reads and the opening range are read out of — without
    // it this window scored its indices on the handful of reads that need neither, and
    // reported the rest as unmeasurable while the app had the data all along.
    void setVolumeSeries(const QHash<QString, trading::VolumeSeries> &volumes);
    void setSymbolSeries(const QHash<QString, QList<double>> &series);
    // The regime the signal is judged in, from the feeds the app already has.
    void setRegime(bool vixValid, double vix, bool eventRisk);

signals:
    // "Fetch the constituent series again." Emitted on a timer while this window is
    // VISIBLE and never while it is closed: the graph is the only consumer that wants
    // minute-by-minute data, so a closed window must not keep the sweep running.
    void refreshRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void buildUi();
    // One index's table + summary line, filled from its own pulse. The table is
    // ordered by the size of the move — the top MOVERS, since a name that has not
    // moved says nothing about where the index goes next.
    //
    // static because it writes only through its arguments: the widgets to fill are
    // passed in, so the same code serves the Nasdaq and the S&P tables without
    // reaching for a member and picking the wrong one.
    static void fillTable(QTableWidget *table, QLabel *summary,
                          const HeavyweightPulse &pulse);
    // The movers as CURVES: each constituent's session normalised to its own start,
    // so ten instruments at ten price levels can be compared on one axis. This is the
    // view that shows the field moving together — or one name running away from it.
    void fillChart(const QHash<QString, QList<double>> &series);
    void updateLeadSignals(const QHash<QString, QList<double>> &series);

    QTableWidget *m_nasdaqTable = nullptr;
    QTableWidget *m_spTable = nullptr;
    QLabel *m_nasdaqSummary = nullptr;
    QLabel *m_spSummary = nullptr;
    QLabel *m_verdict = nullptr;
    QLabel *m_caveat = nullptr;
    QChartView *m_chartView = nullptr;
    QChart *m_chart = nullptr;
    QValueAxis *m_axisX = nullptr;
    QValueAxis *m_axisY = nullptr;
    QList<QLineSeries *> m_curves;
    LeadGauge *m_gauge = nullptr;
    QLabel *m_nasdaqLead = nullptr;
    QLabel *m_spLead = nullptr;
    bool m_vixValid = false;
    double m_vix = 0.0;
    bool m_eventRisk = false;
    QHash<QString, QList<double>> m_series;
    QHash<QString, trading::VolumeSeries> m_volumes;
    QHash<QString, QList<double>> m_symbolSeries;
    QTimer *m_liveTimer = nullptr;
    QLabel *m_updated = nullptr;
};

} // namespace trading::ui

#endif // TRADINGAPP_UI_HEAVYWEIGHTSPANEL_H
