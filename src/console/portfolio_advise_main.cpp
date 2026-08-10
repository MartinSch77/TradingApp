// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// TradingPortfolioAdvise — what to buy next, given what the account already holds, as an
// Excel-readable file (REQ-F-048). Advisory by construction: no order path is linked.

#include "PortfolioReport.h"
#include "ScanBooks.h"
#include "SpreadsheetXml.h"
#include "domain/InstrumentCatalog.h"
#include "domain/TradePlan.h"
#include "services/Config.h"
#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QTimer>

#include <algorithm>
#include <cstdio>

namespace {

using trading::console::PortfolioCandidate;
using trading::console::PortfolioReportInput;
using trading::console::ScanBooks;

struct PortfolioArgs {
    QString outPath;
    bool verbose = false;
    bool help = false;
    qint32 timeoutSec = 180;
};

PortfolioArgs parseArguments(const QStringList &args)
{
    PortfolioArgs out;
    for (qsizetype i = 1; i < args.size(); ++i) {
        if ((args.at(i) == QLatin1String("--help")) || (args.at(i) == QLatin1String("-h"))) {
            out.help = true;
        } else if (args.at(i) == QLatin1String("--verbose")) {
            out.verbose = true;
        } else if ((args.at(i) == QLatin1String("--timeout")) && (i + 1 < args.size())) {
            out.timeoutSec = args.at(++i).toInt();
        } else if ((args.at(i) == QLatin1String("--out")) && (i + 1 < args.size())) {
            out.outPath = args.at(++i);
        }
    }
    if (out.outPath.isEmpty()) {
        out.outPath = QStringLiteral("TradingApp-proposal-%1.xls")
                          .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
    }
    return out;
}

void printUsage()
{
    std::fprintf(
        stderr,
        "TradingPortfolioAdvise — ranked buy proposals for the WHOLE catalog, respecting what\n"
        "the account already holds, written as an Excel-readable file (advisory only; this\n"
        "binary links no order path and can place nothing).\n\n"
        "usage: TradingPortfolioAdvise [options]\n\n"
        "  --out <file>      output path (default TradingApp-proposal-<date>.xls, a\n"
        "                    SpreadsheetML file Excel and LibreOffice open natively)\n"
        "  --timeout <sec>   bound on the data gathering (default 180; a full catalog scan\n"
        "                    takes a while on the broker's rate limits — whatever has not\n"
        "                    arrived is reported ABSENT on the Data health sheet)\n"
        "  --verbose         mirror the client/feed logs to stderr while gathering\n"
        "  --help, -h        this text\n\n"
        "exit codes: 0 = file written, 3 = nothing evaluable arrived, 64 = usage\n");
}

// Wait for the scan to cover the catalog (retrying gently — see TradingAdvise for the two
// measured lessons: the synchronous first finish, and the rate-pool cost of tight retries).
void runGather(EtoroClient &client, MarketFeeds &feeds, ScanBooks *books,
               const PortfolioArgs &args)
{
    auto *scanDone = new bool(false);
    auto *pollCount = new int(0);
    QObject::connect(&client, &EtoroClient::screenerFinished, &feeds,
                     [scanDone] { *scanDone = true; });
    client.start();
    client.refreshPortfolio();
    client.scanInstruments();
    feeds.fetchInstrumentRatings();
    feeds.fetchInstrumentNews();
    feeds.fetchIntradaySeries();
    feeds.fetchReferenceSeries();
    feeds.start(60 * 1000);

    QEventLoop wait;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.start(args.timeoutSec * 1000);
    QObject::connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
    QTimer poll;
    const qsizetype wanted = trading::tradableSymbols().size() / 2;   // half is judgeable
    QObject::connect(&poll, &QTimer::timeout, &wait, [&, scanDone, pollCount] {
        ++*pollCount;
        if (*scanDone && (books->rows.size() >= wanted) && books->vixValid) {
            wait.quit();
        }
        if (*scanDone && (books->rows.size() < wanted) && ((*pollCount % 20) == 0)) {
            *scanDone = false;
            client.scanInstruments();
        }
    });
    poll.start(500);
    wait.exec();
}

// Every scanned instrument with a directional composite gets a COSTED plan; only actionable
// plans become candidates — a direction that cannot pay its own costs is a "no".
QList<PortfolioCandidate> collectCandidates(const ScanBooks &books, EtoroClient &client,
                                            const QList<trading::DecisionRow> &rows)
{
    QList<PortfolioCandidate> out;
    for (const trading::DecisionRow &row : rows) {
        if (row.dir == 0) {
            continue;
        }
        const auto scanRow = std::find_if(books.rows.cbegin(), books.rows.cend(),
                                          [&row](const ScreenerRow &r) {
                                              return r.symbol == row.symbol;
                                          });
        if ((scanRow == books.rows.cend()) || scanRow->closes.isEmpty()) {
            continue;
        }
        trading::PlanInput in;
        in.closes = scanRow->closes;
        in.price = scanRow->lastPrice;
        in.invest = 1000.0;
        in.maxLeverage = scanRow->maxLeverage;
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
        const trading::TradePlan plan = trading::buildTradePlan(in);
        if (plan.valid && (plan.verdict != QLatin1String("STAY OUT")) && (plan.dir != 0)) {
            out.append(PortfolioCandidate{row.symbol, row, plan});
        }
    }
    return out;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TradingApp"));
    QCoreApplication::setApplicationName(QStringLiteral("eToro Trader"));

    const PortfolioArgs args = parseArguments(QCoreApplication::arguments());
    if (args.help) {
        printUsage();
        return 0;
    }

    const Config cfg = Config::load();
    EtoroClient client(cfg);
    MarketFeeds feeds;
    client.setTradableSymbols(trading::tradableSymbols());
    feeds.setTradableSymbols(trading::tradableSymbols());
    auto *books = new ScanBooks();
    trading::console::wireScanBooks(client, feeds, books, &app);

    PortfolioReportInput report;
    QObject::connect(&client, &EtoroClient::portfolioUpdated, &app,
                     [&report](const QList<Position> &positions) {
                         report.portfolioKnown = true;
                         report.positions = positions;
                     });
    QObject::connect(&client, &EtoroClient::cashUpdated, &app,
                     [&report](double available, const QString &currency) {
                         report.cash = available;
                         report.currency = currency;
                     });
    if (args.verbose) {
        QObject::connect(&client, &EtoroClient::log, &app,
                         [](const QString &line, bool) {
                             std::fprintf(stderr, "[client] %s\n", qPrintable(line));
                         });
    }
    runGather(client, feeds, books, args);

    const trading::MarketSnapshot snap = trading::console::snapshotFrom(*books);
    report.candidates =
        collectCandidates(*books, client, trading::computeDecisionRows(snap));
    if (books->rows.isEmpty()) {
        report.absentSources << QStringLiteral("price series / scan rows");
    }
    if (books->ratings.isEmpty()) {
        report.absentSources << QStringLiteral("web ratings");
    }
    if (!books->vixValid) {
        report.absentSources << QStringLiteral("VIX");
    }
    report.generatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (report.candidates.isEmpty() && books->rows.isEmpty()) {
        std::fprintf(stderr, "nothing evaluable arrived — no file written\n");
        return 3;
    }
    QFile out(args.outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(args.outPath));
        return 1;
    }
    out.write(trading::console::spreadsheetXml(
                  trading::console::portfolioReportSheets(report))
                  .toUtf8());
    std::fprintf(stdout, "wrote %s — %d candidate(s), portfolio %s\n",
                 qPrintable(args.outPath), static_cast<int>(report.candidates.size()),
                 report.portfolioKnown ? "read" : "UNAVAILABLE (flat-book ranking)");
    return 0;
}
