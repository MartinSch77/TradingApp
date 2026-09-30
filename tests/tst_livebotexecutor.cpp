// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The real-money mirror's executor (DES-UI-LIVEBOT, REQ-F-076, REQ-N-005, REQ-N-009), driven
// headless: a FakeOrderGateway records exactly what would have gone to the broker, the
// SIMULATION client (no credentials, never a real account) plays the venue where a venue
// is needed — it books a position and answers a close — and the "is the client live"
// question is the constructor-injected predicate the composition root leaves at the
// client's own answer. The paper bot's record is SEEDED to clear the REQ-F-031 gate,
// because arming requires it: an empty record refuses the arming (TS-LBOT-005).

#include "ui/LiveBotExecutor.h"

#include "domain/LiveMirror.h"
#include "domain/Models.h"
#include "domain/PaperTrader.h"
#include "services/Config.h"
#include "services/EtoroClient.h"
#include "services/OrderGateway.h"
#include "ui/BotSimRunner.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest/QtTest>

#include <memory>

using namespace trading;

namespace {

constexpr auto kStore = "tst-live.json";

QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QStringList runnerFiles()
{
    const QDir dir(configDir());
    return {dir.filePath(QLatin1String(kStore)),
            dir.filePath(QLatin1String(kStore) + QStringLiteral(".lock")),
            dir.filePath(QStringLiteral("tst-live-decisions.log")),
            dir.filePath(QStringLiteral("tst-live-experience.jsonl")),
            dir.filePath(QStringLiteral("tst-live-audit.jsonl")),
            BotSimRunner::ledgerPath()};
}

void removeRunnerFiles()
{
    for (const QString &path : runnerFiles()) {
        static_cast<void>(QFile::remove(path));
    }
}

Money eur(double major)
{
    return Money::fromDouble(major, Currency::Eur);
}

Money usd(double major)
{
    return Money::fromDouble(major, Currency::Usd);
}

// A paper record that clears the REQ-F-031 gate (200 closed trades over 20 days, net
// positive, profit factor well above 1.2, a shallow drawdown): ten SPX500 trades a day,
// three winners of ~+29.5 for every loser of ~−10.5, written as the runner's store.
bool seedReadyBook()
{
    PaperBook book;
    qint32 n = 0;
    for (qint32 day = 0; day < 20; ++day) {
        for (qint32 k = 0; k < 10; ++k, ++n) {
            const QDateTime at(QDate(2026, 8, 3).addDays(day), QTime(10 + (k % 6), 0),
                               QTimeZone::UTC);
            EntrySignal sig;
            sig.valid = true;
            sig.symbol = QStringLiteral("SPX500");
            sig.instrumentId = 27;
            sig.isBuy = true;
            sig.fillRate = 5000.0;
            sig.spreadPct = 0.01;
            sig.leverage = 5;
            sig.slRate = 4950.0;
            sig.tpRate = 5075.0;
            sig.confidence = 60.0;
            const qint64 id = book.open(sig, 1000.0, at);
            if (id == 0) {
                return false;
            }
            const bool loser = (n % 4) == 3;
            const PaperClosedTrade done = book.close(
                id, loser ? 4990.0 : 5030.0, 0.01,
                loser ? CloseReason::StopLoss : CloseReason::TakeProfit, at.addSecs(1800));
            if (done.id == 0) {
                return false;
            }
        }
    }
    QFile file(runnerFiles().constFirst());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(QJsonDocument(book.toJson()).toJson()) > 0;
}

// The owner's grant: SPX500 + NSDQ100, 250 EUR per order, 250 EUR realised daily loss.
LiveMirrorConfig grant()
{
    LiveMirrorConfig cfg;
    cfg.maxPerOrder = eur(250.0);
    cfg.maxDailyLoss = eur(250.0);
    return cfg;
}

// A paper SPX500 long the bot would open on its 50 000 account: 3 000 EUR at x10 entered
// at 5 000, the stop 25 below and the target 37.5 above.
PaperTrade paperTrade(qint64 id, const QString &symbol, qint64 instrumentId, bool isBuy = true)
{
    PaperTrade t;
    t.id = id;
    t.symbol = symbol;
    t.instrumentId = instrumentId;
    t.isBuy = isBuy;
    t.stake = 3000.0;
    t.leverage = 10;
    t.openRate = 5000.0;
    t.slRate = isBuy ? 4975.0 : 5025.0;
    t.tpRate = isBuy ? 5037.5 : 4962.5;
    t.openTime = QDateTime::currentDateTimeUtc();
    return t;
}

PaperClosedTrade paperClose(qint64 id, const QString &symbol)
{
    PaperClosedTrade done;
    done.id = id;
    done.symbol = symbol;
    done.reason = CloseReason::StopLoss;
    return done;
}

// The executor's setup: the grant, the audit file in the test config dir, and the live
// check — answered here (there is no real account), or left at the client's own answer.
LiveBotSetup setup(bool clientLive)
{
    LiveBotSetup s;
    s.config = grant();
    s.auditPath = QDir(configDir()).filePath(QStringLiteral("tst-live-audit.jsonl"));
    if (clientLive) {
        s.isClientLive = [] { return true; };
    }
    return s;
}

// The whole cast, wired as the composition root wires it — except the gateway (the fake)
// and the live check (answered here, since there is no real account).
struct Fixture {
    explicit Fixture(bool clientLive = true)
        : client(Config{}), runner(&client, nullptr, nullptr, QLatin1String(kStore)),
          exec(&client, &runner, &fake, setup(clientLive)), logs(&exec, &LiveBotExecutor::log)
    {}

    [[nodiscard]] QString logText() const
    {
        QStringList lines;
        for (const QList<QVariant> &args : logs) {
            lines.append(args.at(0).toString());
        }
        return lines.join(QLatin1Char('\n'));
    }
    [[nodiscard]] QList<OrderAuditEntry> audit() const
    {
        return OrderAudit(exec.auditPath()).readAll();
    }

    EtoroClient client;
    BotSimRunner runner;
    FakeOrderGateway fake;
    LiveBotExecutor exec;
    QSignalSpy logs;
};

}   // namespace

class TestLiveBotExecutor : public QObject
{
    Q_OBJECT;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(QDir().mkpath(configDir()));
    }

    void init()
    {
        removeRunnerFiles();
        QVERIFY(seedReadyBook());
    }

    void cleanup() { removeRunnerFiles(); }

    //! @tstid TS-LBOT-001 @design DES-UI-LIVEBOT
    // @relation(REQ-F-076, REQ-N-005, REQ-N-009, scope=function)
    //
    // Not armed: an in-scope paper open reaches the guarded send, which refuses it as
    // not-armed and RECORDS that; nothing reaches the gateway. Out-of-scope opens while
    // disarmed are not even attempted.
    void TS_LBOT_001_notArmedMeansRefusedAndRecordedNeverSent()
    {
        Fixture f;
        QVERIFY(f.runner.ownsBook());
        QVERIFY2(f.runner.liveReadiness().ready,
                 qPrintable(f.runner.liveReadiness().blockers.join(u"; ")));
        f.exec.onFx(0.9);
        QVERIFY(!f.exec.isArmed());
        QVERIFY(f.exec.stateLine().contains(QStringLiteral("not armed")));

        f.exec.onPaperOpened(paperTrade(1, QStringLiteral("SPX500"), 27));
        QVERIFY(f.fake.sent().isEmpty());
        const QList<OrderAuditEntry> audit = f.audit();
        QCOMPARE(audit.size(), 1);
        QCOMPARE(audit.constFirst().outcome, QStringLiteral("refused-arm"));
        QCOMPARE(audit.constFirst().detail, QStringLiteral("not-armed"));
        QCOMPARE(audit.constFirst().symbol, QStringLiteral("SPX500"));
        QCOMPARE(audit.constFirst().stake, usd(277.78));
        QVERIFY(f.logText().contains(QStringLiteral("LIVE REFUSED not-armed")));
        QVERIFY(f.exec.mirrored().isEmpty());

        f.exec.onPaperOpened(paperTrade(2, QStringLiteral("BTC"), 0));
        QCOMPARE(f.audit().size(), 1);   // out of scope while disarmed: not an attempt
        QVERIFY(f.fake.sent().isEmpty());
    }

    //! @tstid TS-LBOT-002 @design DES-UI-LIVEBOT
    // @relation(REQ-F-076, REQ-N-009, scope=function)
    //
    // Armed: exactly the capped, scaled order goes to the gateway and is recorded as
    // sent; out-of-scope and over-the-cap opens send nothing and say why.
    void TS_LBOT_002_armedSendsTheCappedMirrorOnceAndRefusesTheRest()
    {
        Fixture f;
        f.exec.onFx(0.9);
        QVERIFY2(f.exec.arm(), qPrintable(f.logText()));
        QVERIFY(f.exec.isArmed());
        QVERIFY(f.logText().contains(QStringLiteral("LIVE ARMED")));
        QVERIFY(f.exec.stateLine().contains(QStringLiteral("ARMED")));

        f.exec.onPaperOpened(paperTrade(41, QStringLiteral("SPX500"), 27));
        QCOMPARE(f.fake.sent().size(), 1);
        const OrderRequest &sent = f.fake.sent().constFirst().first;
        QCOMPARE(f.fake.sent().constFirst().second, usd(277.78));   // 250 EUR at 0.9, once
        QCOMPARE(sent.instrumentId, qint64(27));
        QVERIFY(sent.isBuy);
        QCOMPARE(sent.amount, 277.78);
        QCOMPARE(sent.leverage, 10.0);
        QCOMPARE(sent.stopLossAmount, 13.89);   // 150 × 277.78/3000
        QCOMPARE(sent.takeProfitAmount, 20.83);   // 225 × 277.78/3000
        QVERIFY(!sent.isLimit());
        QList<OrderAuditEntry> audit = f.audit();
        QCOMPARE(audit.size(), 1);
        QCOMPARE(audit.constLast().outcome, QStringLiteral("sent"));
        QCOMPARE(audit.constLast().requestId, QStringLiteral("fake-request-id"));
        QCOMPARE(f.exec.mirrored().size(), 1);
        QCOMPARE(f.exec.mirrored().constFirst().paperId, qint64(41));
        QVERIFY(f.exec.mirrored().constFirst().positionId.isEmpty());
        QCOMPARE(f.exec.mirrored().constFirst().state, QStringLiteral("confirming…"));
        QVERIFY(f.logText().contains(QStringLiteral("LIVE SENT BUY SPX500 277.78 USD at x10")));

        // Out of scope: BTC (id-less, as the paper bot opens it) and GOLD — nothing sent,
        // nothing audited (there was no order to record), the refusal logged with its code.
        f.exec.onPaperOpened(paperTrade(42, QStringLiteral("BTC"), 0));
        f.exec.onPaperOpened(paperTrade(43, QStringLiteral("GOLD"), 5));
        QCOMPARE(f.fake.sent().size(), 1);
        QCOMPARE(f.audit().size(), 1);
        QVERIFY(f.logText().contains(QStringLiteral("LIVE REFUSED live-scope: BTC")));
        QVERIFY(f.logText().contains(QStringLiteral("LIVE REFUSED live-scope: GOLD")));

        // One live position per instrument: the second SPX500 open is refused.
        f.exec.onPaperOpened(paperTrade(44, QStringLiteral("SPX500"), 27));
        QCOMPARE(f.fake.sent().size(), 1);
        QVERIFY(f.logText().contains(QStringLiteral("LIVE REFUSED live-position-cap")));
        // …while NSDQ100 is its own instrument, and a SHORT keeps its side.
        f.exec.onPaperOpened(paperTrade(45, QStringLiteral("NSDQ100"), 100000, false));
        QCOMPARE(f.fake.sent().size(), 2);
        QVERIFY(!f.fake.sent().constLast().first.isBuy);
        QCOMPARE(f.fake.sent().constLast().first.instrumentId, qint64(100000));
        QCOMPARE(f.exec.mirrored().size(), 2);
        QVERIFY(f.exec.stateLine().contains(QStringLiteral("2 mirrored (2 open)")));

        // Disarming stops new orders but the mirrored positions stay listed: with both
        // instruments still held the cap refuses first (silently while disarmed); once
        // SPX500's live position is gone, a paper open is an ATTEMPT again, recorded as
        // refused-arm.
        f.exec.disarm();
        QVERIFY(!f.exec.isArmed());
        f.exec.onPaperOpened(paperTrade(46, QStringLiteral("SPX500"), 27));
        QCOMPARE(f.fake.sent().size(), 2);
        QCOMPARE(f.exec.mirrored().size(), 2);
        QCOMPARE(f.audit().size(), 2);
        f.exec.onPositionOpened(QStringLiteral("501"), 27, true);
        f.exec.onPositionClosed(true, QStringLiteral("closed"), QStringLiteral("501"));
        f.exec.onPaperOpened(paperTrade(47, QStringLiteral("SPX500"), 27));
        QCOMPARE(f.fake.sent().size(), 2);
        QCOMPARE(f.audit().size(), 3);
        QCOMPARE(f.audit().constLast().outcome, QStringLiteral("refused-arm"));
    }

    //! @tstid TS-LBOT-003 @design DES-UI-LIVEBOT
    // @relation(REQ-F-076, scope=function)
    //
    // The fill's id maps onto the mirrored position, and the paper close closes the live
    // one — end to end through the simulation client, which books the position and answers
    // the close. A position the broker never named is flagged for a manual close, not
    // dropped.
    void TS_LBOT_003_theFillIsPairedAndThePaperCloseClosesTheLivePosition()
    {
        Fixture f;
        f.client.start();   // simulation: the venue that books and closes positions here
        QTRY_VERIFY(f.client.instrument().instrumentId != 0);
        const qint64 simId = f.client.instrument().instrumentId;
        f.exec.onFx(0.9);
        QVERIFY(f.exec.arm());
        QSignalSpy closed(&f.client, &EtoroClient::positionClosed);

        // The mirror sends (the fake records it); the venue then names the position it
        // opened — here the simulation, opening the same order on its own instrument.
        f.exec.onPaperOpened(paperTrade(7, QStringLiteral("SPX500"), simId));
        QCOMPARE(f.fake.sent().size(), 1);
        f.client.openPosition(f.fake.sent().constFirst().first);
        QCOMPARE(f.exec.mirrored().size(), 1);
        const QString positionId = f.exec.mirrored().constFirst().positionId;
        QVERIFY2(!positionId.isEmpty(), "the simulation's positionOpened never reached the mirror");
        QCOMPARE(f.exec.mirrored().constFirst().state, QStringLiteral("open"));
        QVERIFY(f.logText().contains(
            QStringLiteral("LIVE OPEN BUY SPX500 — position %1").arg(positionId)));

        // A paper close of some OTHER trade touches nothing.
        f.exec.onPaperClosed(paperClose(99, QStringLiteral("SPX500")));
        QCOMPARE(closed.count(), 0);
        // A PARTIAL paper close keeps the live position whole.
        PaperClosedTrade partial = paperClose(7, QStringLiteral("SPX500"));
        partial.partial = true;
        f.exec.onPaperClosed(partial);
        QCOMPARE(closed.count(), 0);
        QVERIFY(f.logText().contains(QStringLiteral("closed PARTIALLY")));

        // The paper close of THE mirrored trade closes the live position: the client is
        // asked by id, the venue answers, the position is realised and listed as closed.
        f.exec.onPaperClosed(paperClose(7, QStringLiteral("SPX500")));
        QCOMPARE(closed.count(), 1);
        QVERIFY(closed.constFirst().at(0).toBool());
        QCOMPARE(closed.constFirst().at(2).toString(), positionId);
        QVERIFY(f.exec.mirrored().constFirst().closed);
        QVERIFY(f.exec.mirrored().constFirst().state.startsWith(QStringLiteral("closed")));
        QVERIFY(f.logText().contains(
            QStringLiteral("LIVE CLOSE BUY SPX500 position %1").arg(positionId)));
        QVERIFY(f.logText().contains(
            QStringLiteral("LIVE CLOSED BUY SPX500 position %1").arg(positionId)));
        QVERIFY(f.exec.stateLine().contains(QStringLiteral("1 mirrored (0 open)")));

        // A mirrored position whose broker id never arrived: the paper close cannot close
        // it, so it is flagged "close by hand", kept in the list, and shouted — never
        // silently dropped, because that would read as flat while the risk is open.
        f.exec.onPaperOpened(paperTrade(8, QStringLiteral("NSDQ100"), 100000));
        QCOMPARE(f.fake.sent().size(), 2);
        f.exec.onPaperClosed(paperClose(8, QStringLiteral("NSDQ100")));
        QCOMPARE(closed.count(), 1);   // nothing to close by
        QCOMPARE(f.exec.mirrored().size(), 2);
        QCOMPARE(f.exec.mirrored().constLast().state, QStringLiteral("close by hand"));
        QVERIFY(!f.exec.mirrored().constLast().closed);
        QVERIFY(f.logText().contains(QStringLiteral("LIVE CLOSE needed BY HAND")));
        QVERIFY(f.logs.constLast().at(1).toBool());   // …as an ERROR line
    }

    //! @tstid TS-LBOT-004 @design DES-UI-LIVEBOT
    // @relation(REQ-F-076, REQ-N-009, scope=function)
    //
    // The realised daily loss reaching the cap TRIPS the kill switch: sticky, refusing
    // every further order and every re-arm until cleared by hand.
    void TS_LBOT_004_theDailyLossCapTripsTheStickyKillSwitch()
    {
        Fixture f;
        f.exec.onFx(0.9);
        QVERIFY(f.exec.arm());
        f.exec.onPaperOpened(paperTrade(11, QStringLiteral("SPX500"), 27));
        QCOMPARE(f.fake.sent().size(), 1);
        f.exec.onPositionOpened(QStringLiteral("77"), 27, true);
        QCOMPARE(f.exec.mirrored().constFirst().positionId, QStringLiteral("77"));

        // The portfolio poll last saw the position 280 USD down — past the 250 EUR cap,
        // which is 277.78 USD at the arming's rate; the close realises that.
        Position p;
        p.positionId = QStringLiteral("77");
        p.instrumentId = 27;
        p.profit = -280.0;
        f.exec.onPortfolio({p});
        f.exec.onPositionClosed(true, QStringLiteral("closed"), QStringLiteral("77"));
        QCOMPARE(f.exec.day().realized, usd(-280.0));
        QVERIFY(f.exec.isTripped());
        QVERIFY(!f.exec.isArmed());
        QVERIFY(f.exec.stateLine().contains(QStringLiteral("KILL SWITCH")));
        QVERIFY(f.exec.stateLine().contains(QStringLiteral("daily loss cap reached")));
        // …compared in the ACCOUNT's currency: 250 EUR is 277.78 USD at the arming's rate,
        // and the ledger is what the broker realises, in USD.
        QVERIFY(f.exec.stateLine().contains(QStringLiteral("277.78 USD loss cap")));
        QVERIFY(f.logText().contains(QStringLiteral("against 277.78 USD (250.00 EUR)")));
        QVERIFY(f.logText().contains(QStringLiteral("LIVE KILL SWITCH tripped")));

        // Tripped: a further paper open is refused (and recorded as kill-switch), and so
        // is arming again.
        f.exec.onPaperOpened(paperTrade(12, QStringLiteral("SPX500"), 27));
        QCOMPARE(f.fake.sent().size(), 1);
        QCOMPARE(f.audit().constLast().outcome, QStringLiteral("refused-arm"));
        QCOMPARE(f.audit().constLast().detail, QStringLiteral("kill-switch"));
        QVERIFY(!f.exec.arm());
        QVERIFY(f.logText().contains(QStringLiteral("LIVE ARM refused: the kill switch")));

        // Only the explicit clear re-enables arming — and clearing does not arm.
        f.exec.clearTrip();
        QVERIFY(!f.exec.isTripped());
        QVERIFY(!f.exec.isArmed());
        QVERIFY(f.exec.arm());
        QVERIFY(f.exec.isArmed());

        // A close whose P/L was never polled books nothing and says so.
        f.exec.onPaperOpened(paperTrade(13, QStringLiteral("NSDQ100"), 100000));
        f.exec.onPositionOpened(QStringLiteral("78"), 100000, true);
        f.exec.onPositionClosed(true, QStringLiteral("closed"), QStringLiteral("78"));
        QCOMPARE(f.exec.day().realized, usd(-280.0));
        QVERIFY(f.logText().contains(QStringLiteral("result UNKNOWN")));
        QVERIFY(f.exec.isArmed());   // −280 was already past the cap once; a new trip needs
                                     // a new booked loss, and nothing was booked
    }

    //! @tstid TS-LBOT-005 @design DES-UI-LIVEBOT
    // @relation(REQ-F-076, REQ-N-005, scope=function)
    //
    // Arming fails closed on every precondition, each named: a client that is not live,
    // a tripped kill switch, a record that does not clear the REQ-F-031 gate, and an
    // unknown EUR/USD rate. Nothing the executor computes arms it.
    void TS_LBOT_005_armingIsRefusedByEachUnmetPrecondition()
    {
        // The DEFAULT live check is the client's own answer — a credential-less client is
        // in simulation, so the composition root's default refuses here. (Each fixture in
        // its own scope: one process holds one book, and a runner that does not own the
        // book is refused for THAT, which is not the reason under test.)
        {
            Fixture sim(/*clientLive=*/false);
            sim.exec.onFx(0.9);
            QVERIFY(!sim.exec.clientLive());
            QVERIFY(!sim.exec.arm());
            QVERIFY(sim.logText().contains(
                QStringLiteral("LIVE ARM refused: the broker client is not live")));
            QVERIFY(!sim.exec.isArmed());
        }
        {
            Fixture f;
            QVERIFY(f.exec.clientLive());
            QVERIFY(f.runner.ownsBook());
            // No EUR/USD rate yet: the EUR caps cannot be priced, so nothing may be armed.
            QVERIFY(!f.exec.arm());
            QVERIFY2(f.logText().contains(QStringLiteral("EUR/USD rate is not known")),
                     qPrintable(f.logText()));
            f.exec.onFx(0.9);
            // Tripped beats everything else.
            f.exec.trip(QStringLiteral("test"));
            QVERIFY(!f.exec.arm());
            QVERIFY(f.logText().contains(QStringLiteral("the kill switch is tripped (test)")));
            f.exec.clearTrip();
            QVERIFY(f.exec.arm());
            f.exec.disarm();
        }
        // A runner that does NOT own the book (another process trades it) is refused too.
        {
            Fixture owner;
            Fixture onlooker;
            QVERIFY(owner.runner.ownsBook());
            QVERIFY(!onlooker.runner.ownsBook());
            onlooker.exec.onFx(0.9);
            QVERIFY(!onlooker.exec.arm());
            QVERIFY(onlooker.logText().contains(QStringLiteral("does not own the paper bot's "
                                                               "book")));
        }

        // The REQ-F-031 gate: a fresh runner over an EMPTY record is refused with the
        // blockers named — readiness is a precondition of arming, not decoration.
        removeRunnerFiles();
        EtoroClient client(Config{});
        BotSimRunner fresh(&client, nullptr, nullptr, QLatin1String(kStore));
        QVERIFY(!fresh.liveReadiness().ready);
        FakeOrderGateway fake;
        LiveBotExecutor exec(&client, &fresh, &fake, setup(/*clientLive=*/true));
        const QSignalSpy logs(&exec, &LiveBotExecutor::log);
        exec.onFx(0.9);
        QVERIFY(!exec.arm());
        QVERIFY(!exec.isArmed());
        QVERIFY(logs.constLast().at(0).toString().contains(
            QStringLiteral("not ready for real money (REQ-F-031)")));
        QVERIFY(logs.constLast().at(0).toString().contains(QStringLiteral("closed trades")));
        // …and an armed-looking paper open still sends nothing.
        exec.onPaperOpened(paperTrade(1, QStringLiteral("SPX500"), 27));
        QVERIFY(fake.sent().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestLiveBotExecutor)
#include "tst_livebotexecutor.moc"
