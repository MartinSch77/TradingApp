#include "services/OrderGateway.h"

#include <QDir>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace trading;

namespace {

Money usd(double major)
{
    return Money::fromDouble(major, Currency::Usd);
}

OrderRequest goodRequest()
{
    OrderRequest r;
    r.isBuy = true;
    r.instrumentId = 27;
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
    c.leverageLadder = {1, 2, 5, 10, 20};
    c.marketRate = 5000.0;
    return c;
}

QDateTime at(qint32 hour, qint32 minute = 0)
{
    return {QDate(2026, 8, 5), QTime(hour, minute), QTimeZone::UTC};
}

// The amounts the guarded send is given, as one value.
OrderAmounts amounts(double stake, double stop, double target)
{
    return OrderAmounts{usd(stake), usd(stop), usd(target)};
}

} // namespace

class TestOrderGateway : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-ARM-001 @design DES-SVC-GATEWAY
    // @relation(REQ-N-009, scope=function)
    void TS_ARM_001_armingIsDeliberateBoundedAndCapped()
    {
        LiveArm arm;
        const QSignalSpy changed(&arm, &LiveArm::stateChanged);

        // Nothing is armed until someone arms it — the default is the safe one.
        QVERIFY(!arm.isArmed(at(9)));
        QCOMPARE(arm.check(usd(10.0), at(9)), ArmRefusal::NotArmed);
        QVERIFY(arm.stateLine(at(9)).contains(QStringLiteral("not armed")));

        QVERIFY(arm.arm(30, usd(1000.0), usd(2000.0), at(9)));
        QVERIFY(arm.isArmed(at(9, 29)));
        QCOMPARE(arm.check(usd(500.0), at(9, 29)), ArmRefusal::None);
        QVERIFY(changed.count() > 0);

        // It EXPIRES on its own: an armed session must not outlive the attention that
        // granted it.
        QVERIFY(!arm.isArmed(at(9, 31)));
        QCOMPARE(arm.check(usd(500.0), at(9, 31)), ArmRefusal::Expired);
        // …and "expired" is reported as itself, not as "not armed": the two mean
        // different things to whoever reads the refusal.
        QVERIFY(armRefusalCode(ArmRefusal::Expired) != armRefusalCode(ArmRefusal::NotArmed));

        // The caps the grant carried are enforced, per order and per day.
        QCOMPARE(arm.check(usd(1000.01), at(9, 10)), ArmRefusal::OverOrderCap);
        QCOMPARE(arm.check(usd(1000.00), at(9, 10)), ArmRefusal::None);
        arm.recordCommitted(usd(1500.0), at(9, 10));
        QCOMPARE(arm.check(usd(600.0), at(9, 15)), ArmRefusal::OverDayCap);
        QCOMPARE(arm.check(usd(500.0), at(9, 15)), ArmRefusal::None);
        // The day rolls over on its own — a daily cap that only resets on restart is
        // not a daily cap.
        QCOMPARE(arm.committedToday(), usd(1500.0));
        arm.recordCommitted(usd(100.0), at(9, 10).addDays(1));
        QCOMPARE(arm.committedToday(), usd(100.0));

        // An ordinary disarm closes the window without tripping anything.
        arm.disarm();
        QVERIFY(!arm.isArmed(at(9, 10)));
        QVERIFY(!arm.isTripped());
        QVERIFY(arm.arm(10, usd(100.0), usd(100.0), at(9, 10)));
    }

    //! @tstid TS-ARM-002 @design DES-SVC-GATEWAY
    // @relation(REQ-N-009, scope=function)
    void TS_ARM_002_theKillSwitchIsStickyAndBeatsEverything()
    {
        LiveArm arm;
        const QSignalSpy tripped(&arm, &LiveArm::trippedChanged);
        QVERIFY(arm.arm(60, usd(1000.0), usd(5000.0), at(9)));

        arm.trip(QStringLiteral("the numbers looked wrong"));
        QVERIFY(arm.isTripped());
        QCOMPARE(tripped.count(), 1);
        QVERIFY(!arm.isArmed(at(9, 1)));
        // Tripped beats every other reason, including the caps: the reason reported is
        // the most fundamental one.
        QCOMPARE(arm.check(usd(1.0), at(9, 1)), ArmRefusal::Tripped);
        QCOMPARE(arm.check(usd(999999.0), at(9, 1)), ArmRefusal::Tripped);
        QVERIFY(arm.stateLine(at(9, 1)).contains(QStringLiteral("KILL SWITCH")));
        QVERIFY(arm.stateLine(at(9, 1)).contains(QStringLiteral("the numbers looked wrong")));

        // STICKY: re-arming is refused while tripped, so a panic action cannot be undone
        // by the next click or timer tick.
        QVERIFY(!arm.arm(60, usd(1000.0), usd(5000.0), at(9, 2)));
        QVERIFY(!arm.isArmed(at(9, 2)));
        QCOMPARE(arm.check(usd(1.0), at(9, 2)), ArmRefusal::Tripped);

        // Only an explicit clear re-enables arming, and that is its own action.
        arm.clearTrip();
        QVERIFY(!arm.isTripped());
        QCOMPARE(tripped.count(), 2);
        // Clearing does NOT re-arm: it only removes the block.
        QVERIFY(!arm.isArmed(at(9, 3)));
        QCOMPARE(arm.check(usd(1.0), at(9, 3)), ArmRefusal::NotArmed);
        QVERIFY(arm.arm(60, usd(1000.0), usd(5000.0), at(9, 3)));
        QCOMPARE(arm.check(usd(1.0), at(9, 4)), ArmRefusal::None);
    }

    //! @tstid TS-GATE-001 @design DES-SVC-GATEWAY
    // @relation(REQ-N-009, scope=function)
    void TS_GATE_001_theGuardedSendCannotBeBypassedAndAlwaysRecords()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        OrderAudit audit(dir.filePath(QStringLiteral("audit.jsonl")));
        FakeOrderGateway gateway;
        LiveArm arm;
        // Composed once — a caller that holds a sender cannot reach past it to the
        // gateway, which is what makes the three guarantees true of EVERY order.
        GuardedOrderSender sender(&gateway, &arm, &audit);

        // 1. An UNARMED send is refused and recorded — and nothing reaches the gateway.
        GuardedSendOutcome out = sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(),
                            at(9), at(9));
        QVERIFY(!out.sent);
        QCOMPARE(out.armRefusal, ArmRefusal::NotArmed);
        QVERIFY(gateway.sent().isEmpty());
        QCOMPARE(out.audited.outcome, QStringLiteral("refused-arm"));
        QVERIFY(!out.refusalText().isEmpty());

        // 2. An INVALID request is refused BEFORE the armed state is even consulted, so
        // arming can never launder a malformed order.
        QVERIFY(arm.arm(30, usd(1000.0), usd(1200.0), at(9)));
        OrderRequest bad = goodRequest();
        bad.leverage = 8.0;   // not on the ladder
        out = sender.send(bad, amounts(500.0, 100.0, 150.0), goodContext(),
                            at(9), at(9));
        QVERIFY(!out.sent);
        QVERIFY(out.validation.codes().contains(QStringLiteral("leverage-not-offered")));
        QCOMPARE(out.armRefusal, ArmRefusal::None);   // never reached
        QVERIFY(gateway.sent().isEmpty());
        QCOMPARE(out.audited.outcome, QStringLiteral("refused-validation"));

        // 3. A correct, armed order goes out — with EXACTLY the stake it was validated
        // with, and the day's commitment is recorded against the cap.
        out = sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(),
                            at(9), at(9, 1));
        QVERIFY2(out.sent, qPrintable(out.refusalText()));
        QCOMPARE(gateway.sent().size(), 1);
        QCOMPARE(gateway.sent().constFirst().second, usd(500.0));
        QCOMPARE(arm.committedToday(), usd(500.0));
        QCOMPARE(out.audited.outcome, QStringLiteral("sent"));
        QVERIFY(out.audited.sentAt.isValid());

        // 4. The armed session's DAILY cap now bites, through the validator's own cap
        // check as well as the arm check — the two use the same numbers by construction.
        out = sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(),
                            at(9), at(9, 2));
        QVERIFY(out.sent);
        out = sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(),
                            at(9), at(9, 3));
        QVERIFY(!out.sent);
        QVERIFY(out.validation.codes().contains(QStringLiteral("over-day-cap"))
                || (out.armRefusal == ArmRefusal::OverDayCap));

        // 5. A broker REJECTION is recorded with the broker's own words, and does not
        // count against the daily cap (nothing was committed).
        arm.disarm();
        QVERIFY(arm.arm(30, usd(1000.0), Money(), at(10)));
        gateway.setNextResult(OrderResult{false, QStringLiteral("req-7"),
                                         QStringLiteral("insufficient funds"), {}});
        // Two 500 sends have been accepted so far, and re-arming does not reset the
        // day's total. Asserting the NUMBER rather than a captured "before" keeps the
        // check meaningful: a rejected order must leave it at exactly that.
        QCOMPARE(arm.committedToday(), usd(1000.0));
        out = sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(),
                            at(10), at(10));
        QVERIFY(!out.sent);
        QCOMPARE(out.audited.outcome, QStringLiteral("rejected"));
        QCOMPARE(out.audited.detail, QStringLiteral("insufficient funds"));
        QCOMPARE(out.refusalText(), QStringLiteral("insufficient funds"));
        QCOMPARE(arm.committedToday(), usd(1000.0));

        // 6. A sender missing a collaborator fails CLOSED rather than sending unguarded.
        GuardedOrderSender halfComposed(nullptr, &arm, &audit);
        QVERIFY(!halfComposed.isUsable());
        const GuardedSendOutcome nothing =
            halfComposed.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(), at(10),
                              at(10));
        QVERIFY(!nothing.sent);
        QVERIFY(nothing.validation.codes().contains(QStringLiteral("no-gateway")));

        // 7. EVERY attempt is in the record — refused, sent and rejected alike, because
        // the ones worth investigating are exactly the ones that did not work.
        const QList<OrderAuditEntry> all = audit.readAll();
        QCOMPARE(all.size(), 6);
        QStringList outcomes;
        for (const OrderAuditEntry &entry : all) {
            outcomes.append(entry.outcome);
        }
        QCOMPARE(outcomes.count(QStringLiteral("refused-arm")), 1);
        QCOMPARE(outcomes.count(QStringLiteral("sent")), 2);
        QCOMPARE(outcomes.count(QStringLiteral("rejected")), 1);
        // TWO validation refusals: the leverage, and the day cap. The day cap is
        // reported by the VALIDATOR rather than by the arm check because guardedSend
        // copies the armed session's caps into the context the validator sees — which is
        // the point of applyCapsTo. The arm check remains the backstop for a caller that
        // built its own context.
        QCOMPARE(outcomes.count(QStringLiteral("refused-validation")), 2);
    }

    //! @tstid TS-GATE-002 @design DES-SVC-GATEWAY
    // @relation(REQ-N-009, scope=function)
    void TS_GATE_002_theAuditRecordSurvivesAndFingerprintsTheOrder()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("audit.jsonl"));

        OrderAuditEntry entry;
        entry.decidedAt = at(9);
        entry.sentAt = at(9, 1);
        entry.requestId = QStringLiteral("abc-123");
        entry.outcome = QStringLiteral("sent");
        entry.detail = QStringLiteral("accepted");
        entry.instrumentId = 27;
        entry.symbol = QStringLiteral("SPX500");
        entry.isBuy = false;
        entry.stake = usd(250.55);
        entry.leverage = 10.0;
        entry.stopLoss = usd(50.10);
        entry.takeProfit = usd(75.25);
        entry.triggerRate = 4999.5;
        entry.orderCurrency = Currency::Usd;
        entry.accountCurrency = Currency::Usd;

        {
            OrderAudit audit(path);
            QVERIFY(audit.append(entry));
        }
        // A NEW instance reads it back: the record's whole purpose is to be there after
        // the process that wrote it is gone.
        const OrderAudit reopened(path);
        const QList<OrderAuditEntry> read = reopened.readAll();
        QCOMPARE(read.size(), 1);
        const OrderAuditEntry &back = read.constFirst();
        QCOMPARE(back.requestId, entry.requestId);
        QCOMPARE(back.symbol, entry.symbol);
        QCOMPARE(back.isBuy, false);
        // Money round-trips as MINOR UNITS: the record must not re-introduce the
        // floating-point ambiguity the type removed.
        QCOMPARE(back.stake.minorUnits(), 25055);
        QCOMPARE(back.stake.currency(), Currency::Usd);
        QCOMPARE(back.stopLoss.minorUnits(), 5010);
        QCOMPARE(back.takeProfit.minorUnits(), 7525);
        QCOMPARE(back.decidedAt.toUTC(), at(9));
        QCOMPARE(back.sentAt.toUTC(), at(9, 1));

        // The fingerprint identifies the ORDER, so a retry of the same order matches
        // while a different amount does not…
        QCOMPARE(back.fingerprint(), entry.fingerprint());
        OrderAuditEntry retry = entry;
        retry.decidedAt = at(11);
        retry.sentAt = at(11, 1);
        retry.requestId = QStringLiteral("def-456");
        QCOMPARE(retry.fingerprint(), entry.fingerprint());
        OrderAuditEntry other = entry;
        other.stake = usd(250.56);
        QVERIFY(other.fingerprint() != entry.fingerprint());

        // …which is what answers "did we already send this?" after an answer went
        // missing.
        {
            OrderAudit audit(path);
            QVERIFY(audit.append(retry));
            QVERIFY(audit.append(other));
        }
        QCOMPARE(reopened.withFingerprint(entry.fingerprint()).size(), 2);
        QCOMPARE(reopened.withFingerprint(other.fingerprint()).size(), 1);
        QCOMPARE(reopened.readAll().size(), 3);

        // A truncated last line — the crash case — must not hide the entries before it.
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        QVERIFY(file.write(QByteArrayLiteral("{\"outcome\":\"se")) > 0);
        file.close();
        QCOMPARE(reopened.readAll().size(), 3);

        // An unwritable path fails honestly rather than pretending to have recorded.
        OrderAudit broken(dir.filePath(QStringLiteral("no/such/dir/audit.jsonl")));
        QVERIFY(!broken.append(entry));
        QVERIFY(broken.readAll().isEmpty());
    }
    //! @tstid TS-GATE-003 @design DES-SVC-GATEWAY
    // @relation(REQ-N-009, scope=function)
    void TS_GATE_003_everyRefusalIsSpelledOutAndEveryGuardHolds()
    {
        // A refusal a user cannot read is as useless as one a machine cannot count, so
        // every ArmRefusal has both a code and a sentence — and they are distinct.
        const QList<ArmRefusal> all{ArmRefusal::None,        ArmRefusal::NotArmed,
                                    ArmRefusal::Expired,     ArmRefusal::Tripped,
                                    ArmRefusal::OverOrderCap, ArmRefusal::OverDayCap};
        QSet<QString> codes;
        for (const ArmRefusal refusal : all) {
            const QString code = armRefusalCode(refusal);
            QVERIFY2(!code.isEmpty(), qPrintable(code));
            QVERIFY2(code != QStringLiteral("unknown"), qPrintable(code));
            codes.insert(code);
        }
        QCOMPARE(codes.size(), all.size());

        // …and the same for the sentences the window shows. Each is reached by putting
        // the outcome into exactly that state.
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        OrderAudit audit(dir.filePath(QStringLiteral("audit.jsonl")));
        FakeOrderGateway gateway;
        LiveArm arm;
        GuardedOrderSender sender(&gateway, &arm, &audit);

        // not armed
        QVERIFY(sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(), at(9),
                            at(9))
                    .refusalText()
                    .contains(QStringLiteral("not armed")));
        // expired — the window closed while nobody was looking
        QVERIFY(arm.arm(10, usd(1000.0), usd(5000.0), at(9)));
        QVERIFY(sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(), at(9),
                            at(9, 11))
                    .refusalText()
                    .contains(QStringLiteral("expired")));
        // over the per-order cap
        QVERIFY(arm.arm(60, usd(100.0), usd(5000.0), at(9)));
        QVERIFY(sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(), at(9),
                            at(9, 1))
                    .refusalText()
                    .contains(QStringLiteral("per-order cap")));
        // over the daily cap, checked by the ARM rather than by the validator: the
        // caller passed its own context, so applyCapsTo is what carries the numbers.
        QVERIFY(arm.arm(60, usd(5000.0), usd(600.0), at(9)));
        arm.recordCommitted(usd(400.0), at(9));
        QVERIFY(sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(), at(9),
                            at(9, 1))
                    .refusalText()
                    .contains(QStringLiteral("daily cap")));
        // the kill switch. Re-armed with generous caps first, because the validator
        // runs BEFORE the arm check and a day-cap breach would otherwise be the reason
        // reported — which is correct behaviour and exactly why this has to be set up
        // deliberately to see the kill switch's own sentence.
        QVERIFY(arm.arm(60, usd(5000.0), usd(50000.0), at(9)));
        arm.trip(QStringLiteral("hands off"));
        QVERIFY(sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(), at(9),
                            at(9, 1))
                    .refusalText()
                    .contains(QStringLiteral("kill switch")));
        arm.clearTrip();
        // clearTrip on an untripped arm is a no-op rather than an event
        arm.clearTrip();
        QVERIFY(!arm.isTripped());

        // A validation refusal speaks in the validator's own words…
        QVERIFY(arm.arm(60, usd(5000.0), usd(50000.0), at(10)));
        OrderRequest bad = goodRequest();
        bad.leverage = 8.0;
        QVERIFY(sender.send(bad, amounts(500.0, 100.0, 150.0), goodContext(), at(10), at(10))
                    .refusalText()
                    .contains(QStringLiteral("leverage-not-offered")));
        // …and a broker rejection WITHOUT a reason still says something useful.
        gateway.setNextResult(OrderResult{false, QStringLiteral("r"), QString(), {}});
        QVERIFY(sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(), at(10),
                            at(10))
                    .refusalText()
                    .contains(QStringLiteral("did not accept")));
        // A successful send has no refusal text at all.
        gateway.setNextResult(OrderResult{true, QStringLiteral("ok"), QString(), {}});
        const GuardedSendOutcome sent =
            sender.send(goodRequest(), amounts(500.0, 100.0, 150.0), goodContext(), at(10),
                        at(10));
        QVERIFY(sent.sent);
        QVERIFY(sent.refusalText().isEmpty());
        // The request may leave the instrument at 0 and let the context name it — the
        // audit entry must still carry the id somebody would search for.
        OrderRequest fromContext = goodRequest();
        fromContext.instrumentId = 0;
        const GuardedSendOutcome viaContext =
            sender.send(fromContext, amounts(500.0, 100.0, 150.0), goodContext(), at(10),
                        at(10));
        QCOMPARE(viaContext.audited.instrumentId, 27);
    }

    //! @tstid TS-ARM-003 @design DES-SVC-GATEWAY
    // @relation(REQ-N-009, scope=function)
    void TS_ARM_003_theCapsAndTheDayLedgerHandleTheirEdges()
    {
        LiveArm arm;
        // Arming for no time at all is refused: a zero-length window is not a grant.
        QVERIFY(!arm.arm(0, usd(100.0), usd(100.0), at(9)));
        QVERIFY(!arm.arm(-5, usd(100.0), usd(100.0), at(9)));
        QVERIFY(!arm.isArmed(at(9)));

        // A grant with NO caps at all is legal (the caller relies on the validator's
        // own limits), and then neither cap can refuse.
        QVERIFY(arm.arm(30, Money(), Money(), at(9)));
        QCOMPARE(arm.check(usd(999999.0), at(9, 1)), ArmRefusal::None);
        QVERIFY(arm.stateLine(at(9, 1)).contains(QStringLiteral("no cap")));
        // Nothing has been committed, so the state line says so in words.
        QVERIFY(arm.stateLine(at(9, 1)).contains(QStringLiteral("nothing")));

        // Committing in one currency and asking about another cannot pass a cap check
        // by accident: the sum is invalid, which is a refusal.
        QVERIFY(arm.arm(30, Money(), usd(1000.0), at(9)));
        arm.recordCommitted(usd(200.0), at(9));
        QCOMPARE(arm.check(Money::fromDouble(100.0, Currency::Eur), at(9, 1)),
                 ArmRefusal::OverDayCap);
        QCOMPARE(arm.check(usd(900.0), at(9, 1)), ArmRefusal::OverDayCap);
        // Yesterday's total does not count against today's cap — for a session that is
        // still open across midnight, which is the only way both can be true at once
        // (a window that has expired refuses for that reason first, as it should).
        LiveArm overnight;
        const QDateTime lateEvening = at(23, 0);
        QVERIFY(overnight.arm(180, Money(), usd(1000.0), lateEvening));
        overnight.recordCommitted(usd(900.0), lateEvening);
        QCOMPARE(overnight.check(usd(500.0), at(23, 30)), ArmRefusal::OverDayCap);
        QCOMPARE(overnight.check(usd(500.0), at(23, 30).addSecs(3600)), ArmRefusal::None);
        // Recording an INVALID stake leaves the ledger unusable rather than pretending
        // the order was free.
        arm.recordCommitted(Money(), at(9));
        QVERIFY(!arm.committedToday().isValid());

        // applyCapsTo hands the validator exactly what the grant carried.
        QVERIFY(arm.arm(30, usd(250.0), usd(750.0), at(11)));
        arm.recordCommitted(usd(100.0), at(11));
        OrderContext ctx;
        arm.applyCapsTo(ctx);
        QCOMPARE(ctx.maxStakePerOrder, usd(250.0));
        QCOMPARE(ctx.maxStakePerDay, usd(750.0));
        QCOMPARE(ctx.committedToday, usd(100.0));
    }

    //! @tstid TS-GATE-004 @design DES-SVC-GATEWAY
    // @relation(REQ-N-009, scope=function)
    void TS_GATE_004_theAuditFileToleratesWhatRealFilesContain()
    {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("audit.jsonl"));
        OrderAudit audit(path);
        OrderAuditEntry entry;
        entry.decidedAt = at(9);
        entry.outcome = QStringLiteral("sent");
        entry.stake = usd(10.0);
        entry.stopLoss = usd(1.0);
        entry.takeProfit = usd(2.0);
        entry.orderCurrency = Currency::Usd;
        entry.accountCurrency = Currency::Usd;
        QVERIFY(audit.append(entry));

        // Blank lines between entries — what an editor or a partial flush leaves —
        // are skipped rather than counted as records.
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append | QIODevice::Text));
        QVERIFY(file.write(QByteArrayLiteral("\n   \n")) > 0);
        file.close();
        QCOMPARE(audit.readAll().size(), 1);

        // An entry whose amounts were never set round-trips as INVALID amounts rather
        // than as zeros: "no stop" and "a stop of nothing" are different claims.
        OrderAuditEntry empty;
        empty.outcome = QStringLiteral("refused-validation");
        QVERIFY(audit.append(empty));
        const QList<OrderAuditEntry> back = audit.readAll();
        QCOMPARE(back.size(), 2);
        QVERIFY(!back.constLast().stake.isValid());
        QVERIFY(!back.constLast().decidedAt.isValid());
        QVERIFY(!back.constLast().sentAt.isValid());
        QCOMPARE(back.constLast().orderCurrency, Currency::Invalid);

        // The default path is under the app's config directory — used when no path is
        // given, which is how the app itself constructs it.
        const OrderAudit dflt;
        QVERIFY(dflt.path().endsWith(QStringLiteral("order-audit.jsonl")));
    }
};

QTEST_MAIN(TestOrderGateway)
#include "tst_ordergateway.moc"
