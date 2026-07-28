// Headless unit tests over the open-trades table model (DES-UI-POSMODEL):
// the in-place refresh path (REQ-N-006) and the SL/TP edit guard that keeps
// polls from overwriting a cell while its editor is open (REQ-F-012). The
// model is pure QAbstractTableModel — no view or widget is instantiated.

#include "ui/PositionsModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <algorithm>

namespace {

Position makePosition(const QString &id, double slRate, double tpRate)
{
    Position p;
    p.positionId = id;
    p.symbol = QStringLiteral("EURUSD");
    p.isBuy = true;
    p.amount = 3750.0;
    p.leverage = 30.0;
    p.openRate = 1.1373;
    p.units = 98920.0;
    p.stopLossRate = slRate;
    p.takeProfitRate = tpRate;
    return p;
}

// True when any dataChanged emission recorded by `spy` covers (row, col).
bool cellTouched(const QSignalSpy &spy, qint32 row, qint32 col)
{
    return std::ranges::any_of(spy, [row, col](const QList<QVariant> &emission) {
        const auto topLeft = emission.at(0).toModelIndex();
        const auto bottomRight = emission.at(1).toModelIndex();
        const bool rowIn = (topLeft.row() <= row) && (row <= bottomRight.row());
        const bool colIn = (topLeft.column() <= col) && (col <= bottomRight.column());
        return rowIn && colIn;
    });
}

} // namespace

class TestPositionsModel : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-PM-001 @design DES-UI-POSMODEL
    // @relation(REQ-N-006, scope=function)
    void TS_PM_001_samePositionSetUpdatesInPlace()
    {
        PositionsModel model;
        model.setPositions({makePosition(QStringLiteral("1"), 1.13, 1.15),
                            makePosition(QStringLiteral("2"), 0.0, 0.0)});

        QSignalSpy resets(&model, &QAbstractItemModel::modelAboutToBeReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

        // Same ids in the same order: values refresh via dataChanged, no reset —
        // open editors and checkbox marks survive by construction.
        model.setPositions({makePosition(QStringLiteral("1"), 1.12, 1.16),
                            makePosition(QStringLiteral("2"), 1.10, 0.0)});
        QCOMPARE(resets.count(), 0);
        QVERIFY(!changed.isEmpty());

        // A changed row set must reset instead (a position was closed).
        model.setPositions({makePosition(QStringLiteral("2"), 1.10, 0.0)});
        QCOMPARE(resets.count(), 1);
        QCOMPARE(model.rowCount(), 1);
    }

    //! @tstid TS-PM-002 @design DES-UI-POSMODEL
    // @relation(REQ-F-012, scope=function)
    void TS_PM_002_editedCellSurvivesRefreshes()
    {
        PositionsModel model;
        model.setPositions({makePosition(QStringLiteral("1"), 1.13, 1.15),
                            makePosition(QStringLiteral("2"), 1.10, 1.20)});
        const QModelIndex slCell = model.index(0, PositionsModel::ColSl);
        const QString before = model.data(slCell, Qt::EditRole).toString();

        // The user opens the SL editor of row 0 (the delegate reports it), then
        // a portfolio poll, an FX update and an SL/TP echo all land mid-edit.
        model.beginCellEdit(0, PositionsModel::ColSl);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setPositions({makePosition(QStringLiteral("1"), 1.11, 1.14),
                            makePosition(QStringLiteral("2"), 1.09, 1.21)});
        model.setDisplay(QStringLiteral("€"), 0.88);
        model.setSlTpRates(0, 1.105, 1.145);

        // Every other cell refreshed; the edited cell got NO dataChanged (Qt
        // re-fills an open editor from the model on dataChanged — the user's
        // typing must not be overwritten)…
        QVERIFY(!changed.isEmpty());
        QVERIFY(!cellTouched(changed, 0, PositionsModel::ColSl));
        QVERIFY(cellTouched(changed, 0, PositionsModel::ColTp));
        QVERIFY(cellTouched(changed, 1, PositionsModel::ColSl));

        // …while the stored value kept updating underneath.
        QVERIFY(model.data(slCell, Qt::EditRole).toString() != before);

        // Editing ends: the held-back cell catches up with one dataChanged.
        changed.clear();
        model.endCellEdit();
        QCOMPARE(changed.count(), 1);
        QVERIFY(cellTouched(changed, 0, PositionsModel::ColSl));

        // With no editor open, refreshes touch the SL cell again as usual.
        changed.clear();
        model.setSlTpRates(0, 1.10, 1.15);
        QVERIFY(cellTouched(changed, 0, PositionsModel::ColSl));
    }
};

QTEST_GUILESS_MAIN(TestPositionsModel)
#include "tst_positionsmodel.moc"
