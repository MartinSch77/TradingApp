#ifndef TRADINGAPP_UI_POSITIONSMODEL_H
#define TRADINGAPP_UI_POSITIONSMODEL_H

#include "domain/Models.h"
#include "domain/PaperTrader.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QDateTime>
#include <QHash>
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
        ColUnits, ColPl, ColCloseCost, ColSl, ColTp,
        // What the local model says about KEEPING this position (REQ-F-034):
        // hold / close / "—" when it has said nothing about the instrument.
        ColAi,
        ColCount
    };

    explicit PositionsModel(QObject *parent = nullptr);

    // Display parameters: currency symbol shown in texts/headers and the
    // USD -> display conversion rate (account figures arrive in USD).
    void setDisplay(const QString &ccySymbol, double eurPerUsd);

    // Replace the snapshot. Same position ids in the same order -> in-place
    // dataChanged (open editors and checkbox marks survive); a changed set ->
    // model reset (marks survive by id; ids no longer open are dropped).
    void setPositions(const QList<Position> &positions);

    // Re-price EVERY row's P/L from the live quote book (EtoroClient::quotes(), keyed
    // by instrumentId), using eToro's own identity at the side the trade closes on —
    // so a trade on an instrument other than the one on screen is just as current as
    // the one on screen. Pure dataChanged, no allocation.
    //
    // A row whose quote is missing, or whose price eToro published more than
    // trading::kQuoteStaleMs ago, keeps eToro's last snapshot figure and is marked as
    // not-live: the alternative — marking off a quote that is minutes behind — is
    // exactly what made the column disagree with eToro's own screen.
    void repriceOpenPnl(const QHash<qint64, Quote> &quotes, const QDateTime &nowUtc);

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

    // The local model's verdict per SYMBOL, as the bot's review pass produced it.
    // Stored by symbol rather than by position id so a re-poll that renumbers rows
    // keeps the reads, and looked up (not computed) in data() — the hot path stays
    // allocation-free (REQ-N-006).
    void setAiOpinions(const QHash<QString, trading::HoldVerdict> &bySymbol);

    [[nodiscard]] QStringList markedIds() const;

    // Totals for the panel's summary line, already in the DISPLAY currency (the model
    // owns that conversion for its cells, so the summary cannot drift from them).
    // The invested total sums the per-row figures exactly as the Amount column rounds
    // them, so adding the column up by hand gives this number.
    [[nodiscard]] double totalInvestedDisplay() const;
    [[nodiscard]] double totalPnlDisplay() const;
    // False when at least one row has no current quote: its P/L is eToro's last
    // snapshot, so the total is not a live figure either and says so.
    [[nodiscard]] bool allPnlLive() const;

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
    // The AI column's colour and tooltip, so data() stays a role switch rather than
    // a place where three columns' formatting accumulates.
    // One switch per role, so data() stays a role switch and each column's
    // formatting lives in one place.
    [[nodiscard]] QVariant foregroundFor(const Position &p, qint32 col, qint32 row) const;
    [[nodiscard]] QVariant tooltipFor(const Position &p, qint32 col, qint32 row) const;
    [[nodiscard]] QVariant aiColour(const QString &symbol) const;
    [[nodiscard]] QString aiTooltip(const QString &symbol) const;

    [[nodiscard]] double toDisplay(double usd) const { return usd * m_eurPerUsd; }
    [[nodiscard]] QString displayText(const Position &p, qint32 column, qint32 row) const;
    // The P/L to show for a row: the live mark when there is one, else eToro's last
    // snapshot figure. Defined out of line (one TU) — see Models.cpp on comdat coverage.
    [[nodiscard]] double shownPnl(qint32 row) const;
    // The P/L cell's text and its tooltip (which states WHICH rate the figure is marked
    // at, or why it is not marked). Both out of line, and out of data()/displayText(),
    // so those two stay off the complexity ratchet.
    [[nodiscard]] QString pnlText(qint32 row) const;
    [[nodiscard]] QColor pnlColor(qint32 row) const;
    [[nodiscard]] QString pnlTooltip(const Position &p, qint32 row) const;
    // The SL/TP cells show what the leg is worth; their tooltip states the instrument
    // RATE that triggers it, its distance from the open rate and from the current rate
    // (m_markRate), plus the trailing-stop note where that applies.
    [[nodiscard]] QString slTpTooltip(const Position &p, qint32 column, qint32 row) const;
    // dataChanged over a rectangle, minus the cell currently being edited (up to
    // four sub-rectangles) — the single place the edit guard is enforced.
    void emitChangedSkippingEditor(qint32 firstRow, qint32 lastRow, qint32 firstCol,
                                   qint32 lastCol);

    QList<Position> m_positions;
    // Live-marked P/L per row and whether that mark is current. Sized with the row
    // set, so a re-price only writes into existing slots (REQ-N-006).
    QList<double> m_plLive;
    QList<bool> m_plIsLive;
    // Rate each trade would close at right now (0 = no current quote), for the SL/TP
    // tooltips' "…from the current rate" clause.
    QList<double> m_markRate;
    QSet<QString> m_marked;
    QHash<QString, trading::HoldVerdict> m_aiBySymbol;   // the model's read per symbol
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
