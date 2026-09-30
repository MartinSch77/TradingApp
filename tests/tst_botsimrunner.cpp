// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The paper bot's RUNNER — the plumbing between the live feeds and the pure rules —
// driven headless through its public surface (DES-UI-BOTSIM, REQ-F-029).
//
// The client is constructed without credentials (SIMULATION mode, no network, never
// started), the books live in the test config directory under their own store name, and
// the honest observable is what the runner REPORTS per evaluated candidate: the
// `entryDecision` signal (traded or refused, with the countable code) — the same line the
// advise console prints — so a rule that silently refuses (or silently passes) shows up
// as a code rather than as an absence. The prediction ledger is checked where it applies:
// a ledger row needs a price (`Prediction::isValid`), so an UNPRICED candidate has none.

#include "ui/BotSimRunner.h"

#include "domain/DecisionEngine.h"
#include "domain/Models.h"
#include "domain/PredictionLedger.h"
#include "services/Config.h"
#include "services/EtoroClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest/QtTest>

#include <memory>

using namespace trading;

namespace {

// The runner's own book for these tests; its decision and experience logs are siblings
// named from it (see BotSimRunner::siblingPath), the prediction ledger is shared.
constexpr auto kStore = "tst-botsim.json";

QStringList runnerFiles()
{
    const QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    return {dir.filePath(QLatin1String(kStore)),
            dir.filePath(QLatin1String(kStore) + QStringLiteral(".lock")),
            dir.filePath(QStringLiteral("tst-botsim-decisions.log")),
            dir.filePath(QStringLiteral("tst-botsim-experience.jsonl")),
            BotSimRunner::ledgerPath()};
}

// The persisted book as the runner wrote it (empty when there is none yet).
QJsonObject savedBook()
{
    QFile file(runnerFiles().constFirst());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

// A row the composite calls BUY with conviction, as the scan hands it to the runner.
DecisionRow buyRow(const QString &symbol)
{
    DecisionRow row;
    row.symbol = symbol;
    row.dir = 1;
    row.composite = 0.6;
    row.confidence = 60.0;
    row.maxLev = 2;
    return row;
}

// `count` hourly closes, gently rising with a little noise — enough history to derive
// a stop from, a non-degenerate volatility. Zero = an instrument nothing has priced.
ScreenerRow scanRow(const QString &symbol, qsizetype count)
{
    ScreenerRow row;
    row.symbol = symbol;
    row.maxLeverage = 2;
    for (qsizetype i = 0; i < count; ++i) {
        row.closes.append(60000.0 + (static_cast<double>(i) * 20.0)
                          + (((i % 3) == 0) ? 60.0 : -40.0));
    }
    row.lastPrice = row.closes.isEmpty() ? 0.0 : row.closes.constLast();
    row.ok = !row.closes.isEmpty();
    return row;
}

// `count` closes moving monotonically by `step` from `start` — a series whose last six
// points have ONE unambiguous direction, which scanRow's noisy shape does not guarantee.
QList<double> trend(double start, double step, qsizetype count)
{
    QList<double> out;
    for (qsizetype i = 0; i < count; ++i) {
        out.append(start + (static_cast<double>(i) * step));
    }
    return out;
}

// A scan row over the given hourly closes.
ScreenerRow hourlyRow(const QString &symbol, const QList<double> &closes)
{
    ScreenerRow row;
    row.symbol = symbol;
    row.maxLeverage = 2;
    row.closes = closes;
    row.lastPrice = closes.isEmpty() ? 0.0 : closes.constLast();
    row.ok = !closes.isEmpty();
    return row;
}

const Prediction *rowFor(const QList<Prediction> &ledger, const QString &symbol)
{
    for (const Prediction &p : ledger) {
        if (p.symbol == symbol) {
            return &p;
        }
    }
    return nullptr;
}

// The refusal code the runner reported for `symbol` (empty when it traded), or a
// sentinel when it reported nothing about the candidate at all — an ABSENT decision must
// not read as an empty (= taken) code.
QString decisionCodeFor(const QSignalSpy &spy, const QString &symbol)
{
    for (const QList<QVariant> &args : spy) {
        if (args.at(0).toString() == symbol) {
            return args.at(2).toString();
        }
    }
    return QStringLiteral("<no decision reported>");
}

}   // namespace

class TestBotSimRunner : public QObject
{
    Q_OBJECT;

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)));
    }

    // Every test starts from no books at all: a store or ledger left by an earlier run
    // would make "the row this scan wrote" indistinguishable from an old one.
    void init()
    {
        for (const QString &path : runnerFiles()) {
            static_cast<void>(QFile::remove(path));
        }
    }

    void cleanup() { init(); }

    //! @tstid TS-BOTSIM-001 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-032, scope=function)
    //
    // `quoteLive` is the runner's claim, and the simulation client cannot hand it a stale
    // per-tick quote (its only quote is stamped `now`), so the AGE rule itself is pinned
    // in TS-PAPER-045. What IS drivable here is the other half of the same contract, the
    // two untimed price paths: an instrument with no per-tick quote and no venue rate is
    // still priced off its scan candle close and counts as live — the gate gets past
    // `no-live-quote` to whatever comes next, and the ledger has its row — while an
    // instrument nothing prices at all is refused exactly there, and REFUSED rather than
    // absent: the runner reports a decision for both, so a silent skip could not pass as
    // a refusal. Crypto on purpose: 24/7, so the day gate cannot turn this into a
    // weekend-dependent test.
    void TS_BOTSIM_001_unpricedCandidateIsNoLiveQuoteWhilePricedFallbackIsLive()
    {
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy decisions(&runner, &BotSimRunner::entryDecision);
        QVERIFY(decisions.isValid());
        runner.setArmed(true);
        QVERIFY(runner.armed());

        const QString priced = QStringLiteral("BTC");
        const QString unpriced = QStringLiteral("ETH");
        MarketSnapshot snap;
        snap.screenerRows = {scanRow(priced, 60), scanRow(unpriced, 0)};
        snap.intradayBySymbol.insert(priced, scanRow(priced, 30).closes);
        runner.onDecisions({buyRow(priced), buyRow(unpriced)}, snap);

        // One decision per candidate, whatever it was.
        QCOMPARE(decisions.size(), 2);
        // Nothing prices ETH here — no per-tick quote, no venue rate, no candle — and
        // that is the one refusal the quote rule owns.
        QCOMPARE(decisionCodeFor(decisions, unpriced), QStringLiteral("no-live-quote"));
        // BTC is priced off its candle close: the untimed fallback is live, so whatever
        // refuses it (or takes it) is a LATER gate, never the quote rule or the market.
        const QString btcCode = decisionCodeFor(decisions, priced);
        QVERIFY2(btcCode != QStringLiteral("no-live-quote"), qPrintable(btcCode));
        QVERIFY2(btcCode != QStringLiteral("market-closed"), qPrintable(btcCode));
        QVERIFY2(btcCode != QStringLiteral("<no decision reported>"), qPrintable(btcCode));
        // …and being priced, it is the one of the two the ledger can record (a row needs a
        // price): the row carries the same verdict the signal reported.
        const QList<Prediction> ledger = loadPredictions(BotSimRunner::ledgerPath());
        const Prediction *btc = rowFor(ledger, priced);
        QVERIFY(btc != nullptr);
        QCOMPARE(btc->refusal, btcCode == QStringLiteral("opened") ? QString{} : btcCode);
        QVERIFY(rowFor(ledger, unpriced) == nullptr);
    }

    //! @tstid TS-BOTSIM-002 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-037, scope=function)
    //
    // The runner keeps the prediction ledger in memory — read from disk once, appended in
    // step with every row it writes — instead of re-reading the whole file every scan.
    // The observable contract is two-sided: the FILE still receives one row per priced
    // candidate per scan (the cache is a copy, never the record), and the per-scan FORECAST
    // line that is computed FROM the cache is still emitted for every scan, so a cache that
    // was loaded but never appended to, or never loaded at all, would show up as a missing
    // line or a wrong row count rather than as silence.
    void TS_BOTSIM_002_theLedgerIsCachedInMemoryAndStaysInStepWithTheFile()
    {
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy logs(&runner, &BotSimRunner::log);
        QVERIFY(logs.isValid());
        runner.setArmed(true);

        const QString symbol = QStringLiteral("BTC");
        MarketSnapshot snap;
        snap.screenerRows = {scanRow(symbol, 60)};
        snap.intradayBySymbol.insert(symbol, scanRow(symbol, 30).closes);
        const auto forecastLines = [&logs]() {
            qsizetype count = 0;
            for (const QList<QVariant> &args : logs) {
                if (args.at(0).toString().startsWith(QStringLiteral("FORECAST BTC"))) {
                    ++count;
                }
            }
            return count;
        };

        // No ledger file exists yet: the first scan loads an EMPTY ledger and appends to it.
        QVERIFY(!QFile::exists(BotSimRunner::ledgerPath()));
        runner.onDecisions({buyRow(symbol)}, snap);
        QCOMPARE(loadPredictions(BotSimRunner::ledgerPath()).size(), 1);
        QCOMPARE(forecastLines(), 1);

        // The second scan appends a second row — to the file and to the copy the
        // forecast reads — and reports again.
        runner.onDecisions({buyRow(symbol)}, snap);
        const QList<Prediction> ledger = loadPredictions(BotSimRunner::ledgerPath());
        QCOMPARE(ledger.size(), 2);
        QCOMPARE(ledger.at(0).symbol, symbol);
        QCOMPARE(ledger.at(1).symbol, symbol);
        QVERIFY(ledger.at(0).at <= ledger.at(1).at);
        QCOMPARE(forecastLines(), 2);
    }

    //! @tstid TS-BOTSIM-003 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, scope=function)
    //
    // One process per book. The GUI and the console share one config dir and one book by
    // design, so a QLockFile beside the store decides who RUNS it: a second runner on the
    // same store still loads and shows the book but reports ownsBook() == false, drops a
    // restored "armed" flag rather than resuming it, refuses setArmed(true) with a log line
    // naming the holder, and neither marks nor writes on a scan (no decision reported, no
    // ledger row — the owner's files stay the owner's). Releasing the first runner lets
    // the next one own the book, so a clean restart is never locked out.
    void TS_BOTSIM_003_aSecondRunnerOnTheSameBookIsReadOnlyAndCannotBeArmed()
    {
        EtoroClient client(Config{});
        auto first =
            std::make_unique<BotSimRunner>(&client, nullptr, nullptr, QLatin1String(kStore));
        QVERIFY(first->ownsBook());
        first->setArmed(true);
        QVERIFY(first->armed());
        // A config change saves the book, armed flag included — what a second process
        // then RESTORES from the file (the focus set itself is not persisted; `armed` is).
        first->setFocusSymbols({QStringLiteral("BTC")});
        QVERIFY(savedBook().value(QStringLiteral("armed")).toBool());

        BotSimRunner second(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy logs(&second, &BotSimRunner::log);
        QSignalSpy decisions(&second, &BotSimRunner::entryDecision);
        QVERIFY(logs.isValid());
        QVERIFY(decisions.isValid());
        QVERIFY(!second.ownsBook());
        QVERIFY(!second.armed());   // the file said armed; the holder runs it, not this one

        second.setArmed(true);
        QVERIFY(!second.armed());
        QCOMPARE(logs.size(), 1);
        const QString refusal = logs.constFirst().at(0).toString();
        QVERIFY2(refusal.contains(QStringLiteral("held")), qPrintable(refusal));
        QVERIFY2(refusal.contains(QStringLiteral("pid %1").arg(QCoreApplication::applicationPid())),
                 qPrintable(refusal));
        QVERIFY(logs.constFirst().at(1).toBool());   // reported as an error, not as chatter

        // A scan reaches the read-only views and nothing else: no candidate is evaluated,
        // no ledger row is written.
        const QString symbol = QStringLiteral("BTC");
        MarketSnapshot snap;
        snap.screenerRows = {scanRow(symbol, 60)};
        snap.intradayBySymbol.insert(symbol, scanRow(symbol, 30).closes);
        second.onDecisions({buyRow(symbol)}, snap);
        QCOMPARE(decisions.size(), 0);
        QVERIFY(!QFile::exists(BotSimRunner::ledgerPath()));

        // The lock goes with its holder.
        first.reset();
        const BotSimRunner third(&client, nullptr, nullptr, QLatin1String(kStore));
        QVERIFY(third.ownsBook());
        // …and the second runner does not retroactively become the owner: the lock is
        // tried once, at construction.
        QVERIFY(!second.ownsBook());
    }

    //! @tstid TS-BOTSIM-004 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-037, scope=function)
    //
    // The ledger row's "previous five minutes" baseline and its price come from the
    // instrument's 1-MINUTE series (MarketSnapshot::intradayBySymbol — the series the
    // window's engine reads), not from the eToro scan's HOURLY closes, whose last six
    // points span five hours. The two series are made to DISAGREE — hourly rising, 1-minute
    // falling — so a baseline read off the wrong one shows as the wrong sign, not as a
    // coincidence; the row is written for a refused candidate too, so no trade need open.
    // A symbol with no 1-minute series has no measurable baseline (0, unknown) and falls
    // back to the hourly last close as its price; every row carries the composite bot's
    // strategy version, so rows from before this switch (empty version) score apart.
    void TS_BOTSIM_004_theLedgerBaselineAndPriceComeFromTheOneMinuteSeries()
    {
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        runner.setArmed(true);
        QVERIFY(runner.armed());

        const QString withSession = QStringLiteral("BTC");
        const QString hourlyOnly = QStringLiteral("ETH");
        MarketSnapshot snap;
        // Both hourly series RISE over their last six points.
        const ScreenerRow btcHourly = hourlyRow(withSession, trend(60000.0, 20.0, 60));
        const ScreenerRow ethHourly = hourlyRow(hourlyOnly, trend(3000.0, 2.0, 60));
        snap.screenerRows = {btcHourly, ethHourly};
        // The 1-minute series falls over its last six points and ends at a price the hourly
        // series never reaches, so the price's source is unambiguous too.
        const QList<double> btcSession = trend(62000.0, -5.0, 40);
        snap.intradayBySymbol.insert(withSession, btcSession);
        runner.onDecisions({buyRow(withSession), buyRow(hourlyOnly)}, snap);

        const QList<Prediction> ledger = loadPredictions(BotSimRunner::ledgerPath());
        const Prediction *btc = rowFor(ledger, withSession);
        QVERIFY(btc != nullptr);
        QCOMPARE(btc->priorMoveDir, -1);   // the 1-minute series fell; the hourly one rose
        QCOMPARE(btc->price, btcSession.constLast());
        QCOMPARE(btc->strategyVersion, QStringLiteral("composite-v2"));

        const Prediction *eth = rowFor(ledger, hourlyOnly);
        QVERIFY(eth != nullptr);
        QCOMPARE(eth->priorMoveDir, 0);   // no 1-minute series: unmeasured, not the hourly sign
        QCOMPARE(eth->price, ethHourly.closes.constLast());
        QCOMPARE(eth->strategyVersion, QStringLiteral("composite-v2"));
    }

    //! @tstid TS-BOTSIM-005 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-031, scope=function)
    //
    // Crypto is actually tradable: its venue id never resolves in this build, so its scan
    // row comes from the public-feed fallback (fromFallbackFeed) and its price from its
    // candles — and neither of the two blockers that used to stop it there may fire. The
    // candle fallback prices it (never `no-live-quote`, TS-BOTSIM-001), and an id-less
    // crypto candidate is no longer refused `instrument-unresolved`: whatever decides it is
    // one of the strategy's own rules, and when they take it the position is booked with
    // instrumentId 0, which the mark path (markFor, candle fallback) then handles. Asserted
    // on the refusal code rather than on an open, because `onDecisions` reads the wall
    // clock and the day/session rules would make an open calendar-dependent.
    void TS_BOTSIM_005_anIdLessCryptoCandidateIsNotRefusedForItsMissingVenueId()
    {
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy decisions(&runner, &BotSimRunner::entryDecision);
        QVERIFY(decisions.isValid());
        runner.setFocusSymbols({QStringLiteral("BTC")});
        runner.setArmed(true);
        QVERIFY(runner.armed());

        const QString symbol = QStringLiteral("BTC");
        QCOMPARE(client.instrumentIdFor(symbol), qint64(0));   // the premise: no venue id
        MarketSnapshot snap;
        ScreenerRow row = scanRow(symbol, 60);
        row.fromFallbackFeed = true;
        snap.screenerRows = {row};
        snap.intradayBySymbol.insert(symbol, scanRow(symbol, 30).closes);
        runner.onDecisions({buyRow(symbol)}, snap);

        QCOMPARE(decisions.size(), 1);
        const QString code = decisionCodeFor(decisions, symbol);
        QVERIFY2(code != QStringLiteral("instrument-unresolved"), qPrintable(code));
        QVERIFY2(code != QStringLiteral("no-live-quote"), qPrintable(code));
        QVERIFY2(code != QStringLiteral("not-focus"), qPrintable(code));
        QVERIFY2(code != QStringLiteral("<no decision reported>"), qPrintable(code));
        // When the strategy's own rules take it, the book holds it without a venue id.
        if (code == QStringLiteral("opened")) {
            QCOMPARE(runner.book().openTrades().size(), 1);
            QCOMPARE(runner.book().openTrades().constFirst().symbol, symbol);
            QCOMPARE(runner.book().openTrades().constFirst().instrumentId, qint64(0));
        } else {
            QVERIFY(runner.book().openTrades().isEmpty());
        }
        // Either way the ledger has its row, carrying the same verdict.
        const Prediction *btc = rowFor(loadPredictions(BotSimRunner::ledgerPath()), symbol);
        QVERIFY(btc != nullptr);
        QCOMPARE(btc->refusal, code == QStringLiteral("opened") ? QString{} : code);
    }
};

QTEST_GUILESS_MAIN(TestBotSimRunner)
#include "tst_botsimrunner.moc"
