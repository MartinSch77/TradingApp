// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// TradingAdvise <INSTRUMENT> — one verdict with its reasons, on stdout (REQ-F-047).
//
//   TradingAdvise SPX500              # gather (bounded), judge, print, exit
//   TradingAdvise BTC --no-ai         # skip the local model
//   TradingAdvise GOLD --timeout 45   # gathering bound in seconds (default 30)
//
// Exit codes: 0 = proposal, 2 = reasoned no-trade, 3 = not enough data, 64 = usage.
// ADVISORY BY CONSTRUCTION: this binary links the read paths only — there is no code path
// from here to an order endpoint, and the report says so on every run.

#include "console/AdviseView.h"
#include "console/ScanBooks.h"
#include "domain/CrowdInference.h"
#include "domain/PaperTrader.h"
#include "domain/IndexConfluence.h"
#include "domain/InstrumentCatalog.h"
#include "domain/TradePlan.h"
#include "services/Config.h"
#include "services/CrowdCollector.h"
#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"
#include "services/OllamaAdvisor.h"
#include "ui/BotSimRunner.h"

#include <QCoreApplication>
#include <QFile>
#include <QNetworkProxy>
#include <QDateTime>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

#include <cstdio>

#include <functional>

namespace {

using trading::console::AdviseInput;
using trading::console::ScanBooks;

// The nine index reads as report lines — only the two indices have them; anything else says
// so once rather than listing nine unknowns that could not apply.
QStringList readLinesFor(const QString &symbol, const trading::MarketSnapshot &snap)
{
    if ((symbol != QLatin1String("SPX500")) && (symbol != QLatin1String("NSDQ100"))) {
        return {QStringLiteral("index reads: not applicable to %1").arg(symbol)};
    }
    const trading::ReadInputs inputs =
        trading::readInputsFor(symbol, snap.referenceSeries, snap.referenceVolumes,
                               snap.intradayBySymbol);
    const trading::IndexReads reads = trading::indexReads(symbol, inputs);
    const auto line = [](const char *name, const trading::Read &r) {
        if (!r.known) {
            return QStringLiteral("%1: unknown").arg(QLatin1String(name));
        }
        const QString dir = r.dir > 0   ? QStringLiteral("supports LONG")
                            : r.dir < 0 ? QStringLiteral("supports SHORT")
                                        : QStringLiteral("neutral");
        return QStringLiteral("%1: %2 — %3").arg(QLatin1String(name), dir, r.detail);
    };
    return {line("futures lead", reads.futuresLead),
            line("futures momentum", reads.futuresMomentum),
            line("volatility", reads.volatility),
            line("yields", reads.yields),
            line("curve", reads.curve),
            line("participation", reads.participation),
            line("above VWAP", reads.aboveVwap),
            line("up/down volume", reads.upDownVolume),
            line("opening range", reads.structure)};
}

trading::PlanInput planInputFor(const ScreenerRow &row, const ScanBooks &books,
                                const EtoroClient &client)
{
    trading::PlanInput in;
    in.closes = row.closes;
    in.price = row.lastPrice;
    in.invest = 1000.0;   // a stake for the geometry; the amounts scale linearly with it
    in.maxLeverage = row.maxLeverage;
    const trading::InstrumentSpec *spec = trading::instrumentSpec(row.symbol);
    if (spec != nullptr) {
        in.leverageSteps = spec->simLeverage;
    }
    in.spreadPct = client.spreadPctFor(row.symbol);
    in.now = QDateTime::currentDateTimeUtc();
    in.vixValid = books.vixValid;
    in.vix = books.vix;
    in.vixChangePct = books.vixChange;
    in.fgValid = books.fgValid;
    in.fearGreed = books.fearGreed;
    return in;
}

// For an index: its top-ten constituents' session moves, worst-first (so the drag shows at a
// glance). Empty for a non-index or when the reference series have not arrived — an absent
// constituent is dropped, never shown as a flat 0%.
QStringList heavyLinesFor(const QString &symbol, const ScanBooks &books)
{
    if ((symbol != QLatin1String("SPX500")) && (symbol != QLatin1String("NSDQ100"))) {
        return {};
    }
    QList<QPair<double, QString>> moves;
    for (const QString &name : trading::indexHeavyweights(symbol)) {
        const QList<double> series = books.reference.value(name);
        if ((series.size() >= 2) && (series.constFirst() > 0.0)) {
            const double pct =
                ((series.constLast() - series.constFirst()) / series.constFirst()) * 100.0;
            moves.append({pct, name});
        }
    }
    std::sort(moves.begin(), moves.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    QStringList out;
    for (const auto &m : moves) {
        out << QStringLiteral("%1 %2%").arg(m.second).arg(m.first, 0, 'f', 2);
    }
    return out;
}

struct AdviseArgs {
    QString symbol;
    bool askAi = true;
    bool verbose = false;
    bool help = false;
    // The DEFAULT is the whole point of the tool: keep running, give advice AND simulate
    // trading THIS one instrument. --once is the scriptable single verdict; --watch reports
    // continuously without the sim bot.
    bool once = false;
    bool watchOnly = false;
    qint32 timeoutSec = 90;
    qint32 intervalSec = 60;
    [[nodiscard]] bool watch() const { return !once; }   // continuous unless --once
    [[nodiscard]] bool trade() const { return !once && !watchOnly; }
};

AdviseArgs parseArguments(const QStringList &args)
{
    AdviseArgs out;
    for (qsizetype i = 1; i < args.size(); ++i) {
        if ((args.at(i) == QLatin1String("--help")) || (args.at(i) == QLatin1String("-h"))) {
            out.help = true;
        } else if (args.at(i) == QLatin1String("--no-ai")) {
            out.askAi = false;
        } else if (args.at(i) == QLatin1String("--verbose")) {
            out.verbose = true;
        } else if (args.at(i) == QLatin1String("--once")) {
            out.once = true;
        } else if (args.at(i) == QLatin1String("--watch")) {
            out.watchOnly = true;   // report continuously, but do NOT simulate trades
        } else if (args.at(i) == QLatin1String("--trade")) {
            // Explicit form of the default; accepted so old invocations keep working.
        } else if ((args.at(i) == QLatin1String("--timeout")) && (i + 1 < args.size())) {
            out.timeoutSec = args.at(++i).toInt();
        } else if ((args.at(i) == QLatin1String("--interval")) && (i + 1 < args.size())) {
            out.intervalSec = args.at(++i).toInt();
        } else if (!args.at(i).startsWith(QLatin1Char('-')) && out.symbol.isEmpty()) {
            out.symbol = args.at(i).toUpper();
        }
    }
    return out;
}

void printUsage()
{
    std::fprintf(
        stderr,
        "TradingAdvise <INSTRUMENT> — keeps running, gives advice AND simulates trading THAT\n"
        "one instrument (paper money on live prices, its own book; advisory by construction —\n"
        "this binary links no order path and can place nothing real).\n\n"
        "usage: TradingAdvise <INSTRUMENT> [options]\n\n"
        "  <INSTRUMENT>      an eToro app symbol from the catalog (see the list below)\n"
        "  (default)         keep running: each --interval, print the decision with the\n"
        "                    index's live top-ten constituents AND let the focused sim bot\n"
        "                    act on it (opens/closes/refusals printed as [bot] lines)\n"
        "  --once            a single verdict then exit (scriptable; no sim bot)\n"
        "  --watch           keep running and report, but do NOT simulate trades\n"
        "  --no-ai           skip the local model in the printed advice (the sim bot still\n"
        "                    uses the configured AI mode)\n"
        "  --interval <sec>  re-decide period (default 60)\n"
        "  --timeout <sec>   one-shot gathering bound for --once (default 90)\n"
        "  --verbose         mirror the client/feed logs to stderr\n"
        "  --help, -h        this text\n\n"
        "exit codes (--once): 0 = proposal, 2 = no-trade, 3 = not enough data, 64 = usage\n\n"
        "instruments: %s\n",
        qPrintable(trading::tradableSymbols().join(QStringLiteral(" "))));
}

void wireVerboseTaps(const EtoroClient &client, QObject *context)
{
    QObject::connect(&client, &EtoroClient::log, context, [](const QString &line, bool) {
        std::fprintf(stderr, "[client] %s\n", qPrintable(line));
    });
    QObject::connect(&client, &EtoroClient::screenerProgress, context, [](int done, int total) {
        std::fprintf(stderr, "[scan] %d/%d\n", done, total);
    });
    QObject::connect(&client, &EtoroClient::screenerRow, context, [](const ScreenerRow &row) {
        std::fprintf(stderr, "[row] %s ok=%d closes=%d\n", qPrintable(row.symbol),
                     row.ok ? 1 : 0, static_cast<int>(row.closes.size()));
    });
}

// Kick every fetch and pump the event loop until the essentials are in the books or the
// deadline speaks. The timeout ends the WAIT, not the truth: absents stay absent.
void runGather(EtoroClient &client, MarketFeeds &feeds, ScanBooks *books,
               const AdviseArgs &args)
{
    // The connect comes FIRST: an id-less first scan finishes SYNCHRONOUSLY inside the
    // kick, and a signal emitted before its connect exists is a wait that never ends.
    auto *scanDone = new bool(false);
    auto *pollCount = new int(0);
    QObject::connect(&client, &EtoroClient::screenerFinished, &feeds,
                     [scanDone] { *scanDone = true; });
    client.start();
    client.scanInstruments();
    feeds.fetchInstrumentRatings();
    feeds.fetchInstrumentNews();
    feeds.fetchIntradaySeries();
    feeds.fetchReferenceSeries();
    feeds.start(60 * 1000);   // the periodic tick fetches VIX/F&G/quote on its first pass

    QEventLoop wait;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.start(args.timeoutSec * 1000);
    QObject::connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &wait, [&] {
        const bool haveRow = std::any_of(books->rows.cbegin(), books->rows.cend(),
                                         [&args](const ScreenerRow &r) {
                                             return r.symbol == args.symbol;
                                         });
        if (*scanDone && haveRow && books->vixValid && !books->reference.isEmpty()) {
            wait.quit();
        }
        // The FIRST scan always runs before the instrument ids resolve and delivers nothing
        // (it nudges resolution and asks for a retry) — retry until the row arrives or the
        // deadline speaks. GENTLY: every scan kick re-nudges the unresolved id searches, and
        // those live on the small shared rate pool — a tight retry loop 429-throttles the
        // very resolution it is waiting for (measured: one id resolved in 95 s of hammering).
        ++*pollCount;
        if (*scanDone && !haveRow && ((*pollCount % 10) == 0)) {
            *scanDone = false;
            client.scanInstruments();
        }
    });
    poll.start(500);
    wait.exec();
}

// Everything except the (slow, optional) local model: the composite row, the costed plan,
// the reads, the crowd knowledge and the named absents.
AdviseInput judgeBooks(const ScanBooks &books, const QString &symbol, const EtoroClient &client,
                       const Config &cfg)
{
    const trading::MarketSnapshot snap = trading::console::snapshotFrom(books);
    AdviseInput in;
    in.symbol = symbol;
    for (const trading::DecisionRow &row : trading::computeDecisionRows(snap)) {
        if (row.symbol == symbol) {
            in.haveRow = true;
            in.row = row;
        }
    }
    for (const ScreenerRow &row : books.rows) {
        if ((row.symbol == symbol) && !row.closes.isEmpty()) {
            in.havePlan = true;
            in.plan = trading::buildTradePlan(planInputFor(row, books, client));
            in.price = row.lastPrice;
        }
    }
    in.readLines = readLinesFor(symbol, snap);
    in.heavyLines = heavyLinesFor(symbol, books);
    in.vixValid = books.vixValid;
    in.vix = books.vix;
    in.fgValid = books.fgValid;
    in.fearGreed = books.fearGreed;

    // The crowd subsystem's persisted knowledge — read-only, the same store the GUI writes.
    trading::crowd::CrowdCollector crowd(
        cfg,
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/crowd.db"));
    trading::crowd::CrowdPrediction prediction;
    if (crowd.model().ready()) {
        prediction = crowd.model().predict(
            crowd.modelFeaturesFor(symbol, QDateTime::currentDateTimeUtc()));
    }
    in.crowdLine = trading::crowd::crowdEvidenceLine(symbol, crowd.store().latestScore(symbol),
                                                     prediction);

    if (!in.haveRow) {
        in.absentSources << QStringLiteral("price series / scan row");
    }
    if (books.ratings.isEmpty()) {
        in.absentSources << QStringLiteral("web ratings");
    }
    if (!books.news.contains(symbol)) {
        in.absentSources << QStringLiteral("news headlines");
    }
    return in;
}

// One bounded ask of the local model; its answer for OUR instrument, or the named failure.
QString aiLineFor(const Config &cfg, const ScanBooks &books, const QString &symbol)
{
    const trading::MarketSnapshot snap = trading::console::snapshotFrom(books);
    const QList<trading::DecisionRow> rows = trading::computeDecisionRows(snap);
    OllamaAdvisor ollama(cfg.ollamaHost, cfg.ollamaModel);
    QString aiLine = QStringLiteral("no usable answer");
    QEventLoop wait;
    QObject::connect(
        &ollama, &OllamaAdvisor::proposalsReady, &wait,
        [&](const QList<AiDecision> &picks, const QString &error) {
            for (const AiDecision &p : picks) {
                // The model names instruments loosely (BTCUSDT, "SPX500 composite"); the
                // same resolution the bot trusts decides whether this pick is OURS.
                if (trading::matchProposalSymbol(p.symbol, trading::tradableSymbols())
                    == symbol) {
                    aiLine = QStringLiteral("%1 (%2%)%3")
                                 .arg(p.action)
                                 .arg(p.confidence, 0, 'f', 0)
                                 .arg(p.rationale.isEmpty()
                                          ? QString()
                                          : QStringLiteral(" — ") + p.rationale);
                }
            }
            if (!error.isEmpty()) {
                aiLine = error;
            }
            wait.quit();
        });
    QTimer::singleShot(90 * 1000, &wait, &QEventLoop::quit);
    ollama.requestDecision(trading::buildDecisionEvidence(rows, snap, 6));
    wait.exec();
    return aiLine;
}

// One report cycle: judge the current books, print with a timestamp, and (in trade mode) hand
// the decisions to the focused runner so it acts on exactly the verdict just printed.
struct CycleContext {
    const ScanBooks *books = nullptr;
    EtoroClient *client = nullptr;
    BotSimRunner *runner = nullptr;   // null in --watch, present in --trade
    QString botAiLine;   // the bot's own model pick, mirrored (trade mode)
};

void reportCycle(const CycleContext &ctx, const AdviseArgs &args, const Config &cfg)
{
    const ScanBooks *books = ctx.books;
    const EtoroClient &client = *ctx.client;
    BotSimRunner *runner = ctx.runner;
    const QString &botAiLine = ctx.botAiLine;
    AdviseInput in = judgeBooks(*books, args.symbol, client, cfg);
    if (runner != nullptr) {
        // Trade mode: SHOW the bot's own model pick rather than make a second, redundant
        // Ollama call — the bot already asked, on the same evidence, and a second call would
        // block the report for up to the model's timeout each cycle.
        if (!botAiLine.isEmpty()) {
            in.aiAsked = true;
            in.aiLine = botAiLine;
        }
    } else if (args.askAi && !cfg.ollamaModel.isEmpty() && in.haveRow) {
        in.aiAsked = true;
        in.aiLine = aiLineFor(cfg, *books, args.symbol);
    }
    const trading::console::AdviseVerdict verdict = trading::console::adviseReport(in);
    std::fprintf(stdout, "==== %s ====\n%s\n",
                 qPrintable(QDateTime::currentDateTime().toString(Qt::ISODate)),
                 qPrintable(verdict.text));
    std::fflush(stdout);
    if (runner != nullptr) {
        const trading::MarketSnapshot snap = trading::console::snapshotFrom(*books);
        runner->onDecisions(trading::computeDecisionRows(snap), snap);
    }
}

// The continuous path for --watch / --trade: kick the feeds and an initial scan, then on each
// scan completion report a cycle, and re-scan every --interval seconds. Runs until killed.
struct WatchContext {
    EtoroClient *client = nullptr;
    MarketFeeds *feeds = nullptr;
    ScanBooks *books = nullptr;
    BotSimRunner *runner = nullptr;   // null in --watch, present in --trade
    std::function<void(const QString &)> *tee = nullptr;   // stdout + session log (trade only)
};

int runContinuous(const WatchContext &ctx, const AdviseArgs &args, const Config &cfg)
{
    EtoroClient &client = *ctx.client;
    MarketFeeds &feeds = *ctx.feeds;
    ScanBooks *books = ctx.books;
    BotSimRunner *runner = ctx.runner;
    // The bot's most recent model pick for the focus symbol, mirrored into the report so the
    // AI decision is shown without a second Ollama call.
    auto *botAiLine = new QString();
    if (runner != nullptr) {
        const QString focus = args.symbol;
        QObject::connect(runner, &BotSimRunner::proposalsUpdated, &client,
                         [botAiLine, focus](const QList<trading::AiProposal> &picks) {
                             for (const trading::AiProposal &p : picks) {
                                 if (p.resolvedSymbol != focus) {
                                     continue;
                                 }
                                 *botAiLine = QStringLiteral("%1 (%2%)%3")
                                     .arg(p.exitNow ? QStringLiteral("CLOSE")
                                          : p.dir > 0 ? QStringLiteral("BUY")
                                          : p.dir < 0 ? QStringLiteral("SELL")
                                                      : QStringLiteral("HOLD"))
                                     .arg(p.confidence, 0, 'f', 0)
                                     .arg(p.rationale.isEmpty()
                                              ? QString()
                                              : QStringLiteral(" — ") + p.rationale);
                             }
                         });
        // The running P/L every 10 s, independent of the scan interval — the runner marks
        // its open positions on its own tick, so stats() is always current.
        auto *pnl = new QTimer(&client);
        std::function<void(const QString &)> *tee = ctx.tee;
        QObject::connect(pnl, &QTimer::timeout, &client, [runner, tee] {
            const trading::PaperStats s = runner->stats();
            const QString line =
                QStringLiteral("[P/L] equity %1 EUR · realised %2 · open %3 · %4 open / "
                               "%5 closed · costs %6")
                    .arg(s.equity, 0, 'f', 2)
                    .arg(s.realized, 0, 'f', 2)
                    .arg(s.openPnl, 0, 'f', 2)
                    .arg(s.openTrades)
                    .arg(s.closedTrades)
                    .arg(s.costsPaid, 0, 'f', 2);
            if (tee != nullptr) {
                (*tee)(line);
            } else {
                std::fprintf(stdout, "%s\n", qPrintable(line));
                std::fflush(stdout);
            }
        });
        pnl->start(10 * 1000);
    }
    const auto rescan = [&client, &feeds] {
        client.scanInstruments();
        feeds.fetchInstrumentRatings();
        feeds.fetchInstrumentNews();
        feeds.fetchIntradaySeries();
        feeds.fetchReferenceSeries();
    };
    QObject::connect(&client, &EtoroClient::screenerFinished, &client, [&, books, runner] {
        const bool haveRow =
            std::any_of(books->rows.cbegin(), books->rows.cend(),
                        [&args](const ScreenerRow &r) { return r.symbol == args.symbol; });
        if (haveRow) {
            reportCycle({books, &client, runner, *botAiLine}, args, cfg);
        } else {
            // Ids not resolved yet. The id-less scan emits screenerFinished
            // SYNCHRONOUSLY, so re-scanning inline here recurses until the
            // stack blows — defer to the event loop instead.
            QTimer::singleShot(2000, &client, [&client] { client.scanInstruments(); });
        }
    });
    // Order matters and is proven by the one-shot path: start the client and kick its scan
    // BEFORE starting the feeds. Starting MarketFeeds first left the client's first
    // proxy/QNAM setup half-initialised and segfaulted in proxyForQuery on that first scan.
    client.start();
    client.scanInstruments();
    feeds.start(60 * 1000);
    feeds.fetchInstrumentRatings();
    feeds.fetchInstrumentNews();
    feeds.fetchIntradaySeries();
    feeds.fetchReferenceSeries();
    auto *timer = new QTimer(&client);
    QObject::connect(timer, &QTimer::timeout, &client, rescan);
    timer->start(args.intervalSec * 1000);
    return QCoreApplication::exec();
}

} // namespace

struct TradeSession {
    BotSimRunner *runner = nullptr;
    std::function<void(const QString &)> *tee = nullptr;
};

// Build the focused paper bot for --trade: its OWN book (never the main bot's botsim.json),
// scoped to the one instrument, armed, with a session log every trade/decision is teed into.
// SIMULATED money on live prices — no order path, like everything here.
TradeSession makeTradeSession(EtoroClient &client, const Config &cfg, const AdviseArgs &args,
                              QCoreApplication &app)
{
    TradeSession s;
    auto *ai = new OllamaAdvisor(cfg.ollamaHost, cfg.ollamaModel, &app);
    s.runner = new BotSimRunner(&client, ai, &app,
                                QStringLiteral("advise-botsim-%1.json").arg(args.symbol));
    s.runner->setFocusSymbols({args.symbol});
    s.runner->setArmed(true);
    const QString logPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                            + QStringLiteral("/advise-botsim-%1-session.log").arg(args.symbol);
    auto *logFile = new QFile(logPath, &app);
    static_cast<void>(logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text));
    s.tee = new std::function<void(const QString &)>([logFile](const QString &line) {
        std::fprintf(stdout, "%s\n", qPrintable(line));
        std::fflush(stdout);
        if (logFile->isOpen()) {
            logFile->write(QStringLiteral("%1  %2\n")
                               .arg(QDateTime::currentDateTime().toString(Qt::ISODate), line)
                               .toUtf8());
            logFile->flush();
        }
    });
    std::fprintf(stdout, "session log: %s\n", qPrintable(logPath));
    auto *tee = s.tee;
    QObject::connect(s.runner, &BotSimRunner::log, &app,
                     [tee](const QString &line, bool) { (*tee)(QStringLiteral("[bot] ") + line); });
    // The decision itself, for the ONE traded instrument (proxy not-focus refusals are noise).
    const QString focus = args.symbol;
    QObject::connect(
        s.runner, &BotSimRunner::entryDecision, &app,
        [focus, tee](const QString &symbol, bool traded, const QString &code, const QString &why) {
            if (symbol == focus) {
                (*tee)(QStringLiteral(">>> DECISION %1: %2 (%3) — %4")
                           .arg(symbol,
                                traded ? QStringLiteral("TRADED") : QStringLiteral("refused"), code,
                                why));
            }
        });
    std::fprintf(stdout,
                 "SIMULATED trading of %s — paper money on live prices, own book (%s). "
                 "No real order is ever placed.\n",
                 qPrintable(args.symbol), qPrintable(s.runner->storePath()));
    return s;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // The SAME identity as the GUIs: the config, the crowd store and the model paths live in
    // the shared per-app directory, so this console reads the very books the app writes.
    QCoreApplication::setOrganizationName(QStringLiteral("TradingApp"));
    QCoreApplication::setApplicationName(QStringLiteral("eToro Trader"));
    // Headless tool talking only to known public hosts: an EXPLICIT no-proxy, which makes
    // every QNAM skip the proxy-FACTORY query path altogether. setUseSystemConfiguration(false)
    // was not enough — proxyForQuery was still invoked and segfaulted in libproxy on the
    // first scan; an application proxy set to NoProxy is never routed through the factory.
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    const AdviseArgs args = parseArguments(QCoreApplication::arguments());
    if (args.help || args.symbol.isEmpty()
        || !trading::tradableSymbols().contains(args.symbol)) {
        printUsage();
        return args.help ? 0 : 64;
    }

    const Config cfg = Config::load();
    EtoroClient client(cfg);
    MarketFeeds feeds;
    // The CLIENT scans ONLY the instrument itself — that is what produces the [row] the user
    // watches and the candle series the plan needs. The two index futures proxies are NOT
    // scanned: the futures-lead/momentum reads take their series from the intraday FEED
    // (Yahoo, client-independent), so the FEEDS set keeps them for an index while the scan
    // stays about the one instrument. Drop them from the feeds too and those reads go
    // silently UNKNOWN — the trap TS-CONF-006 pins.
    client.setTradableSymbols({args.symbol});
    QStringList feedSet{args.symbol};
    if ((args.symbol == QLatin1String("SPX500")) || (args.symbol == QLatin1String("NSDQ100"))) {
        feedSet << QStringLiteral("SP.24-7") << QStringLiteral("NSDQ100.24-7");
    }
    feedSet.removeDuplicates();
    feeds.setTradableSymbols(feedSet);
    auto *books = new ScanBooks();
    trading::console::wireScanBooks(client, feeds, books, &app);
    if (args.verbose) {
        wireVerboseTaps(client, &app);
    }

    if (args.watch()) {
        const TradeSession session =
            args.trade() ? makeTradeSession(client, cfg, args, app) : TradeSession{};
        return runContinuous({&client, &feeds, books, session.runner, session.tee}, args, cfg);
    }

    // One-shot.
    runGather(client, feeds, books, args);
    AdviseInput in = judgeBooks(*books, args.symbol, client, cfg);
    if (args.askAi && !cfg.ollamaModel.isEmpty() && in.haveRow) {
        in.aiAsked = true;
        in.aiLine = aiLineFor(cfg, *books, args.symbol);
    }
    const trading::console::AdviseVerdict verdict = trading::console::adviseReport(in);
    std::fputs(qPrintable(verdict.text), stdout);
    return verdict.exitCode;
}
