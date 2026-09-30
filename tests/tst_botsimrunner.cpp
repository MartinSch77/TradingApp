// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// The paper bot's RUNNER — the plumbing between the live feeds and the pure rules —
// driven headless through its public surface (DES-UI-BOTSIM, REQ-F-029).
//
// The client is constructed without credentials (SIMULATION mode, no network, never
// started) except where a test says otherwise (TS-BOTSIM-006 runs a real-mode client
// against an in-process mock of the venue, because only that can hand the runner a
// per-tick quote with a venue stamp on it). The books live in the test config directory
// under their own store name, and the honest observable is what the runner REPORTS per
// evaluated candidate: the `entryDecision` signal (traded or refused, with the countable
// code) — the same line the advise console prints — so a rule that silently refuses (or
// silently passes) shows up as a code rather than as an absence. The prediction ledger is
// checked where it applies: a ledger row needs a price (`Prediction::isValid`), so an
// UNPRICED candidate has none.

#include "ui/BotSimRunner.h"

#include "MockHttpServer.h"
#include "domain/DecisionEngine.h"
#include "domain/Models.h"
#include "domain/PositionMath.h"
#include "domain/PredictionLedger.h"
#include "services/Config.h"
#include "services/EtoroClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
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

// No books at all: a store or ledger left by an earlier run (or an earlier pass of the
// same test) would make "the row this scan wrote" indistinguishable from an old one.
void removeRunnerFiles()
{
    for (const QString &path : runnerFiles()) {
        static_cast<void>(QFile::remove(path));
    }
}

// The venue id the mock gives BTC. A per-tick quote exists only for an instrument the bot
// HOLDS (its holdings are registered as quote interest) or the one on screen; here BTC is
// both, so the quote the runner reads for it is the one the mock stamps.
constexpr qint64 kBtcId = 100000;

// A book holding one open BTC position on kBtcId, written as the runner's store so a
// runner constructed next restores it (and registers the id as quote interest): the
// stacking case, which is the only way the composite bot ever meets a per-tick quote
// for a candidate. Wide stop and target, so no barrier can close it on the first mark.
bool seedBookHoldingBtc()
{
    PaperBook book;
    EntrySignal sig;
    sig.valid = true;
    sig.symbol = QStringLiteral("BTC");
    sig.instrumentId = kBtcId;
    sig.isBuy = true;
    sig.fillRate = 60000.0;
    sig.spreadPct = 1.0;
    sig.leverage = 2;
    sig.slRate = 50000.0;
    sig.tpRate = 70000.0;
    sig.confidence = 60.0;
    if (book.open(sig, 500.0, QDateTime::currentDateTime()) == 0) {
        return false;
    }
    QJsonObject root = book.toJson();
    root.insert(QStringLiteral("armed"), true);
    QFile file(runnerFiles().constFirst());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(QJsonDocument(root).toJson()) > 0;
}

// A book holding one OPEN swing-strategy position — strategyVersion set, the fixed R basis
// seeded, VALID entry features (the composite path's featuresFor sets them at open; the
// swing path's swingFeaturesFor does too, and recordExperience drops a record without
// them) and NO venue id, so the runner marks it off the 1-minute series exactly like an
// id-less crypto position. Entry 100 with the initial stop at 98: R = 2, so the strategy's
// 2R partial fires at a daily close of 104 or better. Returns the trade id, 0 on failure.
constexpr double kSwingEntry = 100.0;
constexpr double kSwingStop = 98.0;

qint64 seedBookHoldingSwingPosition(const QString &symbol)
{
    PaperBook book;
    EntrySignal sig;
    sig.valid = true;
    sig.symbol = symbol;
    sig.instrumentId = 0;
    sig.isBuy = true;
    sig.fillRate = kSwingEntry;
    sig.spreadPct = 0.1;
    sig.leverage = 1;
    sig.slRate = kSwingStop;
    sig.tpRate = 0.0;   // the swing has no fixed target: the 2R partial is its first exit
    const qint64 id = book.open(sig, 1000.0, QDateTime::currentDateTime().addDays(-1));
    if (id == 0) {
        return 0;
    }
    book.setStrategyVersion(id, QStringLiteral("swing-pullback-v1"));
    book.setSwingInitialStop(id, kSwingStop);
    book.setSwingState(id, kSwingStop, false, 0);
    EntryFeatures features;
    features.volPct = 0.8;
    features.stopPct = 2.0;
    features.targetPct = 4.0;
    features.spreadPct = sig.spreadPct;
    features.leverage = 1;
    features.dir = 1;
    book.setFeatures(id, features);
    QFile file(runnerFiles().constFirst());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return 0;
    }
    return (file.write(QJsonDocument(book.toJson()).toJson()) > 0) ? id : 0;
}

// `closes.size()` daily bars closing at the given values, each with a half-point range
// around its close — the shape applySwingExit's day rules read (close, and the low for
// the five-day-low trailing stop).
QList<DailyBar> dailyBars(const QList<double> &closes)
{
    QList<DailyBar> bars;
    for (const double close : closes) {
        DailyBar bar;
        bar.open = close - 0.2;
        bar.high = close + 0.5;
        bar.low = close - 0.5;
        bar.close = close;
        bars.append(bar);
    }
    return bars;
}

// Every example the runner's experience log holds for `symbol`, one JSON object per line.
QList<QJsonObject> experienceRecordsFor(const QString &symbol)
{
    QList<QJsonObject> records;
    QFile file(runnerFiles().at(3));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return records;
    }
    while (!file.atEnd()) {
        const QJsonObject rec = QJsonDocument::fromJson(file.readLine().trimmed()).object();
        if (rec.value(QStringLiteral("symbol")).toString() == symbol) {
            records.append(rec);
        }
    }
    return records;
}

// The venue, as far as this needs it: BTC resolves to kBtcId, and its rates row is stamped
// `behindSecs` behind the clock AT EACH REQUEST — a row that lags but keeps moving, which
// is what eToro's public feed does for a delayed instrument during an open session (the
// tradeability rule reads a moving stamp as "open"; the age rule is what says "stale").
// Everything else (portfolio, fees, candles — so no candle repair can rescue the row) 404s.
MockHttpServer::Handler venueWithBtcRowBehindBy(qint64 behindSecs)
{
    return [behindSecs](const QByteArray &, const QString &path) {
        if (path.contains(QStringLiteral("/market-data/search"))) {
            return MockHttpServer::Response{
                200,
                QStringLiteral(R"({"items":[{"instrumentId":%1,"internalSymbolFull":"BTC",
                                             "currentRate":60000.0}]})")
                    .arg(kBtcId)
                    .toUtf8(),
                {}};
        }
        if (path.contains(QStringLiteral("/market-data/instruments/rates"))) {
            const QString stamp =
                QDateTime::currentDateTimeUtc().addSecs(-behindSecs).toString(Qt::ISODate);
            return MockHttpServer::Response{
                200,
                QStringLiteral(R"({"rates":[{"instrumentId":%1,"bid":60000.0,"ask":60010.0,
                                             "date":"%2"}]})")
                    .arg(kBtcId)
                    .arg(stamp)
                    .toUtf8(),
                {}};
        }
        return MockHttpServer::Response{404, "{}", {}};
    };
}

// What one scan of the seeded book against that venue produced. Filled by
// scanBtcWithRowBehindBy; the sentinels survive an early return inside it, so a failed
// setup reads as a wrong value in the caller's assertions, never as a pass.
struct QuoteRun {
    qint64 quoteAgeMs = -2;   // the per-tick quote's age when read
    QString code = QStringLiteral("<not run>");   // the reported entry code for BTC
    QString ledgerRefusal = QStringLiteral("<no row>");   // the ledger row's, if any
    int openAfter = -1;   // open positions after the scan
    bool markLive = true;   // the held position's mark flag
};

// The persisted book as the runner wrote it (empty when there is none yet).
QJsonObject savedBook()
{
    QFile file(runnerFiles().constFirst());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

// A prediction-ledger HISTORY for `symbol`, written to the shared ledger file before a
// runner reads it: `count` long calls five minutes apart, every one of them right (the
// price steps up 0.1% per row), the LAST call `lastCallMinutesAgo` before now — so a row
// the runner appends at "now" is that call's earliest row at or past the 15-minute
// horizon and RESOLVES it, moving the record's sample count. Returned as written (oldest
// first); empty when the file could not be written.
QList<Prediction> seedLedgerFor(const QString &symbol, qint32 count, qint32 lastCallMinutesAgo)
{
    QList<Prediction> rows;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    double price = 60000.0;
    for (qint32 i = 0; i < count; ++i) {
        Prediction p;
        p.at = now.addSecs(-60 * qint64{lastCallMinutesAgo + (5 * (count - 1 - i))});
        p.symbol = symbol;
        p.dir = 1;
        p.strength = 60.0;
        p.measured = 9;
        p.price = price;
        p.regime = Regime::Trend;
        p.priorMoveDir = 1;
        if (!appendPrediction(BotSimRunner::ledgerPath(), p)) {
            return {};
        }
        rows.append(p);
        price *= 1.001;
    }
    return rows;
}

// The FORECAST lines the runner logged for `symbol`, in order.
QStringList forecastLinesFor(const QSignalSpy &logs, const QString &symbol)
{
    QStringList lines;
    for (const QList<QVariant> &args : logs) {
        const QString line = args.at(0).toString();
        if (line.startsWith(QStringLiteral("FORECAST %1:").arg(symbol))) {
            lines.append(line);
        }
    }
    return lines;
}

// The record part of a FORECAST line — what scoreHorizon's headline said about the calls
// the runner's copy of the ledger holds.
QString recordPartOf(const QString &forecastLine)
{
    return forecastLine.section(QStringLiteral(" · record: "), 1);
}

// The mark the persisted book holds for its first open position (0 when there is none).
double savedMarkRate()
{
    const QJsonArray open = savedBook().value(QStringLiteral("open")).toArray();
    return open.isEmpty() ? 0.0
                          : open.first().toObject().value(QStringLiteral("markRate")).toDouble();
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

// Seed the book, bring up the venue and a real-mode client on it, let the per-tick quote
// land, then run ONE scan with a BUY row for BTC and report what the runner said.
void scanBtcWithRowBehindBy(qint64 behindSecs, QuoteRun *out)
{
    removeRunnerFiles();
    QVERIFY(seedBookHoldingBtc());
    MockHttpServer server(venueWithBtcRowBehindBy(behindSecs));
    QVERIFY(server.listen(QHostAddress::LocalHost));
    EtoroClient::setTradeConfigBaseForTesting(server.baseUrl());   // fees: local, 404

    Config cfg;
    cfg.apiKey = QStringLiteral("k");   // credentials → the real-mode client
    cfg.userKey = QStringLiteral("u");
    cfg.mode = QStringLiteral("demo");
    cfg.symbol = QStringLiteral("BTC");
    cfg.baseUrl = server.baseUrl() + QStringLiteral("/api");
    cfg.pollIntervalMs = 25;
    EtoroClient client(cfg);
    BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
    QCOMPARE(runner.book().openTrades().size(), 1);   // the seeded holding was restored
    QVERIFY(runner.armed());
    QSignalSpy decisions(&runner, &BotSimRunner::entryDecision);
    QVERIFY(decisions.isValid());
    client.start();

    const QString symbol = QStringLiteral("BTC");
    QTRY_VERIFY_WITH_TIMEOUT(client.quotes().value(kBtcId).isValid(), 15000);
    QCOMPARE(client.instrumentIdFor(symbol), kBtcId);
    out->quoteAgeMs = client.quotes().value(kBtcId).ageMs(QDateTime::currentDateTimeUtc());

    MarketSnapshot snap;
    snap.screenerRows = {scanRow(symbol, 60)};
    snap.intradayBySymbol.insert(symbol, scanRow(symbol, 30).closes);
    runner.onDecisions({buyRow(symbol)}, snap);

    out->code = decisionCodeFor(decisions, symbol);
    out->openAfter = static_cast<int>(runner.book().openTrades().size());
    if (out->openAfter == 1) {
        out->markLive = runner.book().openTrades().constFirst().markLive;
    }
    // The list must outlive the pointer into it (rowFor returns one into its argument).
    const QList<Prediction> ledger = loadPredictions(BotSimRunner::ledgerPath());
    if (const Prediction *row = rowFor(ledger, symbol); row != nullptr) {
        out->ledgerRefusal = row->refusal;
    }
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

    // Every test starts from no books at all (see removeRunnerFiles).
    void init() { removeRunnerFiles(); }

    void cleanup() { removeRunnerFiles(); }

    //! @tstid TS-BOTSIM-001 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-032, scope=function)
    //
    // `quoteLive` is the runner's claim. The simulation client cannot hand it a stale
    // per-tick quote (its only quote is stamped `now`), so the AGE rule itself is pinned
    // as the pure function in TS-PAPER-045 and the runner's WIRING of it — the stale
    // branch of sidesFor, the `live` term in candidateFor, the mark — in TS-BOTSIM-006
    // against a mock venue. What this one drives is the other half of the same contract,
    // the two untimed price paths: an instrument with no per-tick quote and no venue rate
    // is still priced off its scan candle close and counts as live — the gate gets past
    // `no-live-quote` to whatever comes next, and the ledger has its row — while an
    // instrument nothing prices at all is refused exactly there, and REFUSED rather than
    // absent: the runner reports a decision for both, so a silent skip could not pass as
    // a refusal. Crypto on purpose: 24/7, so the day gate cannot turn this into a
    // weekend-dependent test. That is ALL crypto buys here — the client is never started,
    // so tradeability is unknown and `market-closed` is unreachable for any symbol, which
    // is why this test makes no claim about it.
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
    // step with every row it writes — instead of re-reading the whole file every scan, and
    // the FORECAST line's record part is what makes each half of that observable, because
    // it is computed from whatever copy the runner scores. A history seeded on disk BEFORE
    // the runner reads it is calibrated at 15 minutes (48 calls, five minutes apart, all
    // right), with its last call placed so that the row the first scan appends at "now"
    // resolves it: the first scan's line must then carry the record of the file AS IT IS
    // AFTER that scan — seeded rows plus the runner's own — which proves the load (a
    // never-loaded copy reports "0 of 40 samples needed") AND the in-step append (a copy
    // loaded but not appended to reports the seeded count, one sample short). The ledger
    // file is then DELETED and a second scan run: its line must still carry that same
    // record — the copy is not re-read from disk, where the history is gone (the pre-cache
    // code re-read per scan and would report "0 of 40") — while the file now holds exactly
    // the one new row, because the record is the file, and the copy only follows it.
    void TS_BOTSIM_002_theLedgerIsCachedInMemoryAndStaysInStepWithTheFile()
    {
        const QString symbol = QStringLiteral("BTC");
        // 16 minutes ago, not 15: the appended row's elapsed time then reads 16 whole
        // minutes whatever fraction of a minute the scan itself takes — at the horizon,
        // inside its 22-minute limit.
        const QList<Prediction> seeded = seedLedgerFor(symbol, 48, 16);
        QCOMPARE(seeded.size(), 48);
        const HorizonScore seededScore = scoreHorizon(seeded, Horizon::M15);
        QVERIFY(seededScore.trustworthy());   // the record part prints a hit rate, not a wait

        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy logs(&runner, &BotSimRunner::log);
        QVERIFY(logs.isValid());
        runner.setArmed(true);
        MarketSnapshot snap;
        snap.screenerRows = {scanRow(symbol, 60)};
        snap.intradayBySymbol.insert(symbol, scanRow(symbol, 30).closes);

        // Scan 1: the file gains the runner's row, and the line reports the record OF THAT
        // FILE — the seeded history with the new row resolving its last call.
        runner.onDecisions({buyRow(symbol)}, snap);
        const QList<Prediction> afterFirst = loadPredictions(BotSimRunner::ledgerPath());
        QCOMPARE(afterFirst.size(), seeded.size() + 1);
        QCOMPARE(afterFirst.constLast().symbol, symbol);
        const HorizonScore firstScore = scoreHorizon(afterFirst, Horizon::M15);
        QVERIFY(firstScore.samples > seededScore.samples);   // the premise: the row resolved a call
        QCOMPARE(forecastLinesFor(logs, symbol).size(), 1);
        const QString firstLine = forecastLinesFor(logs, symbol).constFirst();
        QCOMPARE(recordPartOf(firstLine), firstScore.headline());
        QVERIFY(recordPartOf(firstLine) != seededScore.headline());

        // Scan 2 with the file GONE: the copy still holds every row it loaded and appended,
        // so the line does not change — and the file holds only what this scan wrote.
        QVERIFY(QFile::remove(BotSimRunner::ledgerPath()));
        runner.onDecisions({buyRow(symbol)}, snap);
        QCOMPARE(forecastLinesFor(logs, symbol).size(), 2);
        QCOMPARE(forecastLinesFor(logs, symbol).at(1), firstLine);
        const QList<Prediction> afterSecond = loadPredictions(BotSimRunner::ledgerPath());
        QCOMPARE(afterSecond.size(), 1);
        QCOMPARE(afterSecond.constFirst().symbol, symbol);
        QVERIFY(afterSecond.constFirst().at >= afterFirst.constLast().at);
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
        // Either way the ledger has its row, carrying the same verdict. (The list is a
        // named local: rowFor returns a pointer INTO it, which a temporary would free.)
        const QList<Prediction> ledger = loadPredictions(BotSimRunner::ledgerPath());
        const Prediction *btc = rowFor(ledger, symbol);
        QVERIFY(btc != nullptr);
        QCOMPARE(btc->refusal, code == QStringLiteral("opened") ? QString{} : code);
    }

    //! @tstid TS-BOTSIM-006 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-032, scope=function)
    //
    // The runner's WIRING of the age rule (the rule itself is TS-PAPER-045), driven the
    // only way a per-tick quote with a venue stamp can reach it: a real-mode client on a
    // mock venue, and a book that already HOLDS the instrument — the stacking case, which
    // is the measured defect (a held instrument whose per-tick quote had stopped printing
    // 11 minutes earlier was still stacked, because `quoteLive` never read the stamp).
    // With BTC's rates row stamped a minute past the open-trades table's own bound
    // (`kQuoteStaleMs`) the per-tick quote is stale: the candidate is still PRICED (a row
    // in the ledger, so the refusal is countable) but refused exactly `no-live-quote`, and
    // the held position's mark is booked as NOT live — while the same venue with the row
    // stamped now takes the runner past that gate (whatever refuses or takes it comes
    // later) and marks the position live. 24/7 crypto on purpose, so neither the day gate
    // nor a weekday session can precede the quote rule. The row lags but keeps MOVING at
    // each request, exactly eToro's delayed-feed shape: the tradeability rule reads that
    // as an open market, and only the age rule can say "stale".
    void TS_BOTSIM_006_aStalePerTickQuoteIsPricedButRefusedAsNotLiveAndMarksNotLive()
    {
        QuoteRun stale;
        scanBtcWithRowBehindBy((kQuoteStaleMs / 1000) + 60, &stale);
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY2(stale.quoteAgeMs > kQuoteStaleMs, qPrintable(QString::number(stale.quoteAgeMs)));
        QCOMPARE(stale.code, QStringLiteral("no-live-quote"));
        QCOMPARE(stale.ledgerRefusal, QStringLiteral("no-live-quote"));   // priced, so counted
        QCOMPARE(stale.openAfter, 1);   // the held position is marked, not closed
        QVERIFY(!stale.markLive);

        QuoteRun fresh;
        scanBtcWithRowBehindBy(0, &fresh);
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY2(fresh.quoteAgeMs <= kQuoteStaleMs, qPrintable(QString::number(fresh.quoteAgeMs)));
        QVERIFY2(fresh.code != QStringLiteral("no-live-quote"), qPrintable(fresh.code));
        QVERIFY2(fresh.code != QStringLiteral("market-closed"), qPrintable(fresh.code));
        QVERIFY2(fresh.code != QStringLiteral("<no decision reported>"), qPrintable(fresh.code));
        QCOMPARE(fresh.openAfter, 1);
        QVERIFY(fresh.markLive);
    }

    //! @tstid TS-BOTSIM-007 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-033, scope=function)
    //
    // The experience log counts a partially closed position ONCE, labelled with the WHOLE
    // position's net. The only producer of partial records is the swing strategy's 2R
    // partial (applySwingExit), so that is what drives it: a book restored holding one
    // swing position (entry 100, initial stop 98, R = 2) is marked off a 1-minute close of
    // 105 — the day rules take 45% off at 2.5R and raise the trailing stop to at least the
    // five-day low — and no example is written yet (the setup is still open). A second
    // mark at 99, below that trailing stop but ABOVE the initial one, closes the remainder
    // at a loss; the log then holds exactly one line for the symbol whose label is the
    // partial's net plus the remainder's — positive, where the remainder's alone is
    // negative. That mislabel (a banked 2R trailed out below entry taught a LOSS) is the
    // measured defect, and the domain identity behind the sum is TS-PAPER-046. The seed
    // carries its features directly: the swing ENTRY path setting them (swingFeaturesFor
    // in trySwingOpen) is not drivable here — the runner has no public switch for
    // BotConfig::useSwingStrategy and the book file does not persist it — so that wiring
    // is verified by inspection; this pins that a record WITH features is folded, not
    // dropped or written twice.
    void TS_BOTSIM_007_aPartiallyClosedPositionIsOneExampleLabelledWithTheWholeNet()
    {
        const QString symbol = QStringLiteral("SPX500");
        QVERIFY(seedBookHoldingSwingPosition(symbol) != 0);
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QCOMPARE(runner.book().openTrades().size(), 1);
        QVERIFY(runner.book().openTrades().constFirst().features.isValid());
        QCOMPARE(runner.book().openTrades().constFirst().strategyVersion,
                 QStringLiteral("swing-pullback-v1"));
        // Six sessions rising to 105: 2.5R above entry, and a five-day low of 100.5.
        runner.setDailyBars(symbol, dailyBars({100.0, 101.0, 102.0, 103.0, 104.0, 105.0}));

        // First mark: the partial. One closed record, flagged partial; the remainder stays
        // open under the same id with its stop trailed up; nothing in the log yet.
        MarketSnapshot atTarget;
        atTarget.intradayBySymbol.insert(symbol, {104.0, 105.0});
        runner.onDecisions({}, atTarget);
        QCOMPARE(runner.book().closedTrades().size(), 1);
        QVERIFY(runner.book().closedTrades().constFirst().partial);
        QVERIFY(runner.book().closedTrades().constFirst().netPnl > 0.0);
        QCOMPARE(runner.book().openTrades().size(), 1);
        QVERIFY(runner.book().openTrades().constFirst().swingPartialTaken);
        QVERIFY(runner.book().openTrades().constFirst().slRate >= 100.5);
        QVERIFY(experienceRecordsFor(symbol).isEmpty());

        // Second mark: below the trailing stop, above the initial one — the remainder
        // closes at a loss through the barrier check, the position is gone.
        MarketSnapshot belowTrail;
        belowTrail.intradayBySymbol.insert(symbol, {99.5, 99.0});
        runner.onDecisions({}, belowTrail);
        QVERIFY(runner.book().openTrades().isEmpty());
        const QList<PaperClosedTrade> &closed = runner.book().closedTrades();
        QCOMPARE(closed.size(), 2);
        QVERIFY(!closed.at(1).partial);
        QCOMPARE(closed.at(1).reason, CloseReason::StopLoss);
        QVERIFY(closed.at(1).netPnl < 0.0);   // the remainder alone reads as a loss

        // ONE example, labelled with the position's net over both legs.
        const QList<QJsonObject> records = experienceRecordsFor(symbol);
        QCOMPARE(records.size(), 1);
        const double label = records.constFirst().value(QStringLiteral("netPnl")).toDouble();
        QCOMPARE(label, closed.at(0).netPnl + closed.at(1).netPnl);
        QVERIFY(label > 0.0);   // one winning trade, not one win and one loss
        QVERIFY(label != closed.at(1).netPnl);
        QCOMPARE(records.constFirst().value(QStringLiteral("reason")).toString(),
                 closeReasonWord(CloseReason::StopLoss));
    }

    //! @tstid TS-BOTSIM-008 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, scope=function)
    //
    // What reaches the DISK when. A pure mark used to rewrite the whole book every
    // 5-second tick while a position was open (~17 000 atomic rewrites a day); now the
    // mark pass saves at most once a minute, and everything structural saves at once.
    // Driven through onDecisions with no rows (the mark pass alone, as TS-BOTSIM-007 does)
    // on a restored book holding one BTC position marked off its 1-minute close: the
    // first mark pass saves (nothing marked has been saved yet), the next pass seconds
    // later moves the book but NOT the file (the throttle), and a pass that closes the
    // position — the candle below the stop — reaches the file immediately although the
    // minute is not up (a close is a shape change, never throttled). The minute itself is
    // not waited out here. Arming is structural too, and it is pinned FIRST: the armed
    // flag is persisted precisely so an unattended bot survives a restart, and before this
    // it rode on the next mark save — a crash inside the throttle's minute would have
    // restarted the bot silently disarmed.
    void TS_BOTSIM_008_marksReachTheDiskOncePerMinuteWhileShapeAndArmChangesReachItAtOnce()
    {
        const QString symbol = QStringLiteral("BTC");
        QVERIFY(seedBookHoldingBtc());   // written ARMED, marked at its 60 000 fill
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QCOMPARE(runner.book().openTrades().size(), 1);
        QVERIFY(runner.armed());
        QCOMPARE(savedMarkRate(), 60000.0);

        // Arming and disarming are saved where they happen, with no mark in between.
        runner.setArmed(false);
        QVERIFY(!savedBook().value(QStringLiteral("armed")).toBool());
        runner.setArmed(true);
        QVERIFY(savedBook().value(QStringLiteral("armed")).toBool());
        QCOMPARE(savedMarkRate(), 60000.0);   // the arm saved the book as it was: unmarked

        // First mark pass: nothing marked has reached the disk yet, so this one does.
        MarketSnapshot first;
        first.intradayBySymbol.insert(symbol, {60500.0, 61000.0});
        runner.onDecisions({}, first);
        QCOMPARE(runner.book().openTrades().constFirst().markRate, 61000.0);
        QCOMPARE(savedMarkRate(), 61000.0);

        // Second pass, seconds later: the BOOK moves, the FILE waits for the minute.
        MarketSnapshot second;
        second.intradayBySymbol.insert(symbol, {61500.0, 62000.0});
        runner.onDecisions({}, second);
        QCOMPARE(runner.book().openTrades().constFirst().markRate, 62000.0);
        QCOMPARE(savedMarkRate(), 61000.0);

        // A close inside the pass is a shape change: saved at once, minute or not.
        MarketSnapshot belowStop;
        belowStop.intradayBySymbol.insert(symbol, {49500.0, 49000.0});
        runner.onDecisions({}, belowStop);
        QVERIFY(runner.book().openTrades().isEmpty());
        QCOMPARE(runner.book().closedTrades().size(), 1);
        QCOMPARE(runner.book().closedTrades().constFirst().reason, CloseReason::StopLoss);
        const QJsonObject saved = savedBook();
        QVERIFY(saved.value(QStringLiteral("open")).toArray().isEmpty());
        QCOMPARE(saved.value(QStringLiteral("closed")).toArray().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestBotSimRunner)
#include "tst_botsimrunner.moc"
