// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Unit tests for position/money arithmetic (DES-DOM-POS).

#include "domain/PositionMath.h"

#include <QLocale>
#include <QtTest/QtTest>

#include <cmath>

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

    //! @tstid TS-POS-005 @design DES-DOM-POS
    // @relation(REQ-F-025, scope=function)
    void TS_POS_005_pnlIsEtorosOwnIdentity()
    {
        // Every figure below was read off a REAL eToro account (2026-07-30) and
        // cross-checked against unrealizedPnL.pnL for the same position: eToro's P/L
        // is units × (close rate − open rate) × conversion rate, to the cent, with no
        // fee or spread term — and the close rate is the BID for a long.
        Position nsdq;                 // NSDQ100.24-7, x10 long
        nsdq.units = 1.545335;
        nsdq.openRate = 27979.10;
        Quote q;
        q.bid = 28075.99;              // = unrealizedPnL.closeRate of that snapshot
        q.ask = 28080.91;
        QVERIFY(std::abs(positionPnl(nsdq, q) - 149.73) < 0.005);   // eToro: 149.73

        Position gold;                 // GOLD.24-7, x20 long
        gold.units = 19.896345;
        gold.openRate = 4098.24;
        Quote gq;
        gq.bid = 4102.29;
        gq.ask = 4103.53;
        QVERIFY(std::abs(positionPnl(gold, gq) - 80.58) < 0.005);   // eToro: 80.58

        // A short marks at the ASK, and a quote-currency instrument (HKG50 in HKD)
        // converts the move into the account currency — 100 units × 6.5 HKD × 0.1275.
        Position hkShort;
        hkShort.isBuy = false;
        hkShort.units = 100.0;
        hkShort.openRate = 25900.0;
        Quote hq;
        hq.bid = 25886.5;
        hq.ask = 25893.5;
        hq.conversionBid = 0.1274994676897224;
        hq.conversionAsk = 0.1274994676897224;
        QVERIFY(std::abs(positionPnl(hkShort, hq) - 82.87) < 0.005);
        // Ignoring the conversion would report 650 — the whole point of carrying it.
        QVERIFY(positionPnl(hkShort, hq) < 100.0);

        // Without units (a simulated position) the account-currency value per point
        // stands in, and an unusable quote yields nothing rather than a wrong number.
        Position sim = makePosition();
        sim.units = 0.0;
        QCOMPARE(positionPnl(sim, Quote{}), 0.0);
        Quote sq;
        sq.bid = 5100.0;
        sq.ask = 5101.0;
        QCOMPARE(positionPnl(sim, sq), (1000.0 * 20.0 / 5000.0) * 100.0);
    }

    //! @tstid TS-POS-006 @design DES-DOM-MODEL
    // @relation(REQ-F-025, scope=function)
    void TS_POS_006_quoteSidesSpreadAndAge()
    {
        // Quote is what decides WHERE a position is marked, so each of its rules is
        // pinned here (and every condition in them gets MC/DC coverage).
        Quote q;
        QVERIFY(!q.isValid());                  // no bid = nothing to mark against
        q.bid = 4102.71;
        q.ask = 4103.95;
        QVERIFY(q.isValid());
        QCOMPARE(q.closeRate(true), 4102.71);   // a long closes at the bid…
        QCOMPARE(q.closeRate(false), 4103.95);  // …a short at the ask
        QCOMPARE(q.spread(), 4103.95 - 4102.71);
        QCOMPARE(q.conversion(true), 1.0);      // USD-quoted default
        QCOMPARE(q.conversion(false), 1.0);

        // One-sided row: the known side stands in for the missing one, both ways.
        Quote bidOnly;
        bidOnly.bid = 100.0;
        bidOnly.ask = 0.0;
        QCOMPARE(bidOnly.closeRate(false), 100.0);
        QCOMPARE(bidOnly.spread(), 0.0);        // unknown spread is 0, never negative
        Quote askOnly;
        askOnly.ask = 100.0;
        QCOMPARE(askOnly.closeRate(true), 100.0);

        // A crossed/degenerate row has no usable spread either.
        Quote crossed;
        crossed.bid = 101.0;
        crossed.ask = 100.0;
        QCOMPARE(crossed.spread(), 0.0);

        // Conversion rates fall back to 1.0 rather than scaling a P/L to zero.
        Quote hkd;
        hkd.bid = 25886.5;
        hkd.conversionBid = 0.1275;
        hkd.conversionAsk = 0.0;
        QCOMPARE(hkd.conversion(true), 0.1275);
        QCOMPARE(hkd.conversion(false), 1.0);

        // Age is the PRICE's age, and −1 while eToro sent no stamp at all.
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QCOMPARE(q.ageMs(now), qint64{-1});
        q.asOf = now.addSecs(-90);
        QVERIFY(q.ageMs(now) >= 90 * 1000LL);
        QVERIFY(q.ageMs(now) < kQuoteStaleMs);  // 90 s still marks a position
        q.asOf = now.addSecs(-11 * 60LL);       // the .24-7 feed's measured lag
        QVERIFY(q.ageMs(now) > kQuoteStaleMs);  // 11 min does not

        // InstrumentFees::isValid: any non-zero leg counts (a negative one is a
        // credit), all-zero does not. The braces are named to keep the QVERIFY macro
        // from seeing four comma-separated arguments.
        const InstrumentFees none;
        const InstrumentFees buyNight{0.1, 0.0, 0.0, 0.0};
        const InstrumentFees sellCredit{0.0, -0.2, 0.0, 0.0};
        const InstrumentFees buyWeekend{0.0, 0.0, 0.3, 0.0};
        const InstrumentFees sellWeekend{0.0, 0.0, 0.0, 0.4};
        QVERIFY(!none.isValid());
        QVERIFY(buyNight.isValid());
        QVERIFY(sellCredit.isValid());
        QVERIFY(buyWeekend.isValid());
        QVERIFY(sellWeekend.isValid());
    }

    //! @tstid TS-POS-011 @design DES-DOM-POS
    // @relation(REQ-F-002, scope=function)
    //
    // A CONFIRMED-CLOSED position leaves the open-trades table at once, and a lagging
    // portfolio poll cannot put it back.
    //
    // This is not simply "remove the row". The close is confirmed by the broker's own
    // reply, but the table is fed by the PORTFOLIO poll, which this project has measured
    // running behind its own truth — so the next poll still lists the position and the row
    // reappears. A row that vanishes and returns reads as a close that FAILED, which is
    // worse than one that lingers.
    void TS_POS_011_aConfirmedCloseIsHiddenWhileThePollCatchesUp()
    {
        Position a = makePosition();
        a.positionId = QStringLiteral("111");
        Position b = makePosition();
        b.positionId = QStringLiteral("222");

        // The broker still reports both; 222 was confirmed closed one second ago.
        const QHash<QString, qint64> closed{{QStringLiteral("222"), 1000}};
        const CloseSuppression fresh =
            suppressClosedPositions({a, b}, closed, /*nowMs=*/2000, /*windowMs=*/30000);

        QCOMPARE(fresh.visible.size(), 1);
        QCOMPARE(fresh.visible.first().positionId, QStringLiteral("111"));
        QVERIFY(fresh.expired.isEmpty());
        // Still reported, so nothing may be forgotten yet — dropping it here is exactly
        // how the row would come back on the following poll.
        QVERIFY(fresh.confirmed.isEmpty());

        // Once the broker stops reporting it, the entry is CONFIRMED gone and the caller
        // may forget it. Without this the book would grow for the life of the session.
        const CloseSuppression agreed =
            suppressClosedPositions({a}, closed, /*nowMs=*/2000, /*windowMs=*/30000);
        QCOMPARE(agreed.visible.size(), 1);
        QCOMPARE(agreed.confirmed, QStringList{QStringLiteral("222")});
    }

    //! @tstid TS-POS-012 @design DES-DOM-POS
    // @relation(REQ-F-002, scope=function)
    //
    // The hiding is BOUNDED, and that bound is the safety property. If the broker is still
    // reporting the position after the window, the close did not take effect — so the row
    // comes back and is NAMED. Hiding it indefinitely would tell someone they were flat
    // while they still carried the risk, which is the most dangerous thing this table can
    // do; lingering for a second is merely untidy.
    void TS_POS_012_aCloseThatDidNotTakeEffectComesBackAndIsNamed()
    {
        Position stubborn = makePosition();
        stubborn.positionId = QStringLiteral("333");
        const QHash<QString, qint64> closed{{QStringLiteral("333"), 1000}};

        // 31 s later, still reported: show it again and say so.
        const CloseSuppression late =
            suppressClosedPositions({stubborn}, closed, /*nowMs=*/32000, /*windowMs=*/30000);
        QCOMPARE(late.visible.size(), 1);
        QCOMPARE(late.expired, QStringList{QStringLiteral("333")});

        // A clock that moved BACKWARDS (an NTP step, or a resume from suspend) expires the
        // entry too. The alternative is a negative age that never reaches the window, which
        // would hide the position forever — the failure this whole bound exists to prevent.
        const CloseSuppression rewound =
            suppressClosedPositions({stubborn}, closed, /*nowMs=*/500, /*windowMs=*/30000);
        QCOMPARE(rewound.visible.size(), 1);
        QCOMPARE(rewound.expired, QStringList{QStringLiteral("333")});

        // An empty book changes nothing at all — the common case must not be special.
        const CloseSuppression none =
            suppressClosedPositions({stubborn}, {}, /*nowMs=*/32000, /*windowMs=*/30000);
        QCOMPARE(none.visible.size(), 1);
        QVERIFY(none.expired.isEmpty());
        QVERIFY(none.confirmed.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPositionMath)
#include "tst_positionmath.moc"
