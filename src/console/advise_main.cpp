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

#include <QCoreApplication>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

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
                                EtoroClient &client)
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


struct AdviseArgs {
    QString symbol;
    bool askAi = true;
    bool verbose = false;
    bool help = false;
    qint32 timeoutSec = 90;
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
        } else if ((args.at(i) == QLatin1String("--timeout")) && (i + 1 < args.size())) {
            out.timeoutSec = args.at(++i).toInt();
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
        "TradingAdvise — one verdict with its reasons for ONE instrument (advisory only;\n"
        "this binary links no order path and can place nothing).\n\n"
        "usage: TradingAdvise <INSTRUMENT> [options]\n\n"
        "  <INSTRUMENT>      an eToro app symbol from the catalog (see the list below)\n"
        "  --no-ai           skip the local model's pick (faster; the other sources stay)\n"
        "  --timeout <sec>   bound on the data gathering (default 90; whatever has not\n"
        "                    arrived by then is reported ABSENT, never zeroed)\n"
        "  --verbose         mirror the client/feed logs to stderr while gathering\n"
        "  --help, -h        this text\n\n"
        "exit codes: 0 = proposal, 2 = reasoned no-trade, 3 = not enough data, 64 = usage\n\n"
        "instruments: %s\n",
        qPrintable(trading::tradableSymbols().join(QStringLiteral(" "))));
}

void wireVerboseTaps(EtoroClient &client, QObject *context)
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
AdviseInput judgeBooks(const ScanBooks &books, const QString &symbol, EtoroClient &client,
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


} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // The SAME identity as the GUIs: the config, the crowd store and the model paths live in
    // the shared per-app directory, so this console reads the very books the app writes.
    QCoreApplication::setOrganizationName(QStringLiteral("TradingApp"));
    QCoreApplication::setApplicationName(QStringLiteral("eToro Trader"));

    const AdviseArgs args = parseArguments(QCoreApplication::arguments());
    if (args.help || args.symbol.isEmpty()
        || !trading::tradableSymbols().contains(args.symbol)) {
        printUsage();
        return args.help ? 0 : 64;
    }

    const Config cfg = Config::load();
    EtoroClient client(cfg);
    MarketFeeds feeds;
    client.setTradableSymbols(trading::tradableSymbols());
    feeds.setTradableSymbols(trading::tradableSymbols());
    auto *books = new ScanBooks();
    trading::console::wireScanBooks(client, feeds, books, &app);
    if (args.verbose) {
        wireVerboseTaps(client, &app);
    }
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
