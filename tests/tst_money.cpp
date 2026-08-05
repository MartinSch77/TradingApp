#include "domain/Money.h"
#include "domain/OrderRequestValidator.h"

#include <QTest>

using namespace trading;

namespace {

Money eur(double major)
{
    return Money::fromDouble(major, Currency::Eur);
}

// A request/context pair that is valid in every respect, so each test can spoil
// exactly one thing and see exactly one refusal.
OrderRequest goodRequest()
{
    OrderRequest r;
    r.isBuy = true;
    r.instrumentId = 27;   // SPX500
    r.amount = 500.0;
    r.leverage = 5.0;
    r.stopLossAmount = 100.0;
    r.takeProfitAmount = 150.0;
    return r;
}

OrderContext goodContext()
{
    OrderContext c;
    c.accountCurrency = Currency::Usd;
    c.orderCurrency = Currency::Usd;
    c.instrument.instrumentId = 27;
    c.instrument.symbol = QStringLiteral("SPX500");
    c.instrument.maxUnitsPerOrder = 0.0;   // unknown: check disabled
    c.leverageLadder = {1, 2, 5, 10, 20};
    c.marketRate = 5000.0;
    return c;
}

OrderAmounts amountsOf(const OrderRequest &r, Currency currency = Currency::Usd)
{
    return OrderAmounts{Money::fromDouble(r.amount, currency),
                        Money::fromDouble(r.stopLossAmount, currency),
                        Money::fromDouble(r.takeProfitAmount, currency)};
}

OrderValidation validateGood(const OrderRequest &r, const OrderContext &c)
{
    return validateOrderRequest(r, amountsOf(r), c);
}

} // namespace

class TestMoney : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-MONEY-001 @design DES-DOM-MONEY
    // @relation(REQ-N-008, scope=function)
    void TS_MONEY_001_amountsAreCountedNotApproximated()
    {
        // The case that motivates the type: a hundred additions of 0.10 is exactly
        // 10.00 in minor units, while the same sum in double arithmetic is not.
        Money sum = Money::zero(Currency::Eur);
        double drift = 0.0;
        for (int i = 0; i < 100; ++i) {
            sum += eur(0.10);
            drift += 0.10;
        }
        QCOMPARE(sum.minorUnits(), 1000);
        QCOMPARE(sum.toDouble(), 10.0);
        // …and the double really does drift, which is why the comparison below matters
        // rather than being a formality.
        QVERIFY(drift != 10.0);

        // A cap comparison at the boundary answers exactly, in both directions.
        const Money cap = eur(1000.00);
        QVERIFY(eur(1000.00) <= cap);
        QVERIFY(!(eur(1000.01) <= cap));
        QVERIFY(eur(999.99) < cap);

        // Rounding is DEFINED: half away from zero, both signs, on conversion…
        QCOMPARE(eur(0.005).minorUnits(), 1);
        QCOMPARE(eur(-0.005).minorUnits(), -1);
        QCOMPARE(eur(2.675).minorUnits(), 268);
        // …and on scaling, so "1% of this" is one reproducible number.
        QCOMPARE(eur(1234.56).scaledBy(0.01).minorUnits(), 1235);
        QCOMPARE(eur(100.00).scaledBy(0.005).minorUnits(), 50);
        QCOMPARE(eur(3.00).timesInt(7).minorUnits(), 2100);

        // A credit is money too: negatives survive every operation.
        QVERIFY(eur(-12.34).isNegative());
        QCOMPARE((eur(5.00) - eur(7.50)).minorUnits(), -250);
        QCOMPARE((-eur(5.00)).minorUnits(), -500);

        // Fractions for the risk-budget arithmetic, and a refusal instead of a wrong
        // ratio when the denominator is zero or the currencies differ.
        QCOMPARE(eur(250.00).fractionOf(eur(1000.00)), 0.25);
        QCOMPARE(eur(250.00).fractionOf(eur(0.00)), 0.0);
        QCOMPARE(eur(250.00).fractionOf(Money::fromDouble(1000.0, Currency::Usd)), 0.0);
    }

    //! @tstid TS-MONEY-002 @design DES-DOM-MONEY
    // @relation(REQ-N-008, scope=function)
    void TS_MONEY_002_invalidityIsReportedNeverGuessed()
    {
        // A default-constructed amount is INVALID rather than "0 EUR": a forgotten
        // assignment must not become a free trade.
        const Money none;
        QVERIFY(!none.isValid());
        QVERIFY(!none.isZero());
        QCOMPARE(none.toString(), QStringLiteral("invalid amount"));
        QVERIFY(Money::zero(Currency::Eur).isValid());
        QVERIFY(Money::zero(Currency::Eur).isZero());

        // Mixing currencies gives an invalid amount, not a plausible wrong one.
        const Money mixed = eur(10.00) + Money::fromDouble(10.0, Currency::Usd);
        QVERIFY(!mixed.isValid());
        QVERIFY(!(eur(10.00) - Money::fromDouble(1.0, Currency::Usd)).isValid());
        // And it is UNORDERED, so neither "<" nor ">=" can pass a cap check by
        // accident — the trap a bare double comparison walks straight into.
        QVERIFY(!(eur(1.00) < Money::fromDouble(1000.0, Currency::Usd)));
        QVERIFY(!(eur(1.00) >= Money::fromDouble(1000.0, Currency::Usd)));
        QVERIFY(eur(1.00) != Money::fromDouble(1.0, Currency::Usd));

        // A non-finite input is refused rather than converted to something arbitrary.
        QVERIFY(!Money::fromDouble(std::numeric_limits<double>::quiet_NaN(), Currency::Eur)
                     .isValid());
        QVERIFY(!Money::fromDouble(std::numeric_limits<double>::infinity(), Currency::Eur)
                     .isValid());
        QVERIFY(!Money::fromDouble(1.0e30, Currency::Eur).isValid());
        QVERIFY(!Money::fromDouble(1.0, Currency::Invalid).isValid());
        QVERIFY(!eur(1.00).scaledBy(std::numeric_limits<double>::quiet_NaN()).isValid());
        QVERIFY(!none.scaledBy(2.0).isValid());

        // The currency code round-trips, and an unknown code stays unknown — this is
        // what reads a broker's own `orderCurrency` field.
        QCOMPARE(currencyFromCode(QStringLiteral("usd")), Currency::Usd);
        QCOMPARE(currencyFromCode(QStringLiteral(" EUR ")), Currency::Eur);
        QCOMPARE(currencyFromCode(QStringLiteral("GBP")), Currency::Invalid);
        QCOMPARE(currencyCode(Currency::Usd), QStringLiteral("USD"));
        // An amount always prints with its currency: the ambiguity this type removes
        // must not come back through the display path.
        QVERIFY(eur(12.34).toString().endsWith(QStringLiteral("EUR")));
    }

    //! @tstid TS-ORDVAL-001 @design DES-DOM-ORDERVAL
    // @relation(REQ-N-009, scope=function)
    void TS_ORDVAL_001_aCorrectRequestPassesAndEveryFaultIsNamed()
    {
        QVERIFY(validateGood(goodRequest(), goodContext()).ok());
        QCOMPARE(validateGood(goodRequest(), goodContext()).summary(), QStringLiteral("valid"));

        // Each fault below is introduced ALONE, and the assertion is on the CODE: a
        // refusal nobody can count is a refusal nobody can act on.
        {   // an amount that is not exactly the validated stake — two different orders
            OrderRequest r = goodRequest();
            const OrderAmounts amounts{Money::fromDouble(500.0, Currency::Usd),
                                       Money::zero(Currency::Usd),
                                       Money::zero(Currency::Usd)};
            r.amount = 500.004;   // rounds to the same cents…
            QVERIFY(validateOrderRequest(r, amounts, goodContext()).ok());
            r.amount = 500.01;   // …this one does not
            QVERIFY(validateOrderRequest(r, amounts, goodContext())
                        .codes()
                        .contains(QStringLiteral("stake-mismatch")));
        }
        {   // a leverage the instrument does not offer — the x8-on-a-1/2/5/20-ladder case
            OrderRequest r = goodRequest();
            r.leverage = 8.0;
            QVERIFY(validateGood(r, goodContext())
                        .codes()
                        .contains(QStringLiteral("leverage-not-offered")));
            r.leverage = 0.5;
            QVERIFY(validateGood(r, goodContext())
                        .codes()
                        .contains(QStringLiteral("leverage-invalid")));
        }
        {   // the order currency that eToro accepts and then rejects at execution
            OrderContext c = goodContext();
            c.orderCurrency = Currency::Eur;
            QVERIFY(validateGood(goodRequest(), c)
                        .codes()
                        .contains(QStringLiteral("order-currency")));
            c.accountCurrency = Currency::Invalid;
            QVERIFY(validateGood(goodRequest(), c)
                        .codes()
                        .contains(QStringLiteral("account-currency-unknown")));
        }
        {   // the per-order unit cap (GOLD = 20 units), and its unknown-disables rule
            OrderContext c = goodContext();
            c.instrument.maxUnitsPerOrder = 0.4;   // 500 x 5 / 5000 = 0.5 units
            QVERIFY(validateGood(goodRequest(), c)
                        .codes()
                        .contains(QStringLiteral("units-over-cap")));
            c.instrument.maxUnitsPerOrder = 0.0;
            QVERIFY(validateGood(goodRequest(), c).ok());
        }
        {   // a stop bigger than the money at risk, and a losing geometry
            OrderRequest r = goodRequest();
            r.stopLossAmount = 600.0;
            QVERIFY(validateGood(r, goodContext())
                        .codes()
                        .contains(QStringLiteral("stop-over-stake")));
            r.stopLossAmount = 150.0;
            r.takeProfitAmount = 100.0;
            QVERIFY(validateGood(r, goodContext())
                        .codes()
                        .contains(QStringLiteral("reward-below-risk")));
        }
        {   // a limit order on the wrong side fills at once — the user asked to WAIT
            OrderRequest r = goodRequest();
            r.triggerRate = 5100.0;   // buy trigger above the market
            QVERIFY(validateGood(r, goodContext())
                        .codes()
                        .contains(QStringLiteral("trigger-wrong-side")));
            r.triggerRate = 4900.0;   // below: a real resting order
            QVERIFY(validateGood(r, goodContext()).ok());
            // A limit order needs no live quote of its own instrument…
            OrderContext c = goodContext();
            c.marketRate = 0.0;
            QVERIFY(validateGood(r, c).ok());
            // …while a MARKET order without one does not know what it is buying.
            r.triggerRate = 0.0;
            QVERIFY(validateGood(r, c).codes().contains(QStringLiteral("no-market-rate")));
        }
        {   // an unresolved instrument, and a context describing a different one
            OrderRequest r = goodRequest();
            r.instrumentId = 0;
            OrderContext c = goodContext();
            c.instrument.instrumentId = 0;
            QVERIFY(validateGood(r, c).codes().contains(QStringLiteral("instrument-unresolved")));
            r.instrumentId = 99;
            QVERIFY(validateGood(r, goodContext())
                        .codes()
                        .contains(QStringLiteral("instrument-mismatch")));
        }
    }

    //! @tstid TS-ORDVAL-002 @design DES-DOM-ORDERVAL
    // @relation(REQ-N-009, scope=function)
    void TS_ORDVAL_002_capsAndInvalidAmountsAreRefusedNotRepaired()
    {
        // The caps the armed session was granted under.
        OrderContext c = goodContext();
        c.minStake = Money::fromDouble(50.0, Currency::Usd);
        c.maxStakePerOrder = Money::fromDouble(400.0, Currency::Usd);
        QVERIFY(validateGood(goodRequest(), c)
                    .codes()
                    .contains(QStringLiteral("over-order-cap")));

        OrderRequest small = goodRequest();
        small.amount = 20.0;
        small.stopLossAmount = 5.0;
        small.takeProfitAmount = 10.0;
        QVERIFY(validateGood(small, c).codes().contains(QStringLiteral("below-min-stake")));

        // The daily cap counts what today already committed — a per-order cap alone
        // lets twenty orders through.
        c.maxStakePerOrder = Money::fromDouble(1000.0, Currency::Usd);
        c.maxStakePerDay = Money::fromDouble(1200.0, Currency::Usd);
        c.committedToday = Money::fromDouble(800.0, Currency::Usd);
        QVERIFY(validateGood(goodRequest(), c).codes().contains(QStringLiteral("over-day-cap")));
        c.committedToday = Money::fromDouble(600.0, Currency::Usd);
        QVERIFY(validateGood(goodRequest(), c).ok());
        // No cap set = no bound, rather than a bound of zero.
        const OrderContext unbounded = goodContext();
        QVERIFY(validateGood(goodRequest(), unbounded).ok());

        // An INVALID amount — what a mixed-currency addition upstream produces — is
        // refused, and the stake's own currency must be the account's.
        const OrderRequest r = goodRequest();
        const Money zeroUsd = Money::zero(Currency::Usd);
        QVERIFY(validateOrderRequest(r, OrderAmounts{Money(), zeroUsd, zeroUsd}, goodContext())
                    .codes()
                    .contains(QStringLiteral("stake-invalid")));
        QVERIFY(validateOrderRequest(r,
                                     OrderAmounts{Money::fromDouble(500.0, Currency::Eur),
                                                  Money::zero(Currency::Eur),
                                                  Money::zero(Currency::Eur)},
                                     goodContext())
                    .codes()
                    .contains(QStringLiteral("stake-currency")));
        QVERIFY(validateOrderRequest(r,
                                     OrderAmounts{Money::fromDouble(-500.0, Currency::Usd),
                                                  zeroUsd, zeroUsd},
                                     goodContext())
                    .codes()
                    .contains(QStringLiteral("stake-not-positive")));
        QVERIFY(validateOrderRequest(r,
                                     OrderAmounts{Money::fromDouble(500.0, Currency::Usd),
                                                  Money::fromDouble(-1.0, Currency::Usd), zeroUsd},
                                     goodContext())
                    .codes()
                    .contains(QStringLiteral("stop-loss-negative")));

        // A request with several faults reports several: fixing them one round-trip at
        // a time is how a validator becomes a nuisance instead of a safeguard.
        OrderRequest bad = goodRequest();
        bad.leverage = 8.0;
        bad.instrumentId = 99;
        bad.triggerRate = 5100.0;
        QVERIFY(validateGood(bad, goodContext()).problems.size() >= 3);
        QVERIFY(!validateGood(bad, goodContext()).summary().isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestMoney)
#include "tst_money.moc"
