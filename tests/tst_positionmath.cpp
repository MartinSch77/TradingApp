// Unit tests for position/money arithmetic (DES-DOM-POS).

#include "domain/PositionMath.h"

#include <QLocale>
#include <QtTest/QtTest>

using namespace trading;

namespace {

Position makePosition()
{
    Position p;
    p.isBuy = true;
    p.amount = 1000.0;   // account currency
    p.leverage = 20.0;
    p.openRate = 5000.0;
    p.units = 4.0;       // 1000×20/5000
    return p;
}

} // namespace

class TestPositionMath : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-POS-001 @design DES-DOM-POS
    // @relation(REQ-F-016, scope=function)
    void TS_POS_001_priceDecimalsByMagnitude()
    {
        QCOMPARE(priceDecimals(6543.21), 2);   // index level
        QCOMPARE(priceDecimals(45.0), 3);
        QCOMPARE(priceDecimals(1.0834), 4);    // forex
        QCOMPARE(priceDecimals(0.63), 5);
    }

    //! @tstid TS-POS-002 @design DES-DOM-POS
    // @relation(REQ-F-003, REQ-F-016, scope=function)
    void TS_POS_002_valuePerPointIdentity()
    {
        Position p = makePosition();
        // amount × leverage / openRate — no FX rate needed.
        QCOMPARE(accountValuePerPoint(p), (1000.0 * 20.0) / 5000.0);
        // Unknown notional (e.g. a synthetic position) falls back to units.
        p.amount = 0.0;
        QCOMPARE(accountValuePerPoint(p), p.units);
        // No open rate → undefined.
        p.openRate = 0.0;
        QCOMPARE(accountValuePerPoint(p), 0.0);
    }

    //! @tstid TS-POS-003 @design DES-DOM-POS
    // @relation(REQ-F-003, REQ-F-016, scope=function)
    void TS_POS_003_slTpAmountsAndSigns()
    {
        const Position p = makePosition();
        // Off legs render empty.
        QCOMPARE(slTpAmountText(p, 0.0, 0.0), QString());

        // 100 points below the open on 4 value-per-point = 400 account ccy;
        // ×0.9 EUR/USD = 360 in display currency.
        QCOMPARE(slTpAmountText(p, 4900.0, 0.0), QLocale().toString(400.0, 'f', 2));
        QCOMPARE(slTpAmountText(p, 4900.0, 0.9), QLocale().toString(360.0, 'f', 2));

        // A long's stop below the open is a loss (negative), above a locked gain.
        Position sl = p;
        sl.stopLossRate = 4900.0;
        QVERIFY(slSignedAmountText(sl, 0.0).startsWith(QLatin1Char('-')));
        sl.stopLossRate = 5100.0;
        QVERIFY(slSignedAmountText(sl, 0.0).startsWith(QLatin1Char('+')));
        // Mirrored for a short.
        sl.isBuy = false;
        QVERIFY(slSignedAmountText(sl, 0.0).startsWith(QLatin1Char('-')));
    }

    //! @tstid TS-POS-004 @design DES-DOM-POS
    // @relation(REQ-F-025, scope=function)
    void TS_POS_004_closedSincePreviousIds()
    {
        const auto pos = [](const QString &id) {
            Position p;
            p.positionId = id;
            return p;
        };
        const QList<Position> before{pos(QStringLiteral("1")), pos(QStringLiteral("2")),
                                    pos(QStringLiteral("3"))};

        // Nothing closed: same set, order irrelevant.
        QVERIFY(closedSincePreviousIds(before, {pos(QStringLiteral("3")),
                                                pos(QStringLiteral("2")),
                                                pos(QStringLiteral("1"))})
                    .isEmpty());

        // One gone, reported by id; a newly opened trade does not confuse it.
        QCOMPARE(closedSincePreviousIds(before, {pos(QStringLiteral("1")),
                                                pos(QStringLiteral("3")),
                                                pos(QStringLiteral("4"))}),
                 QStringList{QStringLiteral("2")});

        // Several gone at once, in the order they were shown.
        QCOMPARE(closedSincePreviousIds(before, {pos(QStringLiteral("2"))}),
                 QStringList({QStringLiteral("1"), QStringLiteral("3")}));

        // The whole portfolio closed.
        QCOMPARE(closedSincePreviousIds(before, {}).size(), 3);

        // First snapshot: no previous set, so nothing can have disappeared —
        // otherwise every app start would fire a closed-trades refresh.
        QVERIFY(closedSincePreviousIds({}, before).isEmpty());

        // An empty id is not a trade and must never be reported as closed.
        QVERIFY(closedSincePreviousIds({pos(QString())}, {}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPositionMath)
#include "tst_positionmath.moc"
