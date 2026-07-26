#include "ui/PositionsModel.h"

#include "domain/PositionMath.h"

#include <QColor>
#include <QLocale>

#include <cmath>

namespace {
const QColor kGreen(0x25, 0xb5, 0x63);
const QColor kRed(0xe3, 0x55, 0x55);
const QColor kGrey(0x9a, 0x9a, 0x9a);
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
        emit dataChanged(index(0, 0), index(rowCount() - 1, ColCount - 1));
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
        m_plDelta.fill(0.0);
        if (!m_positions.isEmpty()) {
            // Values changed, identities didn't: open editors/marks survive.
            emit dataChanged(index(0, ColAmount),
                             index(rowCount() - 1, ColCount - 1));
        }
        return;
    }
    beginResetModel();
    m_positions = positions;
    m_plDelta = QList<double>(positions.size(), 0.0);
    QSet<QString> stillOpen;
    for (const Position &p : positions) {
        static_cast<void>(stillOpen.insert(p.positionId));
    }
    m_marked.intersect(stillOpen);  // drop marks of closed positions
    endResetModel();
}

void PositionsModel::repriceOpenPnl(const QString &symbol, double price, double anchorPrice)
{
    if ((price <= 0.0) || (anchorPrice <= 0.0)) {
        return;
    }
    for (qsizetype row = 0; row < m_positions.size(); ++row) {
        const Position &p = m_positions[row];
        if ((p.openRate <= 0.0) || (p.symbol.compare(symbol, Qt::CaseInsensitive) != 0)) {
            continue;
        }
        const double perPoint = trading::accountValuePerPoint(p);
        if (perPoint <= 0.0) {
            continue;
        }
        m_plDelta[row] = (p.isBuy ? 1.0 : -1.0) * perPoint * (price - anchorPrice);
        const QModelIndex idx = index(static_cast<qint32>(row), ColPl);
        emit dataChanged(idx, idx);
    }
}

void PositionsModel::setSlTpRates(qint32 row, double slRate, double tpRate)
{
    if ((row < 0) || (row >= m_positions.size())) {
        return;
    }
    m_positions[row].stopLossRate = slRate;
    m_positions[row].takeProfitRate = tpRate;
    emit dataChanged(index(row, ColSl), index(row, ColTp));
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
    case ColPl: {
        const double profitUsd = p.profit + m_plDelta.value(row, 0.0);
        const QString amount = QLocale().toString(qAbs(toDisplay(profitUsd)), 'f', 2);
        return ((profitUsd < 0.0) ? QStringLiteral("-") : QString()) + m_ccy + amount;
    }
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
            const double profitUsd = p.profit + m_plDelta.value(index.row(), 0.0);
            return (profitUsd >= 0.0) ? kGreen : kRed;
        }
        if (col == ColCloseCost) {
            return kGrey;
        }
        return {};
    case Qt::ToolTipRole:
        if (col == ColCloseCost) {
            return QStringLiteral(
                "Estimated cost to close this trade at the current price — eToro attributes "
                "half the bid/ask spread to the exit, i.e. roughly spread/2 × units, matching "
                "its close dialog. eToro's P/L already reflects this (a long is valued at the "
                "bid, a short at the ask).");
        }
        if ((col == ColSl) && p.trailingStop && (p.stopLossRate > 0.0)) {
            return QStringLiteral("Trailing stop-loss (follows the price in your favour)");
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
