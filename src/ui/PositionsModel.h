#ifndef TRADINGAPP_UI_POSITIONSMODEL_H
#define TRADINGAPP_UI_POSITIONSMODEL_H

#include "domain/Models.h"

#include <QAbstractTableModel>
#include <QList>
#include <QSet>
#include <QString>
#include <QStyledItemDelegate>

// Model behind the open-trades table (QTableView). Replaces the former
// QTableWidget item churn: the periodic portfolio poll and the per-tick P/L
// re-price now emit dataChanged over existing rows instead of allocating new
// QTableWidgetItems — open SL/TP editors and the mark checkboxes survive both
// kinds of refresh by construction (REQ-N-006: hot UI paths allocation-free).
class PositionsModel : public QAbstractTableModel
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees markers
public:
    enum Column {
        ColMark = 0, ColInstrument, ColSide, ColAmount, ColLev, ColOpen,
        ColUnits, ColPl, ColCloseCost, ColSl, ColTp, ColCount
    };

    explicit PositionsModel(QObject *parent = nullptr);

    // Display parameters: currency symbol shown in texts/headers and the
    // USD -> display conversion rate (account figures arrive in USD).
    void setDisplay(const QString &ccySymbol, double eurPerUsd);

    // Replace the snapshot. Same position ids in the same order -> in-place
    // dataChanged (open editors and checkbox marks survive); a changed set ->
    // model reset (marks survive by id; ids no longer open are dropped).
    void setPositions(const QList<Position> &positions);

    // Live re-price of the shown instrument's P/L column between polls: each
    // row shows its last API P/L plus the value of the price move since
    // anchorPrice — pure dataChanged, no allocation.
    // Re-price the shown P/L of `symbol`'s rows from live rates: eToro's API P/L
    // plus the move of the marking side (long → bid, short → ask) since the rate
    // the API figure was computed at (Position::apiCloseRate; anchorPrice and
    // midPrice are the fallbacks when a side or the anchor is unavailable).
    void repriceOpenPnl(const QString &symbol, double bid, double ask, double midPrice,
                        double anchorPrice);

    // Echo a just-submitted SL/TP edit immediately (the server snapshot
    // catches up on a later poll; see MainWindow::m_pendingSlTp).
    void setSlTpRates(qint32 row, double slRate, double tpRate);

    // An SL/TP cell editor is open (view-side; see SlTpEditGuardDelegate):
    // while set, NO dataChanged is emitted for that one cell — Qt refreshes an
    // open editor from the model on dataChanged, which would overwrite what the
    // user is typing on every poll/re-price. The stored values keep updating;
    // endCellEdit() emits the deferred dataChanged so the cell catches up.
    void beginCellEdit(qint32 row, qint32 column);
    void endCellEdit();

    [[nodiscard]] QStringList markedIds() const;

    [[nodiscard]] qint32 rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] qint32 columnCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, qint32 role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 qint32 role = Qt::EditRole) override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
    [[nodiscard]] QVariant headerData(qint32 section, Qt::Orientation orientation,
                        qint32 role = Qt::DisplayRole) const override;

signals:
    // The user committed an SL/TP cell editor; text is the raw editor content.
    // The model does not change itself — MainWindow validates, converts to
    // rates and echoes back via setSlTpRates().
    void slTpEdited(qint32 row, qint32 column, const QString &text);

private:
    [[nodiscard]] double toDisplay(double usd) const { return usd * m_eurPerUsd; }
    [[nodiscard]] QString displayText(const Position &p, qint32 column, qint32 row) const;
    // dataChanged over a rectangle, minus the cell currently being edited (up to
    // four sub-rectangles) — the single place the edit guard is enforced.
    void emitChangedSkippingEditor(qint32 firstRow, qint32 lastRow, qint32 firstCol,
                                   qint32 lastCol);

    QList<Position> m_positions;
    QList<double> m_plDelta;  // live re-price delta per row (0 when not repriced)
    QSet<QString> m_marked;   // checked position ids (survive snapshot resets)
    QString m_ccy;
    double m_eurPerUsd = 1.0;
    qint32 m_editRow = -1;    // cell with an open editor (-1 = none); see beginCellEdit
    qint32 m_editCol = -1;
};

// Thin delegate for the SL/TP columns: its only job is telling the model when a
// cell editor opens and closes, so the model can hold back dataChanged for that
// cell (the view refreshes open editors from the model on dataChanged — every
// portfolio poll would otherwise overwrite the user's typing mid-edit).
class SlTpEditGuardDelegate : public QStyledItemDelegate
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees markers
public:
    explicit SlTpEditGuardDelegate(PositionsModel *model, QObject *parent = nullptr);

    [[nodiscard]] QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                                        const QModelIndex &index) const override;
    void destroyEditor(QWidget *editor, const QModelIndex &index) const override;

private:
    PositionsModel *m_model;
};

#endif // TRADINGAPP_UI_POSITIONSMODEL_H
