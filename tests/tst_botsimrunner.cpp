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
#include "domain/PaperTrader.h"
#include "domain/PositionMath.h"
#include "domain/PredictionLedger.h"
#include "services/Config.h"
#include "services/EtoroClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest/QtTest>

#include <algorithm>
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

// The whole of a file, empty when it cannot be read.
QByteArray fileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A lock file with the given contents and modification time — a lock as a process that
// is no longer holding it natively left it (TS-BOTSIM-009). The time is set on the open
// handle, since a closed file's time cannot be set through QFile.
bool writeLockFile(const QString &path, const QByteArray &bytes, const QDateTime &modified)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()
        || !file.flush()) {   // a write still buffered at close() would re-stamp the file
        return false;
    }
    return file.setFileTime(modified, QFileDevice::FileModificationTime);
}

// A pid no process can have: Linux caps pid_max at 2^22, Windows pids are far smaller.
constexpr qint64 kNoSuchPid = 2000000000;

// The strategy version the composite/AI bot tags its ledger rows with (the runner's
// kCompositeStrategyVersion); the forecast line scores rows of this version only.
constexpr auto kCompositeVersion = "composite-v2";

// A prediction-ledger HISTORY for `symbol`, written to the shared ledger file before a
// runner reads it: `count` long calls five minutes apart, every one of them right (the
// price steps up 0.1% per row), the LAST call `lastCallMinutesAgo` before now — so a row
// the runner appends at "now" is that call's earliest row at or past the 15-minute
// horizon and RESOLVES it, moving the record's sample count. Tagged `strategyVersion`
// (empty = a row from before the composite carried one). Returned as written (oldest
// first); empty when the file could not be written.
QList<Prediction> seedLedgerFor(const QString &symbol, qint32 count, qint32 lastCallMinutesAgo,
                                const QString &strategyVersion)
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
        p.strategyVersion = strategyVersion;
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

// `count` hourly closes alternating `swingPct` percent below and above `mid` — a series
// whose hourly sigma is about `swingPct` × 2, wide enough for the entry's target (1.5 × a
// 1.5-sigma stop over the 24-hour horizon) to clear the 1% crypto round trip by the 2.5x
// `minEdgeOverCost` asks for: scanRow's gentle noise gives a sigma near 0.17%/h, which
// prices a target worth ~1.5x its costs and is refused `cost-vs-edge` before any later
// rule is reached. Ends on the ABOVE point, so the last close is mid × (1 + swing).
ScreenerRow swingingRow(const QString &symbol, double mid, double swingPct, qsizetype count)
{
    ScreenerRow row;
    row.symbol = symbol;
    row.maxLeverage = 2;
    for (qsizetype i = 0; i < count; ++i) {
        const double sign = ((count - 1 - i) % 2 == 0) ? 1.0 : -1.0;
        row.closes.append(mid * (1.0 + (sign * swingPct / 100.0)));
    }
    row.lastPrice = row.closes.isEmpty() ? 0.0 : row.closes.constLast();
    row.ok = !row.closes.isEmpty();
    return row;
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

// Every ledger row for `symbol`, in the order written — one per scan that evaluated it.
QList<Prediction> rowsFor(const QList<Prediction> &ledger, const QString &symbol)
{
    QList<Prediction> out;
    for (const Prediction &p : ledger) {
        if (p.symbol == symbol) {
            out.append(p);
        }
    }
    return out;
}

// The ledger's rows carrying the composite's own version — the group the forecast scores.
QList<Prediction> compositeRows(const QList<Prediction> &ledger)
{
    QList<Prediction> out;
    for (const Prediction &p : ledger) {
        if (p.strategyVersion == QLatin1String(kCompositeVersion)) {
            out.append(p);
        }
    }
    return out;
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

    //! @tstid TS-BOTSIM-011 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-031, scope=function)
    //
    // The venue's tradeable set never lists a 24/7 instrument, so once the first
    // tradeability poll has answered, the focus/market gate in front of everything else
    // read every coin as `market-closed` — before candidateFor's own 24/7 exemption could
    // run — and no crypto position could open outside the simulation, where that poll
    // never answers. The set is handed to the runner exactly as the client publishes it
    // (the signal is the client's public surface): with only SPX500 listed, BTC is not
    // refused for the market while an index the venue does not list still is.
    void TS_BOTSIM_011_aTwentyFourSevenInstrumentIsNeverMarketClosedByTheVenueSet()
    {
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy decisions(&runner, &BotSimRunner::entryDecision);
        QVERIFY(decisions.isValid());
        runner.setArmed(true);
        emit client.tradeabilityUpdated({QStringLiteral("SPX500")});

        const QString coin = QStringLiteral("BTC");
        const QString index = QStringLiteral("NSDQ100");
        MarketSnapshot snap;
        snap.screenerRows = {scanRow(coin, 60), scanRow(index, 60)};
        snap.intradayBySymbol.insert(coin, scanRow(coin, 30).closes);
        snap.intradayBySymbol.insert(index, scanRow(index, 30).closes);
        runner.onDecisions({buyRow(coin), buyRow(index)}, snap);

        QCOMPARE(decisions.size(), 2);
        QCOMPARE(decisionCodeFor(decisions, index), QStringLiteral("market-closed"));
        const QString coinCode = decisionCodeFor(decisions, coin);
        QVERIFY2(coinCode != QStringLiteral("market-closed"), qPrintable(coinCode));
        QVERIFY2(coinCode != QStringLiteral("<no decision reported>"), qPrintable(coinCode));
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
    // the one new row, because the record is the file, and the copy only follows it. The
    // seeded rows carry the composite's own version: the forecast scores that group alone
    // (TS-BOTSIM-010), and this test is about the copy, not the grouping.
    void TS_BOTSIM_002_theLedgerIsCachedInMemoryAndStaysInStepWithTheFile()
    {
        const QString symbol = QStringLiteral("BTC");
        // 16 minutes ago, not 15: the appended row's elapsed time then reads 16 whole
        // minutes whatever fraction of a minute the scan itself takes — at the horizon,
        // inside its 22-minute limit.
        const QList<Prediction> seeded =
            seedLedgerFor(symbol, 48, 16, QLatin1String(kCompositeVersion));
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
    // restored "armed" flag rather than resuming it — and SAYS so: its deferred restore
    // line never reads "RESUMED ARMED" beside the read-only line naming the holder (the
    // measured console showed both, one contradicting the other) — refuses setArmed(true)
    // with a log line naming the holder AND emits `changed` on the refusal (the GUI's
    // checkable arm button re-reads armed() only on that signal, and stayed pressed), and
    // neither marks, exits nor writes: a config change and a scan whose candle is below an
    // OPEN position's stop leave the position open in its book, the owner's store bytes
    // untouched and the decision, experience and prediction files absent. Every one of
    // those is what deleting the `!m_ownsBook` guard in save() or onDecisions would
    // change. Releasing the first runner lets the next one own the book, so a clean
    // restart is never locked out.
    void TS_BOTSIM_003_aSecondRunnerOnTheSameBookIsReadOnlyAndCannotBeArmed()
    {
        // The store holds one OPEN BTC position (fill 60 000, stop 50 000) and says ARMED —
        // the exit path is only reachable over an open position, and a scan below its
        // stop is exactly what a non-owned runner must NOT act on.
        const QString symbol = QStringLiteral("BTC");
        QVERIFY(seedBookHoldingBtc());
        EtoroClient client(Config{});
        auto first =
            std::make_unique<BotSimRunner>(&client, nullptr, nullptr, QLatin1String(kStore));
        QVERIFY(first->ownsBook());
        QVERIFY(first->armed());
        // A config change saves the book, armed flag and AI mode included — what a second
        // process then RESTORES from the file (the focus set itself is not persisted).
        first->setFocusSymbols({symbol});
        QVERIFY(savedBook().value(QStringLiteral("armed")).toBool());
        QCOMPARE(savedBook().value(QStringLiteral("aiMode")).toInt(),
                 static_cast<int>(BotAiMode::Off));
        const QByteArray ownersBytes = fileBytes(runnerFiles().constFirst());
        QVERIFY(!ownersBytes.isEmpty());

        BotSimRunner second(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy logs(&second, &BotSimRunner::log);
        QSignalSpy decisions(&second, &BotSimRunner::entryDecision);
        QSignalSpy changes(&second, &BotSimRunner::changed);
        QVERIFY(logs.isValid());
        QVERIFY(decisions.isValid());
        QVERIFY(changes.isValid());
        QVERIFY(!second.ownsBook());
        QVERIFY(!second.armed());   // the file said armed; the holder runs it, not this one
        QCOMPARE(second.book().openTrades().size(), 1);   // …but it shows the book

        // The constructor's deferred lines: the restore note and the read-only line,
        // and the first must not claim what the second denies.
        QCoreApplication::processEvents();
        QCOMPARE(logs.size(), 2);
        const QString restored = logs.at(0).at(0).toString();
        QVERIFY2(restored.startsWith(QStringLiteral("BOT SIM books restored")),
                 qPrintable(restored));
        QVERIFY2(!restored.contains(QStringLiteral("RESUMED")), qPrintable(restored));
        QVERIFY2(restored.contains(QStringLiteral("ARMED")),
                 qPrintable(restored));   // the file's fact
        QVERIFY2(restored.contains(QStringLiteral("holder")), qPrintable(restored));
        QVERIFY2(logs.at(1).at(0).toString().contains(QStringLiteral("held")),
                 qPrintable(logs.at(1).at(0).toString()));
        QCOMPARE(changes.size(), 1);
        logs.clear();
        changes.clear();

        second.setArmed(true);
        QVERIFY(!second.armed());
        QCOMPARE(logs.size(), 1);
        const QString refusal = logs.constFirst().at(0).toString();
        QVERIFY2(refusal.contains(QStringLiteral("held")), qPrintable(refusal));
        QVERIFY2(refusal.contains(QStringLiteral("pid %1").arg(QCoreApplication::applicationPid())),
                 qPrintable(refusal));
        QVERIFY(logs.constFirst().at(1).toBool());   // reported as an error, not as chatter
        QCOMPARE(changes.size(), 1);   // the refusal is a state to re-read, not just a line

        // A config change on the viewer stays in the viewer: the console sets the AI
        // mode at start-up, and save() is the one place that must refuse it.
        second.setAiMode(BotAiMode::Lead);
        QCOMPARE(second.book().config().aiMode, BotAiMode::Lead);
        QCOMPARE(fileBytes(runnerFiles().constFirst()), ownersBytes);

        // A scan reaches the read-only views and nothing else: no candidate is evaluated,
        // no ledger row is written, and the candle below the stop closes nothing — the
        // holder marks and exits, and a close booked here would write the owner's
        // decision and experience logs over its own.
        MarketSnapshot snap;
        snap.screenerRows = {scanRow(symbol, 60)};
        snap.intradayBySymbol.insert(symbol, {49500.0, 49000.0});
        second.onDecisions({buyRow(symbol)}, snap);
        QCOMPARE(decisions.size(), 0);
        QCOMPARE(second.book().openTrades().size(), 1);
        QVERIFY(second.book().closedTrades().isEmpty());
        QVERIFY(!QFile::exists(BotSimRunner::ledgerPath()));
        QVERIFY(!QFile::exists(runnerFiles().at(2)));   // decisions log
        QVERIFY(!QFile::exists(runnerFiles().at(3)));   // experience log
        QCOMPARE(fileBytes(runnerFiles().constFirst()), ownersBytes);

        // The lock goes with its holder.
        first.reset();
        const BotSimRunner third(&client, nullptr, nullptr, QLatin1String(kStore));
        QVERIFY(third.ownsBook());
        // …and the second runner does not retroactively become the owner: the lock is
        // tried once, at construction.
        QVERIFY(!second.ownsBook());
    }

    //! @tstid TS-BOTSIM-004 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-037, REQ-F-056, scope=function)
    //
    // The ledger row's "previous five minutes" baseline and the gate's opening-range read
    // come from the instrument's 1-MINUTE series (MarketSnapshot::intradayBySymbol — the
    // series the window's engine reads), not from the eToro scan's HOURLY closes, whose
    // last six points span five hours and whose first thirty are a thirty-hour "range" —
    // and only while that series is LIVE, which the runner can judge one way only: it
    // CHANGED since the previous scan (the snapshot carries no stamp, the producers never
    // clear an entry, and a cash index frozen at the New York close or a sweep that failed
    // both leave a present, plausible, dead series). Three scans of the same book:
    //
    //  1. first sighting — the series is present but not yet known live: the baseline is
    //     0 (unknown), not the sign of a series nobody has seen move;
    //  2. the series grew by five bars — live: hourly rising, 1-minute falling, so a
    //     baseline read off the wrong series shows as the wrong sign, not a coincidence;
    //  3. the identical snapshot again — frozen: the baseline is 0 although the series
    //     itself still falls and the hourly still rises.
    //
    // The row's PRICE is the mid the fill is priced at (sidesFor — for id-less crypto its
    // 1-minute close), never the hourly close while something prices it, and every row
    // carries the composite's version; a symbol with no 1-minute series at all has an
    // unmeasured baseline and the hourly last close. The structure read is pinned in its
    // NEGATIVE form, because the ordered gate chain in front of it (calendar, tradability,
    // the session-phase windows on the exchange clocks) makes a positive
    // `against-range-break` wall-clock dependent: SOL's HOURLY closes break their first-30
    // range DOWN, against the buy, while its live 1-minute series breaks UP — a read off
    // the hourly closes refused it, a read off the 1-minute series never does; and XRP's
    // 1-minute series breaks DOWN against the buy but is FROZEN on scan 3, so a break that
    // is not fresh never refuses either. The rows are written for refused candidates too,
    // so no trade need open.
    void TS_BOTSIM_004_theLedgerBaselineAndPriceComeFromTheOneMinuteSeries()
    {
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy decisions(&runner, &BotSimRunner::entryDecision);
        QVERIFY(decisions.isValid());
        runner.setArmed(true);
        QVERIFY(runner.armed());

        const QString withSession = QStringLiteral("BTC");
        const QString hourlyOnly = QStringLiteral("ETH");
        const QString hourlyAgainst = QStringLiteral("SOL");
        const QString frozenAgainst = QStringLiteral("XRP");
        const QList<DecisionRow> rows = {buyRow(withSession), buyRow(hourlyOnly),
                                         buyRow(hourlyAgainst), buyRow(frozenAgainst)};
        // BTC and ETH hourly RISE over their last six points; SOL's hourly closes break
        // their first-30 range DOWN (60000..59420, last 58820); XRP's break it UP.
        const ScreenerRow btcHourly = hourlyRow(withSession, trend(60000.0, 20.0, 60));
        const ScreenerRow ethHourly = hourlyRow(hourlyOnly, trend(3000.0, 2.0, 60));
        const ScreenerRow solHourly = hourlyRow(hourlyAgainst, trend(60000.0, -20.0, 60));
        const ScreenerRow xrpHourly = hourlyRow(frozenAgainst, trend(2.0, 0.01, 60));
        // The 1-minute series, as a function of how many bars the sweep has delivered so
        // far: BTC falls over its last six points and ends at a price the hourly series
        // never reaches (so the price's source is unambiguous too), SOL breaks its first-30
        // range UP (with the buy), XRP breaks its own DOWN (against it).
        const auto snapshotWith = [&](qsizetype bars) {
            MarketSnapshot snap;
            snap.screenerRows = {btcHourly, ethHourly, solHourly, xrpHourly};
            snap.intradayBySymbol.insert(withSession, trend(62000.0, -5.0, bars));
            snap.intradayBySymbol.insert(hourlyAgainst, trend(60000.0, 5.0, bars));
            snap.intradayBySymbol.insert(frozenAgainst, trend(2.0, -0.002, bars));
            return snap;
        };
        const auto ledgerRows = [&](const QString &symbol) {
            return rowsFor(loadPredictions(BotSimRunner::ledgerPath()), symbol);
        };

        // Scan 1: first sighting — present, not yet known live.
        runner.onDecisions(rows, snapshotWith(40));
        QCOMPARE(ledgerRows(withSession).size(), 1);
        QCOMPARE(ledgerRows(withSession).constFirst().priorMoveDir, 0);

        // Scan 2: the series grew — live.
        decisions.clear();
        const MarketSnapshot live = snapshotWith(45);
        runner.onDecisions(rows, live);
        const QList<Prediction> btc = ledgerRows(withSession);
        QCOMPARE(btc.size(), 2);
        QCOMPARE(btc.at(1).priorMoveDir, -1);   // the 1-minute series fell; the hourly rose
        QCOMPARE(btc.at(1).price, live.intradayBySymbol.value(withSession).constLast());
        QCOMPARE(btc.at(1).strategyVersion, QLatin1String(kCompositeVersion));
        const QList<Prediction> eth = ledgerRows(hourlyOnly);
        QCOMPARE(eth.size(), 2);
        // No 1-minute series at all: unmeasured, not the hourly sign.
        QCOMPARE(eth.at(1).priorMoveDir, 0);
        QCOMPARE(eth.at(1).price, ethHourly.closes.constLast());
        QCOMPARE(eth.at(1).strategyVersion, QLatin1String(kCompositeVersion));
        // The structure read is off the live 1-minute series, which breaks WITH the buy.
        QVERIFY2(decisionCodeFor(decisions, hourlyAgainst) != QStringLiteral("against-range-break"),
                 qPrintable(decisionCodeFor(decisions, hourlyAgainst)));

        // Scan 3: the identical snapshot — frozen, so not live.
        decisions.clear();
        runner.onDecisions(rows, live);
        QCOMPARE(ledgerRows(withSession).size(), 3);
        QCOMPARE(ledgerRows(withSession).at(2).priorMoveDir, 0);   // a stopped series: no baseline
        // XRP's frozen series still reads as a break against the buy — not a fresh one.
        QVERIFY2(decisionCodeFor(decisions, frozenAgainst) != QStringLiteral("against-range-break"),
                 qPrintable(decisionCodeFor(decisions, frozenAgainst)));
    }

    //! @tstid TS-BOTSIM-005 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-031, scope=function)
    //
    // Crypto is actually tradable: its venue id never resolves in this build, so its scan
    // row comes from the public-feed fallback (fromFallbackFeed) and its price from its
    // candles — and neither of the two blockers that used to stop it there may fire. The
    // candle fallback prices it (never `no-live-quote`, TS-BOTSIM-001), and an id-less
    // crypto candidate is no longer refused `instrument-unresolved`. The candidate is built
    // to pass every strategy rule (a series wide enough for its target to clear the 1%
    // crypto round trip — swingingRow — the rest of the chain is silent in simulation: no
    // fee table, no reads, no history in the book), so the open is asserted OUTRIGHT, with
    // the position booked on instrumentId 0. The one gate that reads the wall clock for a
    // 24/7 instrument is the session phase (BTC's calendar is the US one, so the New York
    // opening chaos and the Fed window are sat out); the test computes that phase itself
    // and asserts `volatile-window` exactly in those two windows, never a looser set.
    // Alongside it: the fill is priced off the 1-MINUTE series' last close when there is
    // one (BTC: its hourly and 1-minute last closes differ, the 1-minute one wins — an open
    // filled at the hourly close would start with up to an hour of the coin's move already
    // booked against a 1-minute mark), and off the hourly last close when there is none
    // (ETH); and an id-less NON-crypto candidate (SP.24-7, given a 1-minute series too) is
    // never opened — without the venue id its spread is unknown, so it is refused
    // `no-live-quote` (on Sat/Sun the day gate speaks first: `weekend`); the
    // `instrument-unresolved` refusal behind that is unreachable through a client that
    // keys the spread by the same id, verified by inspection.
    void TS_BOTSIM_005_anIdLessCryptoCandidateOpensOnItsCandleClose()
    {
        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy decisions(&runner, &BotSimRunner::entryDecision);
        QVERIFY(decisions.isValid());
        const QString btc = QStringLiteral("BTC");
        const QString eth = QStringLiteral("ETH");
        const QString index = QStringLiteral("SP.24-7");
        runner.setFocusSymbols({btc, eth, index});
        runner.setArmed(true);
        QVERIFY(runner.armed());
        QCOMPARE(client.instrumentIdFor(btc), qint64(0));   // the premise: no venue id
        QCOMPARE(client.instrumentIdFor(index), qint64(0));

        MarketSnapshot snap;
        ScreenerRow btcRow = swingingRow(btc, 60000.0, 0.5, 60);
        btcRow.fromFallbackFeed = true;
        ScreenerRow ethRow = swingingRow(eth, 3000.0, 0.5, 60);
        ethRow.fromFallbackFeed = true;
        snap.screenerRows = {btcRow, ethRow, swingingRow(index, 5800.0, 0.5, 60)};
        const QList<double> btcSession = trend(59000.0, 1.0, 30);   // ends at 59029
        snap.intradayBySymbol.insert(btc, btcSession);
        snap.intradayBySymbol.insert(index, trend(5790.0, 0.1, 30));
        QVERIFY(!qFuzzyCompare(btcSession.constLast(), btcRow.closes.constLast()));
        const QDateTime now = QDateTime::currentDateTime();
        const SessionPhase phase = sessionPhaseFor(btc, now);
        const bool satOut =
            (phase == SessionPhase::OpeningChaos) || (phase == SessionPhase::PolicyWindow);
        runner.onDecisions({buyRow(btc), buyRow(eth), buyRow(index)}, snap);

        QCOMPARE(decisions.size(), 3);
        const auto openTrade = [&runner](const QString &symbol) {
            const QList<PaperTrade> &open = runner.book().openTrades();
            return std::find_if(open.cbegin(), open.cend(),
                                [&symbol](const PaperTrade &t) { return t.symbol == symbol; });
        };
        const auto notOpen = [&openTrade, &runner](const QString &symbol) {
            return openTrade(symbol) == runner.book().openTrades().cend();
        };
        // The non-crypto candidate never opens: refused before any geometry, for the spread
        // the venue never priced (the day gate outranks that on a weekend).
        const QString indexCode = decisionCodeFor(decisions, index);
        QCOMPARE(indexCode, (now.date().dayOfWeek() > 5) ? QStringLiteral("weekend")
                                                         : QStringLiteral("no-live-quote"));
        QVERIFY(notOpen(index));
        if (satOut) {
            QCOMPARE(decisionCodeFor(decisions, btc), QStringLiteral("volatile-window"));
            QCOMPARE(decisionCodeFor(decisions, eth), QStringLiteral("volatile-window"));
            QVERIFY(runner.book().openTrades().isEmpty());
            return;
        }
        QCOMPARE(decisionCodeFor(decisions, btc), QStringLiteral("opened"));
        QCOMPARE(decisionCodeFor(decisions, eth), QStringLiteral("opened"));
        QCOMPARE(runner.book().openTrades().size(), 2);
        QVERIFY(!notOpen(btc));
        QCOMPARE(openTrade(btc)->instrumentId, qint64(0));
        QVERIFY(openTrade(btc)->isBuy);
        // The 1-minute close priced the fill (the mid; the half-spread is a separate line).
        QCOMPARE(openTrade(btc)->openRate, btcSession.constLast());
        QVERIFY(!notOpen(eth));
        QCOMPARE(openTrade(eth)->instrumentId, qint64(0));
        QCOMPARE(openTrade(eth)->openRate, ethRow.closes.constLast());   // no 1-minute series
        // The ledger has both rows as TAKEN, priced at the same fill. (The list is a named
        // local: rowFor returns a pointer INTO it, which a temporary would free.)
        const QList<Prediction> ledger = loadPredictions(BotSimRunner::ledgerPath());
        const Prediction *btcRowInLedger = rowFor(ledger, btc);
        QVERIFY(btcRowInLedger != nullptr);
        QVERIFY(btcRowInLedger->taken);
        QCOMPARE(btcRowInLedger->refusal, QString{});
        QCOMPARE(btcRowInLedger->price, btcSession.constLast());
        const Prediction *ethRowInLedger = rowFor(ledger, eth);
        QVERIFY(ethRowInLedger != nullptr);
        QVERIFY(ethRowInLedger->taken);
        QCOMPARE(ethRowInLedger->price, ethRow.closes.constLast());
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

    //! @tstid TS-BOTSIM-009 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, scope=function)
    //
    // The book's lock is never judged stale by AGE. QLockFile's default stale time (30 s)
    // treats a lock file older than that as stale even when the pid it names is alive
    // and matches the recorded application, and nothing refreshes the file while it is
    // held — so a bot that has run for an hour would have had its book taken by the next
    // process to look at it (measured: refused while fresh, taken over at 45 s old). The
    // runner therefore turns the age rule off, and what decides staleness is the holder:
    // a lock naming a LIVE pid with this application's name, a minute old and held by no
    // native lock at all (the file written back after its holder released it — a holder
    // on a filesystem that does not enforce flock looks exactly like this), is refused,
    // the runner reports the holder and the file keeps its old modification time; the
    // same file naming a pid that cannot be running is freed at once and the new runner
    // owns the book with its own pid in the lock. Both halves fail under the default:
    // the first is stolen, and the second is what "never stale" must not cost.
    void TS_BOTSIM_009_anOldLockOfALiveHolderIsRefusedWhileADeadHoldersIsFreed()
    {
        const QString lockPath = runnerFiles().at(1);
        EtoroClient client(Config{});
        QByteArray lockBytes;
        {
            const BotSimRunner holder(&client, nullptr, nullptr, QLatin1String(kStore));
            QVERIFY(holder.ownsBook());
            lockBytes = fileBytes(lockPath);   // this process's pid, name, host, boot id
            QVERIFY(!lockBytes.isEmpty());
        }
        QVERIFY(!QFile::exists(lockPath));   // released with its holder

        // The same lock, a minute old, with no process holding it natively.
        const QDateTime aMinuteAgo = QDateTime::currentDateTimeUtc().addSecs(-60);
        QVERIFY(writeLockFile(lockPath, lockBytes, aMinuteAgo));
        {
            const BotSimRunner viewer(&client, nullptr, nullptr, QLatin1String(kStore));
            QSignalSpy logs(&viewer, &BotSimRunner::log);
            QVERIFY(!viewer.ownsBook());
            QCoreApplication::processEvents();
            QCOMPARE(logs.size(), 1);
            const QString line = logs.constFirst().at(0).toString();
            QVERIFY2(
                line.contains(QStringLiteral("pid %1").arg(QCoreApplication::applicationPid())),
                qPrintable(line));
        }
        // Refused means untouched: a takeover rewrites the file, which is then fresh.
        QVERIFY(QFileInfo(lockPath).lastModified() < aMinuteAgo.addSecs(30));

        // The same file naming a pid nothing runs under: stale by the pid rule alone.
        QByteArray deadBytes = lockBytes;
        deadBytes.replace(0, deadBytes.indexOf('\n'), QByteArray::number(kNoSuchPid));
        QVERIFY(writeLockFile(lockPath, deadBytes, aMinuteAgo));
        const BotSimRunner successor(&client, nullptr, nullptr, QLatin1String(kStore));
        QVERIFY(successor.ownsBook());
        QCOMPARE(fileBytes(lockPath), lockBytes);   // rewritten by the new holder: us
    }

    //! @tstid TS-BOTSIM-010 @design DES-UI-BOTSIM
    // @relation(REQ-F-029, REQ-F-037, scope=function)
    //
    // The forecast line scores ONLY the rows carrying the composite's own strategy version.
    // Rows from before the version existed (empty) were priced off another series and carry
    // a five-HOUR baseline; "they score as their own group" was the field's contract, but
    // the runner was the one consumer and it filtered by symbol alone — so an old row paired
    // against a new row's price from a different feed, and its baseline was counted in the
    // five-minute one's hit rate. Seeded: 48 untagged calls five minutes apart, the last
    // 16 minutes ago, which the row the scan appends at "now" would resolve if it were
    // scored with them (TS-BOTSIM-002's premise, over tagged rows). The line must then say
    // what the composite's rows ALONE say — one row, no claim yet — and not what the mixed
    // file says, which is a calibrated hit rate over 46 samples.
    void TS_BOTSIM_010_theForecastScoresOnlyTheRowsOfTheCompositesOwnVersion()
    {
        const QString symbol = QStringLiteral("BTC");
        const QList<Prediction> untagged = seedLedgerFor(symbol, 48, 16, QString{});
        QCOMPARE(untagged.size(), 48);

        EtoroClient client(Config{});
        BotSimRunner runner(&client, nullptr, nullptr, QLatin1String(kStore));
        QSignalSpy logs(&runner, &BotSimRunner::log);
        QVERIFY(logs.isValid());
        runner.setArmed(true);
        MarketSnapshot snap;
        snap.screenerRows = {scanRow(symbol, 60)};
        snap.intradayBySymbol.insert(symbol, scanRow(symbol, 30).closes);
        runner.onDecisions({buyRow(symbol)}, snap);

        const QList<Prediction> file = loadPredictions(BotSimRunner::ledgerPath());
        QCOMPARE(file.size(), untagged.size() + 1);
        const QList<Prediction> composite = compositeRows(file);
        QCOMPARE(composite.size(), 1);
        // The premise: mixed, the new row resolves the seeded history's last call and the
        // record is a calibrated claim; alone, it is one call and no claim at all.
        const HorizonScore mixed = scoreHorizon(file, Horizon::M15);
        QVERIFY(mixed.trustworthy());
        QVERIFY(mixed.samples > scoreHorizon(untagged, Horizon::M15).samples);
        const HorizonScore own = scoreHorizon(composite, Horizon::M15);
        QCOMPARE(own.samples, 0);

        QCOMPARE(forecastLinesFor(logs, symbol).size(), 1);
        const QString record = recordPartOf(forecastLinesFor(logs, symbol).constFirst());
        QCOMPARE(record, own.headline());
        QVERIFY(record != mixed.headline());
    }
};

QTEST_GUILESS_MAIN(TestBotSimRunner)
#include "tst_botsimrunner.moc"
