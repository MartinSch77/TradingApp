// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// TradingPortfolioAdvise — analyses the instruments the account ACTUALLY HOLDS and recommends
// what to do with each (hold / add / reduce / exit), written as an Excel-readable file
// (REQ-F-048). It does NOT scan the app catalogue: it iterates your open positions, fetches
// each one's candles by the instrumentId the position already carries (no per-symbol search,
// so no id-resolution rate-limit storm), and reads whatever signal that yields. Advisory by
// construction — no order path is linked; it can place nothing.

#include "PortfolioReport.h"
#include "SpreadsheetXml.h"
#include "domain/DecisionEngine.h"
#include "domain/InstrumentCatalog.h"
#include "domain/TradePlan.h"
#include "services/Config.h"
#include "services/EtoroClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QNetworkProxy>
#include <QTimer>

#include <algorithm>
#include <cstdio>

namespace {

using trading::console::HoldingSignal;
using trading::console::PortfolioReportInput;

struct PortfolioArgs {
    QString outPath;
    bool verbose = false;
    bool help = false;
    qint32 timeoutSec = 900;   // a large book, paced against the rate limit, can take a while
    qint32 concurrency = 4;    // candle fetches in flight at once — gentle on the shared pool
    qint32 maxHoldings = 0;    // 0 = every distinct holding; >0 = the largest N by value
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
        } else if ((args.at(i) == QLatin1String("--concurrency")) && (i + 1 < args.size())) {
            out.concurrency = qMax(1, args.at(++i).toInt());
        } else if ((args.at(i) == QLatin1String("--max")) && (i + 1 < args.size())) {
            out.maxHoldings = qMax(0, args.at(++i).toInt());
        } else if ((args.at(i) == QLatin1String("--out")) && (i + 1 < args.size())) {
            out.outPath = args.at(++i);
        }
    }
    if (out.outPath.isEmpty()) {
        out.outPath = QStringLiteral("TradingApp-portfolio-%1.xls")
                          .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
    }
    return out;
}

void printUsage()
{
    std::fprintf(
        stderr,
        "TradingPortfolioAdvise — analyses the instruments you HOLD and recommends what to do\n"
        "with each (hold / add / reduce / exit), as an Excel-readable file (advisory only;\n"
        "this binary links no order path and can place nothing).\n\n"
        "usage: TradingPortfolioAdvise [options]\n\n"
        "  --out <file>      output path (default TradingApp-portfolio-<date>.xls, a\n"
        "                    SpreadsheetML file Excel and LibreOffice open natively)\n"
        "  --timeout <sec>   overall bound (default 900; a large book fetched one instrument\n"
        "                    at a time against the rate limit can take many minutes; whatever\n"
        "                    has not arrived is reported as 'no signal')\n"
        "  --concurrency <n> candle fetches in flight at once (default 4)\n"
        "  --max <n>         analyse only the largest N holdings by value (0 = all)\n"
        "  --verbose         mirror progress to stderr\n"
        "  --help, -h        this text\n\n"
        "exit codes: 0 = file written, 3 = no portfolio, 64 = usage\n");
}

// One holding's candle series turned into a signal: a ScreenerRow-backed composite plus a
// costed plan. Candles empty -> haveSignal stays false (reported as "no data feed").
HoldingSignal analyseHolding(const Position &pos, const QList<Candle> &candles)
{
    HoldingSignal out;
    out.position = pos;
    QList<double> closes;
    closes.reserve(candles.size());
    for (const Candle &c : candles) {
        closes.append(c.close);
    }
    if (closes.size() < 30) {
        out.note = QStringLiteral("no usable candle series (%1 closes)").arg(closes.size());
        return out;
    }
    // The composite over a one-instrument snapshot: technical ensemble from the closes; the
    // rating/news/reads are absent for an arbitrary held instrument and the row says so.
    ScreenerRow row;
    row.symbol = pos.symbol;
    row.closes = closes;
    row.lastPrice = closes.constLast();
    row.ok = true;
    trading::MarketSnapshot snap;
    snap.screenerRows = {row};
    const QList<trading::DecisionRow> rows = trading::computeDecisionRows(snap);
    if (!rows.isEmpty()) {
        out.haveSignal = true;
        out.row = rows.first();
    }
    trading::PlanInput in;
    in.closes = closes;
    in.price = row.lastPrice;
    in.invest = pos.amount > 0.0 ? pos.amount : 1000.0;
    in.now = QDateTime::currentDateTimeUtc();
    const trading::TradePlan plan = trading::buildTradePlan(in);
    if (plan.valid) {
        out.havePlan = true;
        out.plan = plan;
    }
    return out;
}

// Fetch every held instrument's candles by id, at most `concurrency` in flight, filling
// `report.holdings`. Runs its own event loop until the queue drains or the deadline fires.
void gatherHoldings(EtoroClient &client, const QList<Position> &positions,
                    PortfolioReportInput *report, const PortfolioArgs &args)
{
    // Distinct instruments (a book can hold several lots of one); the first lot carries the
    // fields the report shows, and amounts are summed so concentration is the whole holding.
    QList<Position> distinct;
    QHash<QString, qsizetype> indexBySymbol;
    for (const Position &p : positions) {
        const auto it = indexBySymbol.constFind(p.symbol);
        if (it == indexBySymbol.constEnd()) {
            indexBySymbol.insert(p.symbol, distinct.size());
            distinct.append(p);
        } else {
            distinct[it.value()].amount += p.amount;
            distinct[it.value()].profit += p.profit;
        }
    }
    if ((args.maxHoldings > 0) && (distinct.size() > args.maxHoldings)) {
        std::sort(distinct.begin(), distinct.end(),
                  [](const Position &a, const Position &b) { return a.amount > b.amount; });
        distinct = distinct.mid(0, args.maxHoldings);
    }

    QEventLoop wait;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.start(args.timeoutSec * 1000);
    QObject::connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);

    auto *next = new qsizetype(0);
    auto *done = new qsizetype(0);
    auto *inFlight = new qint32(0);
    const qsizetype total = distinct.size();
    std::function<void()> pump;
    auto *pumpPtr = new std::function<void()>();
    *pumpPtr = [&, next, done, inFlight, total]() {
        while ((*inFlight < args.concurrency) && (*next < total)) {
            const Position pos = distinct.at(*next);
            ++*next;
            ++*inFlight;
            if (pos.instrumentId <= 0) {
                HoldingSignal h;
                h.position = pos;
                h.note = QStringLiteral("no instrument id on the position");
                report->holdings.append(h);
                --*inFlight;
                ++*done;
                continue;
            }
            client.fetchCandlesForId(
                pos.instrumentId, QStringLiteral("OneHour"), 300,
                [&, pos, done, inFlight](const QList<Candle> &candles) {
                    report->holdings.append(analyseHolding(pos, candles));
                    --*inFlight;
                    ++*done;
                    if (args.verbose) {
                        std::fprintf(stderr, "[holding] %s (%d/%d)\n",
                                     qPrintable(pos.symbol), static_cast<int>(*done),
                                     static_cast<int>(total));
                    }
                    if (*done >= total) {
                        wait.quit();
                    } else {
                        (*pumpPtr)();
                    }
                });
        }
    };
    if (total == 0) {
        return;
    }
    (*pumpPtr)();
    wait.exec();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TradingApp"));
    QCoreApplication::setApplicationName(QStringLiteral("eToro Trader"));
    // Headless tool talking to known hosts; skip the libproxy factory (see TradingAdvise).
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    const PortfolioArgs args = parseArguments(QCoreApplication::arguments());
    if (args.help) {
        printUsage();
        return 0;
    }

    const Config cfg = Config::load();
    EtoroClient client(cfg);
    if (args.verbose) {
        QObject::connect(&client, &EtoroClient::log, &app, [](const QString &line, bool) {
            std::fprintf(stderr, "[client] %s\n", qPrintable(line));
        });
    }

    // Phase 1: the WHOLE book, unfiltered — every open position, however many instruments,
    // including ones outside the app's catalogue (the whole point of this tool).
    PortfolioReportInput report;
    QList<Position> positions;
    QObject::connect(&client, &EtoroClient::cashUpdated, &app,
                     [&report](double available, const QString &currency) {
                         report.cash = available;
                         report.currency = currency;
                     });
    client.start();
    client.refreshPortfolio();   // populates cash (and the GUI-filtered set we ignore)
    {
        bool got = false;
        QEventLoop wait;
        QTimer deadline;
        deadline.setSingleShot(true);
        deadline.start(120 * 1000);
        QObject::connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
        client.fetchAllPositions([&](const QList<Position> &p) {
            positions = p;
            got = true;
            report.portfolioKnown = true;
            if (args.verbose) {
                std::fprintf(stderr, "[portfolio] %d position(s) across the whole book\n",
                             static_cast<int>(p.size()));
            }
            wait.quit();
        });
        if (!got) {
            wait.exec();
        }
    }

    if (!report.portfolioKnown || positions.isEmpty()) {
        std::fprintf(stderr, "no portfolio read (no credentials, or nothing held) — "
                             "no file written\n");
        return 3;
    }

    // Resolve display names for the distinct instruments (chunked; the endpoint 500s on a bad
    // id in a batch), so the report names the holdings rather than showing bare ids.
    {
        QList<qint64> ids;
        for (const Position &p : positions) {
            if (!ids.contains(p.instrumentId)) {
                ids.append(p.instrumentId);
            }
        }
        QHash<qint64, QString> names;
        for (qsizetype base = 0; base < ids.size(); base += 100) {
            const QList<qint64> chunk = ids.mid(base, 100);
            QEventLoop wait;
            QTimer deadline;
            deadline.setSingleShot(true);
            deadline.start(30 * 1000);
            QObject::connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
            client.resolveInstrumentNames(chunk, [&](const QHash<qint64, QString> &got) {
                for (auto it = got.constBegin(); it != got.constEnd(); ++it) {
                    names.insert(it.key(), it.value());
                }
                wait.quit();
            });
            wait.exec();
        }
        for (Position &p : positions) {
            p.symbol = names.value(p.instrumentId,
                                   QStringLiteral("#%1").arg(p.instrumentId));
        }
    }

    // Phase 2: fetch and analyse every held instrument by its own id, paced.
    gatherHoldings(client, positions, &report, args);
    if (report.holdings.size() < positions.size()) {
        report.absentSources << QStringLiteral("%1 holding(s) not fetched before the timeout")
                                    .arg(positions.size() - report.holdings.size());
    }
    report.generatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QFile out(args.outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(args.outPath));
        return 1;
    }
    out.write(trading::console::spreadsheetXml(trading::console::portfolioReportSheets(report))
                  .toUtf8());
    const auto analysed = std::count_if(report.holdings.cbegin(), report.holdings.cend(),
                                        [](const HoldingSignal &h) { return h.haveSignal; });
    std::fprintf(stdout, "wrote %s — %d holding(s), %d with a signal\n",
                 qPrintable(QFileInfo(args.outPath).absoluteFilePath()),
                 static_cast<int>(report.holdings.size()), static_cast<int>(analysed));
    return 0;
}
