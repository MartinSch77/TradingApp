#include "ui/PositionsModel.h"

#include "domain/PositionMath.h"
#include "ui/Palette.h"

#include <QColor>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
using trading::ui::kGreen;
using trading::ui::kGrey;
using trading::ui::kRed;

// "225.43 below" / "1,554.97 above" — a signed price distance said in words, so an
// SL/TP tooltip reads the same whichever side of the reference rate the leg sits on
// (a stop may legitimately sit on the winning side).
QString relativeText(double delta, qint32 decimals)
{
    return QLocale().toString(std::abs(delta), 'f', decimals)
           + ((delta < 0.0) ? QStringLiteral(" below") : QStringLiteral(" above"));
}
}  // namespace

PositionsModel::PositionsModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void PositionsModel::setDisplay(const QString &ccySymbol, double eurPerUsd)
{
    m_ccy = ccySymbol;
    if (eurPerUsd > 0.0) {
        m_eurPerUsd = eurPerUsd;
    }
    if (!m_positions.isEmpty()) {
        emitChangedSkippingEditor(0, rowCount() - 1, 0, ColCount - 1);
    }
    emit headerDataChanged(Qt::Horizontal, ColPl, ColTp);
}

void PositionsModel::setPositions(const QList<Position> &positions)
{
    bool sameRows = m_positions.size() == positions.size();
    for (qsizetype i = 0; sameRows && (i < positions.size()); ++i) {
        sameRows = m_positions[i].positionId == positions[i].positionId;
    }
    if (sameRows) {
        m_positions = positions;
        // Deliberately NOT clearing m_plIsLive/m_plLive: the snapshot was marked from the
        // same quote book this model re-prices from, so a row that was live still is.
        // Clearing them made every row flash grey with the not-live marker on each ~3 s
        // portfolio poll until the next tick's re-price restored it.
        if (!m_positions.isEmpty()) {
            // Values changed, identities didn't: open editors/marks survive (an
            // SL/TP cell being edited is skipped — the values still updated).
            emitChangedSkippingEditor(0, rowCount() - 1, ColAmount, ColCount - 1);
        }
        return;
    }
    // The row set changes: the reset closes any open editor with it.
    m_editRow = -1;
    m_editCol = -1;
    beginResetModel();
    m_positions = positions;
    m_plLive = QList<double>(positions.size(), 0.0);
    m_plIsLive = QList<bool>(positions.size(), false);
    m_markRate = QList<double>(positions.size(), 0.0);
    QSet<QString> stillOpen;
    for (const Position &p : positions) {
        static_cast<void>(stillOpen.insert(p.positionId));
    }
    static_cast<void>(m_marked.intersect(stillOpen));  // drop marks of closed positions
    endResetModel();
}

void PositionsModel::repriceOpenPnl(const QHash<qint64, Quote> &quotes,
                                    const QDateTime &nowUtc)
{
    for (qsizetype row = 0; row < m_positions.size(); ++row) {
        const Position &p = m_positions[row];
        const Quote quote = quotes.value(p.instrumentId);
        // Mark only against a quote that is current. eToro publishes some instruments
        // minutes behind (worst measured: the .24-7 index variants), and marking off
        // such a row moves the figure away from the live one eToro itself shows. An
        // unstamped quote (age -1) passes — fail open, as the market-open gate does.
        const bool live = (p.openRate > 0.0) && quote.isValid()
                          && (quote.ageMs(nowUtc) <= trading::kQuoteStaleMs);
        const double value = live ? trading::positionPnl(p, quote) : 0.0;
        // The rate this trade would close at right now, for the SL/TP tooltips (0 when
        // there is none — those then simply omit the "…from the current rate" clause).
        // Assigned before the skip below: a tooltip is pulled on hover, not repainted.
        m_markRate[row] = live ? quote.closeRate(p.isBuy) : 0.0;
        // Skip the emission when nothing moved: this runs on every price tick and the
        // view repaints per dataChanged (REQ-N-006). qFuzzyCompare is unusable here —
        // it is false for 0.0 vs 0.0, and a flat position is exactly 0.
        if ((m_plIsLive[row] == live) && (std::abs(m_plLive[row] - value) < 1e-9)) {
            continue;
        }
        m_plIsLive[row] = live;
        m_plLive[row] = value;
        const QModelIndex idx = index(static_cast<qint32>(row), ColPl);
        emit dataChanged(idx, idx);
    }
}

double PositionsModel::shownPnl(qint32 row) const
{
    return m_plIsLive.value(row, false) ? m_plLive.value(row, 0.0)
                                        : m_positions.value(row).profit;
}

QString PositionsModel::pnlText(qint32 row) const
{
    const double profitUsd = shownPnl(row);
    const QString amount = QLocale().toString(qAbs(toDisplay(profitUsd)), 'f', 2);
    // A trailing "*" (plus grey, plus the tooltip) = no current quote for this
    // instrument, so the figure is eToro's last snapshot rather than a mark of this
    // tick. Silently showing such a figure as live is what made the column drift from
    // eToro's own screen. ASCII on purpose: a marker only renders as a marker if the
    // user's font has the glyph, and the ⇅ of the SL column is the one exception earned.
    const QString marker = m_plIsLive.value(row, false) ? QString() : QStringLiteral(" *");
    return ((profitUsd < 0.0) ? QStringLiteral("-") : QString()) + m_ccy + amount + marker;
}

QColor PositionsModel::pnlColor(qint32 row) const
{
    // Grey says "this is not a live figure": no current quote, so the cell shows eToro's
    // last snapshot rather than a mark of this tick (and pnlText appends the "*").
    if (!m_plIsLive.value(row, false)) {
        return kGrey;
    }
    return (shownPnl(row) >= 0.0) ? kGreen : kRed;
}

QString PositionsModel::slTpTooltip(const Position &p, qint32 column, qint32 row) const
{
    // The cells state what the leg is WORTH; the tooltip states the instrument rate that
    // triggers it, and how far that is from the open rate and from the current one.
    const bool isSl = (column == ColSl);
    const double rate = isSl ? p.stopLossRate : p.takeProfitRate;
    const QString leg = isSl ? QStringLiteral("Stop-loss") : QStringLiteral("Take-profit");
    if (rate <= 0.0) {
        return QStringLiteral("No %1 on this trade — nothing closes it automatically %2.")
            .arg(leg.toLower(), isSl ? QStringLiteral("if the price moves against you")
                                     : QStringLiteral("once it is in profit"));
    }
    // Each part in its own local: several function calls inside one .arg() list are
    // evaluated in an unspecified order (MISRA C++ 2023 4.6.1), and naming them makes
    // the sentence being built readable.
    const qint32 decimals = trading::priceDecimals(rate);
    const QString rateText = QLocale().toString(rate, 'f', decimals);
    const QString fromOpen = relativeText(rate - p.openRate, decimals);
    const QString openText =
        QLocale().toString(p.openRate, 'f', trading::priceDecimals(p.openRate));
    QString text = QStringLiteral("%1 triggers when %2 trades at %3 — %4 the open rate %5")
                       .arg(leg, p.symbol, rateText, fromOpen, openText);
    const double mark = m_markRate.value(row, 0.0);
    if (mark > 0.0) {
        const QString fromNow = relativeText(rate - mark, decimals);
        const QString markText = QLocale().toString(mark, 'f', decimals);
        text += QStringLiteral(", %1 the current %2").arg(fromNow, markText);
    }
    // The cell's own figure, with the currency symbol placed after any sign — a loss
    // reads as -€302.39, so the tooltip states exactly what the cell states. Written
    // without a parenthesised quoted pair on purpose: MISRA C++ 2023 5.7.2 reads that
    // shape as commented-out code.
    QString money = isSl ? trading::slSignedAmountText(p, m_eurPerUsd)
                         : trading::slTpAmountText(p, rate, m_eurPerUsd);
    if (money.startsWith(QLatin1Char('+')) || money.startsWith(QLatin1Char('-'))) {
        static_cast<void>(money.insert(1, m_ccy));   // returns *this; discard deliberately
    } else {
        static_cast<void>(money.prepend(m_ccy));
    }
    text += QStringLiteral(". Closing there is %1 (the cell's figure).").arg(money);
    if (isSl && p.trailingStop) {
        text += QStringLiteral(" Trailing: the rate follows the price in your favour, so the "
                               "trigger moves up (long) / down (short) with it.");
    }
    return text;
}

QString PositionsModel::pnlTooltip(const Position &p, qint32 row) const
{
    // eToro's own identity, verified against its /pnl payload position by position:
    // units × (close rate − open rate) × conversion rate, marked at the bid for a long
    // and the ask for a short. No fee or spread term — the spread a long pays for is
    // already inside its open rate.
    // m_markRate — not Position::apiCloseRate — is the rate the SHOWN figure is marked
    // at: the latter is only the rate of the last portfolio poll, and printed as 0 for a
    // row the poll had not marked yet.
    const double mark = m_markRate.value(row, 0.0);
    const QString markText = QLocale().toString(mark, 'f', trading::priceDecimals(mark));
    const QString side = p.isBuy ? QStringLiteral("the bid, where a long closes")
                                 : QStringLiteral("the ask, where a short closes");
    const QString how =
        m_plIsLive.value(row, false)
            ? QStringLiteral("marked at the live rate %1 (%2), the same way eToro marks it")
                  .arg(markText, side)
            : QStringLiteral("marked \"*\" and grey because there is no CURRENT quote for %1 "
                             "right now (its market is closed, or eToro has not published a "
                             "price for it in the last two minutes) — this is eToro's last "
                             "snapshot figure, left un-marked rather than marked wrong")
                  .arg(p.symbol);
    const QString figure = QLocale().toString(shownPnl(row), 'f', 2);
    return QStringLiteral("$%1 in the USD account currency: %2. The column converts it at "
                          "the live EUR/USD rate.")
        .arg(figure, how);
}

void PositionsModel::setSlTpRates(qint32 row, double slRate, double tpRate)
{
    if ((row < 0) || (row >= m_positions.size())) {
        return;
    }
    m_positions[row].stopLossRate = slRate;
    m_positions[row].takeProfitRate = tpRate;
    emitChangedSkippingEditor(row, row, ColSl, ColTp);
}

void PositionsModel::beginCellEdit(qint32 row, qint32 column)
{
    m_editRow = row;
    m_editCol = column;
}

void PositionsModel::endCellEdit()
{
    const qint32 row = m_editRow;
    const qint32 col = m_editCol;
    m_editRow = -1;
    m_editCol = -1;
    if ((row >= 0) && (row < rowCount()) && (col >= 0)) {
        const QModelIndex idx = index(row, col);
        emit dataChanged(idx, idx);  // catch up on the refreshes held back
    }
}

void PositionsModel::emitChangedSkippingEditor(qint32 firstRow, qint32 lastRow,
                                               qint32 firstCol, qint32 lastCol)
{
    const bool editorInRange = (m_editRow >= firstRow) && (m_editRow <= lastRow)
                               && (m_editCol >= firstCol) && (m_editCol <= lastCol);
    if (!editorInRange) {
        emit dataChanged(index(firstRow, firstCol), index(lastRow, lastCol));
        return;
    }
    // Split the rectangle around the edited cell: full-width bands above and
    // below its row, then the edited row minus the edited column.
    if (m_editRow > firstRow) {
        emit dataChanged(index(firstRow, firstCol), index(m_editRow - 1, lastCol));
    }
    if (m_editRow < lastRow) {
        emit dataChanged(index(m_editRow + 1, firstCol), index(lastRow, lastCol));
    }
    if (m_editCol > firstCol) {
        emit dataChanged(index(m_editRow, firstCol), index(m_editRow, m_editCol - 1));
    }
    if (m_editCol < lastCol) {
        emit dataChanged(index(m_editRow, m_editCol + 1), index(m_editRow, lastCol));
    }
}

SlTpEditGuardDelegate::SlTpEditGuardDelegate(PositionsModel *model, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_model(model)
{
}

QWidget *SlTpEditGuardDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                                             const QModelIndex &index) const
{
    m_model->beginCellEdit(index.row(), index.column());
    return QStyledItemDelegate::createEditor(parent, option, index);
}

void SlTpEditGuardDelegate::destroyEditor(QWidget *editor, const QModelIndex &index) const
{
    m_model->endCellEdit();
    QStyledItemDelegate::destroyEditor(editor, index);
}

double PositionsModel::totalInvestedDisplay() const
{
    // std::ceil per row, exactly as ColAmount renders it: the summary has to be the sum
    // the user gets from adding the visible column up.
    return std::accumulate(m_positions.cbegin(), m_positions.cend(), 0.0,
                           [this](double acc, const Position &p) {
                               return acc + std::ceil(toDisplay(p.amount));
                           });
}

double PositionsModel::totalPnlDisplay() const
{
    double total = 0.0;
    for (qsizetype row = 0; row < m_positions.size(); ++row) {
        total += shownPnl(static_cast<qint32>(row));
    }
    return toDisplay(total);
}

bool PositionsModel::allPnlLive() const
{
    return std::ranges::all_of(m_plIsLive, [](bool live) { return live; });
}

QStringList PositionsModel::markedIds() const
{
    QStringList ids;
    for (const Position &p : m_positions) {
        if (m_marked.contains(p.positionId)) {
            ids << p.positionId;
        }
    }
    return ids;
}

qint32 PositionsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<qint32>(m_positions.size());
}

qint32 PositionsModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColCount;
}

QString PositionsModel::displayText(const Position &p, qint32 column, qint32 row) const
{
    switch (column) {
    case ColMark:
        return p.positionId;
    case ColInstrument:
        return p.symbol;
    case ColSide:
        return p.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");
    case ColAmount:
        // Invested amount (account currency -> display), up to the next whole unit.
        return QLocale().toString(std::ceil(toDisplay(p.amount)), 'f', 0);
    case ColLev:
        return (p.leverage > 0.0) ? QStringLiteral("x%1").arg(p.leverage, 0, 'f', 0)
                                  : QStringLiteral("—");
    case ColOpen:
        return QLocale().toString(p.openRate, 'f', trading::priceDecimals(p.openRate));
    case ColUnits:
        return QLocale().toString(p.units, 'f', 4);
    case ColPl:
        return pnlText(row);
    case ColCloseCost:
        return p.closingCost > 0.0
                   ? m_ccy + QLocale().toString(toDisplay(p.closingCost), 'f', 2)
                   : QStringLiteral("—");
    case ColSl: {
        QString text = trading::slSignedAmountText(p, m_eurPerUsd);
        if (p.trailingStop && (p.stopLossRate > 0.0) && !text.isEmpty()) {
            text += QStringLiteral(" ⇅");
        }
        return text;
    }
    case ColTp:
        return trading::slTpAmountText(p, p.takeProfitRate, m_eurPerUsd);
    default:
        return {};
    }
}

QVariant PositionsModel::data(const QModelIndex &index, qint32 role) const
{
    if (!index.isValid() || (index.row() >= m_positions.size())) {
        return {};
    }
    const Position &p = m_positions[index.row()];
    const qint32 col = index.column();
    switch (role) {
    case Qt::DisplayRole:
        return displayText(p, col, index.row());
    case Qt::EditRole: {
        // Editors open on the plain amount, without the trailing-stop marker.
        QString text = displayText(p, col, index.row());
        static_cast<void>(text.remove(QStringLiteral(" ⇅")));
        return text;
    }
    case Qt::TextAlignmentRole:
        return static_cast<qint32>(Qt::AlignCenter);
    case Qt::CheckStateRole:
        if (col == ColMark) {
            return m_marked.contains(p.positionId) ? Qt::Checked : Qt::Unchecked;
        }
        return {};
    case Qt::ForegroundRole:
        if (col == ColSide) {
            return p.isBuy ? kGreen : kRed;
        }
        if (col == ColPl) {
            return pnlColor(index.row());
        }
        if (col == ColCloseCost) {
            return kGrey;
        }
        return {};
    case Qt::ToolTipRole:
        if (col == ColPl) {
            return pnlTooltip(p, index.row());
        }
        if (col == ColAmount) {
            return QStringLiteral(
                       "$%1 invested — USD account currency, as the eToro app shows it; "
                       "the column converts it at the live EUR/USD rate.")
                .arg(QLocale().toString(p.amount, 'f', 2));
        }
        if (col == ColCloseCost) {
            return QStringLiteral(
                "Estimated cost to close this trade at the current price — eToro attributes "
                "half the bid/ask spread to the exit, i.e. roughly spread/2 × units, matching "
                "its close dialog. eToro's P/L already reflects this (a long is valued at the "
                "bid, a short at the ask).");
        }
        if ((col == ColSl) || (col == ColTp)) {
            return slTpTooltip(p, col, index.row());
        }
        return {};
    default:
        return {};
    }
}

bool PositionsModel::setData(const QModelIndex &index, const QVariant &value, qint32 role)
{
    if (!index.isValid() || (index.row() >= m_positions.size())) {
        return false;
    }
    const qint32 col = index.column();
    if ((role == Qt::CheckStateRole) && (col == ColMark)) {
        const QString id = m_positions[index.row()].positionId;
        if (value.toInt() == Qt::Checked) {
            static_cast<void>(m_marked.insert(id));
        } else {
            static_cast<void>(m_marked.remove(id));
        }
        emit dataChanged(index, index, {Qt::CheckStateRole});
        return true;
    }
    if ((role == Qt::EditRole) && ((col == ColSl) || (col == ColTp))) {
        // Validation, rate conversion and the client call live in MainWindow;
        // it echoes the accepted values back via setSlTpRates().
        emit slTpEdited(index.row(), col, value.toString());
        return true;
    }
    return false;
}

Qt::ItemFlags PositionsModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    const qint32 col = index.column();
    if (col == ColMark) {
        return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
    }
    if ((col == ColSl) || (col == ColTp)) {
        return Qt::ItemIsEnabled | Qt::ItemIsEditable;
    }
    return Qt::ItemIsEnabled;
}

QVariant PositionsModel::headerData(qint32 section, Qt::Orientation orientation,
                                    qint32 role) const
{
    if ((orientation != Qt::Horizontal) || (role != Qt::DisplayRole)) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
    case ColMark: return QStringLiteral("Position");
    case ColInstrument: return QStringLiteral("Instrument");
    case ColSide: return QStringLiteral("Side");
    case ColAmount: return QStringLiteral("Amount");
    case ColLev: return QStringLiteral("Lev");
    case ColOpen: return QStringLiteral("Open");
    case ColUnits: return QStringLiteral("Units");
    case ColPl: return QStringLiteral("P/L (%1)").arg(m_ccy);
    case ColCloseCost: return QStringLiteral("Close (%1)").arg(m_ccy);
    case ColSl: return QStringLiteral("SL (%1)").arg(m_ccy);
    case ColTp: return QStringLiteral("TP (%1)").arg(m_ccy);
    default: return {};
    }
}
