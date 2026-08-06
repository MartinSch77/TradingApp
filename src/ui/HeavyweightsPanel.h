#ifndef TRADINGAPP_UI_HEAVYWEIGHTSPANEL_H
#define TRADINGAPP_UI_HEAVYWEIGHTSPANEL_H

#include "domain/IndexConfluence.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>

class QLabel;
class QTableWidget;

namespace trading::ui {

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

private:
    void buildUi();
    // One index's table + summary line, filled from its own pulse.
    void fillTable(QTableWidget *table, QLabel *summary, const HeavyweightPulse &pulse);

    QTableWidget *m_nasdaqTable = nullptr;
    QTableWidget *m_spTable = nullptr;
    QLabel *m_nasdaqSummary = nullptr;
    QLabel *m_spSummary = nullptr;
    QLabel *m_verdict = nullptr;
    QLabel *m_caveat = nullptr;
};

} // namespace trading::ui

#endif // TRADINGAPP_UI_HEAVYWEIGHTSPANEL_H
