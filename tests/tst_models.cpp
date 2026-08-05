#include "domain/Models.h"

#include <QTest>
#include <QVariant>

#include <cmath>

// The shared data types every layer speaks in. Small as they are, two properties
// of theirs are load-bearing and easy to break silently: the equality that
// decides whether a poll changed anything (a wrong one repaints the world on
// every tick, or misses a fill), and their registration as Qt metatypes, which
// is what lets them cross a queued signal at all.

class TestModels : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-MODEL-001 @design DES-DOM-MODELS
    // @relation(REQ-F-026, scope=function)
    void TS_MODEL_001_pendingOrderEqualityComparesEveryFieldThatCanChange()
    {
        PendingOrder a;
        a.orderId = QStringLiteral("o-1");
        a.instrumentId = 27;
        a.symbol = QStringLiteral("SPX500");
        a.isBuy = true;
        a.triggerRate = 5000.0;
        a.amount = 250.0;
        a.leverage = 5.0;
        a.stopLossAmount = 50.0;
        a.takeProfitAmount = 75.0;
        a.trailingStop = false;
        a.status = QStringLiteral("Waiting for market");
        a.submitted = QDateTime(QDate(2026, 8, 5), QTime(9, 30), QTimeZone::UTC);
        QCOMPARE(a, a);

        // Every field a broker poll can bring back changed has to break equality —
        // otherwise the UI would keep showing a stale resting order as current.
        const auto differsIn = [a](auto mutate) {
            PendingOrder b = a;
            mutate(b);
            return !(a == b);
        };
        QVERIFY(differsIn([](PendingOrder &o) { o.orderId = QStringLiteral("o-2"); }));
        QVERIFY(differsIn([](PendingOrder &o) { o.instrumentId = 1; }));
        QVERIFY(differsIn([](PendingOrder &o) { o.symbol = QStringLiteral("GER40"); }));
        QVERIFY(differsIn([](PendingOrder &o) { o.isBuy = false; }));
        QVERIFY(differsIn([](PendingOrder &o) { o.triggerRate = 5001.0; }));
        QVERIFY(differsIn([](PendingOrder &o) { o.amount = 251.0; }));
        QVERIFY(differsIn([](PendingOrder &o) { o.leverage = 10.0; }));
        QVERIFY(differsIn([](PendingOrder &o) { o.stopLossAmount = 60.0; }));
        QVERIFY(differsIn([](PendingOrder &o) { o.takeProfitAmount = 80.0; }));
        QVERIFY(differsIn([](PendingOrder &o) { o.trailingStop = true; }));
        QVERIFY(differsIn([](PendingOrder &o) { o.status = QStringLiteral("Executed"); }));
        QVERIFY(differsIn([](PendingOrder &o) { o.submitted = o.submitted.addSecs(1); }));
    }

    //! @tstid TS-MODEL-002 @design DES-DOM-MODELS
    // @relation(REQ-F-026, scope=function)
    void TS_MODEL_002_everySharedTypeSurvivesAQVariantRoundTrip()
    {
        // These types cross queued signals between the network layer and the GUI
        // thread, which QVariant-wraps them. A type that lost its metatype
        // declaration would fail there at runtime, in a slot that simply never
        // fires — so the declaration is worth a test of its own.
        const auto roundTrip = [](auto value) -> bool {
            const QVariant packed = QVariant::fromValue(value);
            if (!packed.isValid() || !packed.canConvert<decltype(value)>()) {
                return false;
            }
            static_cast<void>(packed.value<decltype(value)>());
            return true;
        };
        QVERIFY(roundTrip(Instrument{}));
        QVERIFY(roundTrip(ClosedTrade{}));
        QVERIFY(roundTrip(Candle{}));
        QVERIFY(roundTrip(ScreenerRow{}));
        QVERIFY(roundTrip(Position{}));
        QVERIFY(roundTrip(OrderRequest{}));
        QVERIFY(roundTrip(PendingOrder{}));
        QVERIFY(roundTrip(InstrumentPnl{}));
        QVERIFY(roundTrip(MonthlyPnl{}));
        QVERIFY(roundTrip(NewsHeadline{}));
        QVERIFY(roundTrip(WebRating{}));
        QVERIFY(roundTrip(AiDecision{}));

        // A resting order also travels as a LIST across those signals, and its fields
        // survive the trip — the member-by-member copy is what the equality above
        // compares, so both need to be exercised.
        PendingOrder order;
        order.orderId = QStringLiteral("o-9");
        order.instrumentId = 27;
        order.symbol = QStringLiteral("SPX500");
        order.isBuy = false;
        order.triggerRate = 5100.0;
        order.amount = 750.0;
        order.leverage = 10.0;
        order.stopLossAmount = 90.0;
        order.takeProfitAmount = 180.0;
        order.trailingStop = true;
        order.status = QStringLiteral("Waiting for market");
        order.submitted = QDateTime(QDate(2026, 8, 5), QTime(9, 0), QTimeZone::UTC);
        const QVariant packedList = QVariant::fromValue(QList<PendingOrder>{order});
        QVERIFY(packedList.canConvert<QList<PendingOrder>>());
        const auto back = packedList.value<QList<PendingOrder>>();
        QCOMPARE(back.size(), 1);
        QCOMPARE(back.constFirst(), order);
        QCOMPARE(back.constFirst().symbol, QStringLiteral("SPX500"));
        QCOMPARE(back.constFirst().status, QStringLiteral("Waiting for market"));
        QVERIFY(back.constFirst().trailingStop);
    }
};

QTEST_GUILESS_MAIN(TestModels)
#include "tst_models.moc"
