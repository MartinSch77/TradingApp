// Headless unit tests over the open-trades table model (DES-UI-POSMODEL):
// the in-place refresh path (REQ-N-006) and the SL/TP edit guard that keeps
// polls from overwriting a cell while its editor is open (REQ-F-012). The
// model is pure QAbstractTableModel — no view or widget is instantiated.

#include "ui/PositionsModel.h"

#include "ui/Palette.h"

#include <QColor>
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

        const QSignalSpy resets(&model, &QAbstractItemModel::modelAboutToBeReset);
        const QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

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

    //! @tstid TS-PM-003 @design DES-UI-POSMODEL
    // @relation(REQ-F-025, scope=function)
    void TS_PM_003_everyRowMarkedFromItsOwnLiveQuote()
    {
        // Regression: only the rows of the instrument ON SCREEN were re-priced, so a
        // trade on any other instrument showed whatever the last portfolio snapshot
        // said. With eToro publishing some instruments minutes behind real time, that
        // read tens of euro away from eToro's own screen on a fast-moving index.
        PositionsModel model;
        model.setDisplay(QStringLiteral("€"), 1.0);   // no FX scaling in the assertions

        Position live = makePosition(QStringLiteral("1"), 0.0, 0.0);
        live.instrumentId = 1;
        live.profit = -100.0;          // eToro's (delayed) snapshot figure
        Position other = makePosition(QStringLiteral("2"), 0.0, 0.0);
        other.instrumentId = 686;      // a different instrument: NOT the one on screen
        other.symbol = QStringLiteral("NSDQ100.24-7");
        other.units = 1.545335;
        other.openRate = 27979.10;
        other.profit = 84.98;          // what a stale snapshot claimed
        model.setPositions({live, other});

        const QDateTime now = QDateTime::currentDateTimeUtc();
        Quote fresh;                   // EURUSD, stamped now
        fresh.bid = 1.1400;
        fresh.ask = 1.1401;
        fresh.asOf = now;
        Quote current;                 // NSDQ100.24-7, stamped now: 28075.99 bid
        current.bid = 28075.99;
        current.ask = 28080.91;
        current.asOf = now;
        model.repriceOpenPnl({{1, fresh}, {686, current}}, now);

        // Both rows are marked from their own quote, eToro's identity: the second row
        // reads 149.73 — the figure eToro's own screen showed — not its stale 84.98.
        const auto plText = [&model](qint32 row) {
            return model.data(model.index(row, PositionsModel::ColPl), Qt::DisplayRole).toString();
        };
        QCOMPARE(plText(1), QStringLiteral("€149.73"));
        QCOMPARE(model.data(model.index(1, PositionsModel::ColPl), Qt::ForegroundRole)
                     .value<QColor>(),
                 trading::ui::kGreen);
        // …and the row on screen is marked the same way: 98920 × (1.1400 − 1.1373).
        QCOMPARE(plText(0), QStringLiteral("€267.08"));

        // A quote eToro published minutes ago must NOT mark the row: the figure would
        // move away from eToro's live one. The row keeps the snapshot value and says so.
        Quote delayed = current;
        delayed.asOf = now.addSecs(-11 * 60LL);   // the .24-7 feed's measured lag
        model.repriceOpenPnl({{1, fresh}, {686, delayed}}, now);
        QCOMPARE(plText(1), QStringLiteral("€84.98 *"));
        QCOMPARE(model.data(model.index(1, PositionsModel::ColPl), Qt::ForegroundRole)
                     .value<QColor>(),
                 trading::ui::kGrey);

        // No quote at all (a position just opened, or a market that never quoted) is
        // the same case, not a zero P/L.
        model.repriceOpenPnl({{1, fresh}}, now);
        QCOMPARE(plText(1), QStringLiteral("€84.98 *"));
    }

    //! @tstid TS-PM-005 @design DES-UI-POSMODEL
    // @relation(REQ-F-025, scope=function)
    void TS_PM_005_panelTotalsSumTheVisibleColumns()
    {
        PositionsModel model;
        model.setDisplay(QStringLiteral("€"), 0.5);   // a rate that is obvious in the sums

        Position a = makePosition(QStringLiteral("1"), 0.0, 0.0);
        a.instrumentId = 1;
        a.amount = 4000.0;        // → €2000 invested
        a.profit = 40.0;
        Position b = makePosition(QStringLiteral("2"), 0.0, 0.0);
        b.instrumentId = 2;
        b.amount = 1001.0;        // → €500.50, and the column ceils it to 501
        b.profit = -10.0;
        model.setPositions({a, b});

        // Without a quote, both rows show eToro's snapshot figures: (40 − 10) × 0.5.
        QCOMPARE(model.totalPnlDisplay(), 15.0);
        QVERIFY(!model.allPnlLive());
        // The invested total is the sum of the Amount column AS SHOWN (2000 + 501), so
        // adding the column up by hand gives the summary — not 2500.50.
        QCOMPARE(model.totalInvestedDisplay(), 2501.0);

        // One live quote: that row is marked from it, the other keeps its snapshot, and
        // the total stays flagged as not fully live.
        const QDateTime now = QDateTime::currentDateTimeUtc();
        Quote q;                  // 98920 units × (1.1383 − 1.1373) = 98.92 USD
        q.bid = 1.1383;
        q.ask = 1.1384;
        q.asOf = now;
        model.repriceOpenPnl({{1, q}}, now);
        QVERIFY(!model.allPnlLive());
        QCOMPARE(qRound(model.totalPnlDisplay() * 100.0), qRound((98.92 - 10.0) * 0.5 * 100.0));

        // Both live → the total is a live figure.
        model.repriceOpenPnl({{1, q}, {2, q}}, now);
        QVERIFY(model.allPnlLive());
    }

    //! @tstid TS-PM-004 @design DES-UI-POSMODEL
    // @relation(REQ-F-025, scope=function)
    void TS_PM_004_slTpTooltipNamesTheTriggerRate()
    {
        // The SL/TP cells state what the leg is WORTH; the rate that actually triggers it
        // was nowhere in the table, so the tooltip states it — with the distance from the
        // open rate and from the rate the trade would close at right now.
        PositionsModel model;
        model.setDisplay(QStringLiteral("€"), 1.0);
        Position p = makePosition(QStringLiteral("1"), 1.1300, 1.1500);
        p.instrumentId = 1;
        model.setPositions({p});

        const auto tip = [&model](qint32 col) {
            return model.data(model.index(0, col), Qt::ToolTipRole).toString();
        };
        // Before any quote: the trigger rate and its distance from the open rate.
        QVERIFY(tip(PositionsModel::ColSl).contains(QStringLiteral("triggers when EURUSD "
                                                                  "trades at 1.1300")));
        QVERIFY(tip(PositionsModel::ColSl).contains(QStringLiteral("0.0073 below the open rate "
                                                                  "1.1373")));
        QVERIFY(tip(PositionsModel::ColTp).contains(QStringLiteral("trades at 1.1500")));
        QVERIFY(tip(PositionsModel::ColTp).contains(QStringLiteral("0.0127 above the open rate")));
        // No "current rate" clause while there is no quote to name one.
        QVERIFY(!tip(PositionsModel::ColSl).contains(QStringLiteral("the current")));

        // With a live quote the distance from the closing side is stated too (a long
        // closes at the bid: 1.1400 → the stop sits 0.0100 below it).
        const QDateTime now = QDateTime::currentDateTimeUtc();
        Quote q;
        q.bid = 1.1400;
        q.ask = 1.1401;
        q.asOf = now;
        model.repriceOpenPnl({{1, q}}, now);
        QVERIFY(tip(PositionsModel::ColSl).contains(QStringLiteral("0.0100 below the current "
                                                                  "1.1400")));
        QVERIFY(tip(PositionsModel::ColTp).contains(QStringLiteral("0.0100 above the current "
                                                                  "1.1400")));

        // A trailing stop says so; a leg that is off says THAT rather than a rate of 0.
        Position trailing = p;
        trailing.trailingStop = true;
        trailing.takeProfitRate = 0.0;
        model.setPositions({trailing});
        QVERIFY(tip(PositionsModel::ColSl).contains(QStringLiteral("Trailing")));
        QVERIFY(tip(PositionsModel::ColTp).startsWith(QStringLiteral("No take-profit on this "
                                                                    "trade")));
    }
};

QTEST_GUILESS_MAIN(TestPositionsModel)
#include "tst_positionsmodel.moc"
