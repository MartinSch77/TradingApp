// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// TradingBot — a console front end for EXAMINING the paper-trading bot (REQ-F-029).
//
// It exists to answer one question the GUI answers less directly: left running, does this
// strategy make money? So it shows the record the way a person watching a terminal wants it —
// P/L and invested pinned at the top, a stable row per open trade, the closed trades and the
// exit rule that ended each, and the two indices this build is focused on with their megacap
// constituents drawn as a bar chart, because the biggest names lead the index.
//
// SAME LOGIC AS THE GUIS, NOT A COPY. It links the identical BotSimRunner the Widgets and
// Quick front ends drive — the split of BotSimRunner out of BotSimPanel is what makes that
// possible — so the decisions, the cost model, the books and the decision log are the same
// code. A console that reasoned differently would measure a different bot.
//
// LLM LEAD BY DEFAULT (the user's choice), with every other signal still gating: in lead mode
// the model supplies the DIRECTION and the composite's risk rules, confluence gate, session
// phase and cost model all still have to pass. The model can lead the bot INTO a trade; it
// can never lever it past the risk budget or open into a shut market.
//
// STATIC SCREEN. The display is redrawn in place with ANSI positioning — it does not scroll.
// The decision log is a bounded region the arrow keys / j-k / PageUp-Down scroll through.
// Plain ANSI and a raw-mode read on stdin rather than ncurses, so setup.sh gains no
// dependency and a Windows console port stays possible.

#include "console/BotConsoleView.h"
#include "console/ScanBooks.h"
#include "domain/IndexConfluence.h"
#include "domain/InstrumentCatalog.h"
#include "services/Config.h"
#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"
#include "services/OllamaAdvisor.h"
#include "ui/BotSimRunner.h"

#include <QCoreApplication>
#include <QSocketNotifier>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <deque>

#include <termios.h>
#include <unistd.h>

namespace {

// The two indices this build watches (the user's focus). Their futures proxies are what the
// confluence reads and the heavyweight chart are built from.
const QString kIndexA = QStringLiteral("SPX500");
const QString kIndexB = QStringLiteral("NSDQ100");

// ANSI, kept in one place. No ncurses: these six escapes are the whole vocabulary.
constexpr const char *kClear = "\x1b[2J";
constexpr const char *kHome = "\x1b[H";
constexpr const char *kHideCursor = "\x1b[?25l";
constexpr const char *kShowCursor = "\x1b[?25h";
constexpr const char *kClearLine = "\x1b[K";

// Terminal raw mode, restored on exit however we leave — including a signal, since a bot left
// running overnight is exactly the process that gets a Ctrl-C or a SIGTERM, and a terminal
// left in raw mode with the cursor hidden is unusable afterwards.
struct RawMode {
    termios saved{};
    bool active = false;

    void enter()
    {
        if (tcgetattr(STDIN_FILENO, &saved) != 0) {
            return;   // not a tty (piped/redirected): run without raw mode rather than fail
        }
        termios raw = saved;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active = true;
        std::fputs(kHideCursor, stdout);
    }
    void leave()
    {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
            std::fputs(kShowCursor, stdout);
            std::fputs("\x1b[2J\x1b[H", stdout);
            std::fflush(stdout);
            active = false;
        }
    }
};

RawMode g_raw;

void restoreOnSignal(int sig)
{
    g_raw.leave();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// The scrolling decision-log region: a bounded ring the screen shows a window into. Bounded
// because a bot runs for days and an unbounded buffer is a slow memory leak; the file
// (botsim-decisions.log) is the permanent record, this is only the live view.
class LogScroller
{
public:
    void add(const QString &line)
    {
        m_lines.push_back(line);
        while (m_lines.size() > kCap) {
            m_lines.pop_front();
        }
        if (m_stickToBottom) {
            m_top = maxTop();
        }
    }

    void scroll(int delta)
    {
        // Any manual scroll detaches from the bottom; scrolling back to the end re-attaches,
        // so a user who scrolls up to read stays put while new lines arrive, and one who
        // returns to the bottom resumes following.
        m_top = std::clamp<int>(m_top + delta, 0, maxTop());
        m_stickToBottom = (m_top == maxTop());
    }

    [[nodiscard]] QStringList window(int rows) const
    {
        QStringList out;
        for (int i = 0; i < rows; ++i) {
            const int idx = m_top + i;
            out.append((idx >= 0 && idx < static_cast<int>(m_lines.size()))
                           ? m_lines.at(static_cast<std::size_t>(idx))
                           : QString());
        }
        return out;
    }

    void setViewport(int rows) { m_viewport = std::max(1, rows); }

private:
    [[nodiscard]] int maxTop() const
    {
        return std::max(0, static_cast<int>(m_lines.size()) - m_viewport);
    }

    static constexpr std::size_t kCap = 2000;
    std::deque<QString> m_lines;
    int m_top = 0;
    int m_viewport = 10;
    bool m_stickToBottom = true;
};

// One heavyweight list turned into HeavyMove rows from the reference series the feeds fetch.
// An absent series stays absent (known = false) rather than becoming a flat 0% — the
// absent-is-not-zero rule the whole app follows.
QList<trading::console::HeavyMove> heavyMovesFor(
    const QString &indexSymbol, const QHash<QString, QList<double>> &byTicker)
{
    QList<trading::console::HeavyMove> out;
    for (const QString &name : trading::indexHeavyweights(indexSymbol)) {
        trading::console::HeavyMove h;
        h.name = name;
        const QList<double> series = byTicker.value(name);
        if (series.size() >= 2 && series.constFirst() > 0.0) {
            h.changePct =
                ((series.constLast() - series.constFirst()) / series.constFirst()) * 100.0;
            h.known = true;
        }
        out.append(h);
    }
    return out;
}

// The whole static screen as one function, so main() stays under the metrics ratchet and
// the drawing is separable from the wiring. Redrawn in place; no scrolling.
void drawScreen(const BotSimRunner &runner, const QHash<QString, QList<double>> &snapshot,
                LogScroller &log)
{
        const trading::PaperStats stats = runner.stats();
        QString out;
        out += kHome;
        const auto line = [&](const QString &s) { out += s + QString::fromLatin1(kClearLine) + "\r\n"; };

        line(QStringLiteral("  TradingBot — SIMULATED money on live prices. No real order is "
                            "ever placed.  [q quit · ↑↓/PgUp/PgDn scroll log]"));
        line(QStringLiteral("  ") + QString(110, QChar(u'─')));
        line(QStringLiteral("  ") + trading::console::consoleHeader(stats, runner.aiMode(),
                                                                    runner.armed()));
        line(QString());

        // The two focused indices and their megacaps, SIDE BY SIDE.
        for (const QString &row : trading::console::consoleHeavyBarsSideBySide(
                 kIndexA, heavyMovesFor(kIndexA, snapshot),
                 kIndexB, heavyMovesFor(kIndexB, snapshot), 10)) {
            line(row);
        }
        // The one-line summarised constituent-lead: cap-weighted top-ten direction of each
        // index, up or down together. The number and arrow come from the domain pulse.
        line(trading::console::consoleConstituentLead(
            trading::heavyweightPulse(kIndexA, snapshot).leadIndicator(),
            trading::heavyweightPulse(kIndexB, snapshot).leadIndicator()));
        line(QString());

        line(QStringLiteral("  OPEN"));
        for (const QString &row : trading::console::consoleOpenTrades(runner.book().openTrades())) {
            line(row);
        }
        line(QString());
        line(QStringLiteral("  CLOSED (newest first)"));
        for (const QString &row :
             trading::console::consoleClosedTrades(runner.book().closedTrades(), 6)) {
            line(row);
        }
        line(QString());
        line(QStringLiteral("  DECISIONS  (live; full history: %1)")
                 .arg(BotSimRunner::decisionLogPath()));
        log.setViewport(10);
        for (const QString &row : log.window(10)) {
            line(QStringLiteral("  ") + row);
        }
        std::fputs(out.toUtf8().constData(), stdout);
        std::fflush(stdout);
}

// One read's worth of keys: scroll the log, or return true to quit. Its own function so
// main() stays under the metrics ratchet; the escape-sequence parse for the arrow keys is the
// only fiddly part, and it belongs next to the LogScroller it drives, not in the wiring.
[[nodiscard]] bool handleKeys(const char *buf, ssize_t n, LogScroller &log)
{
    for (ssize_t i = 0; i < n; ++i) {
        const char c = buf[i];
        if (c == 'q') {
            return true;
        }
        if (c == 'j') {
            log.scroll(1);
        } else if (c == 'k') {
            log.scroll(-1);
        } else if (c == '\x1b' && (i + 2 < n) && buf[i + 1] == '[') {
            const char code = buf[i + 2];
            if (code == 'A') {
                log.scroll(-1);   // up arrow
            } else if (code == 'B') {
                log.scroll(1);    // down arrow
            } else if (code == '5') {
                log.scroll(-10);  // PageUp
            } else if (code == '6') {
                log.scroll(10);   // PageDown
            }
            i += 2;
        }
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // The SAME organisation and application name the Widgets app (main.cpp) uses. This is
    // load-bearing, not cosmetic: QStandardPaths::AppConfigLocation is built from these, and
    // it is where Config::load reads the credentials AND where the bot's books
    // (botsim.json, botsim-decisions.log, the experience log) live. A different name here
    // gave the console its OWN empty book — it was examining a fresh 50k account, not the
    // bot the GUI actually runs. Sharing the identity is what makes "examine the bot" true.
    QCoreApplication::setOrganizationName(QStringLiteral("TradingApp"));
    QCoreApplication::setApplicationName(QStringLiteral("eToro Trader"));

    const Config config = Config::load();

    // The bot's own services, exactly as the GUI composition root wires them.
    EtoroClient client(config);
    MarketFeeds feeds;
    OllamaAdvisor advisor(config.ollamaHost, config.ollamaModel);

    BotSimRunner runner(&client, &advisor);
    // LLM LEAD by default (the user's choice). TRADINGAPP_BOT_AI overrides for parity with
    // the GUI, but a bare `TradingBot` leads with the model — the same off|confirm|lead
    // words MainWindow reads.
    const QString aiEnv = qEnvironmentVariable("TRADINGAPP_BOT_AI").trimmed().toLower();
    trading::BotAiMode aiMode = trading::BotAiMode::Lead;
    if (aiEnv == QStringLiteral("off")) {
        aiMode = trading::BotAiMode::Off;
    } else if (aiEnv == QStringLiteral("confirm")) {
        aiMode = trading::BotAiMode::Confirm;
    }
    runner.setAiMode(aiMode);
    runner.setArmed(true);   // a console launched to watch the bot is launched to run it

    LogScroller log;
    // Every runner line goes to the scroller AND is already appended to the decision-file by
    // the runner itself — the console shows reasoning live, the file keeps it.
    QObject::connect(&runner, &BotSimRunner::log, &app,
                     [&log](const QString &line, bool) { log.add(line); });

    // The books that feed the confluence reads, the heavyweight chart AND the decision scan.
    // `snapshot` is the reference series (also drawn as the heavy bars); `books` collects the
    // REST of the MarketSnapshot the runner needs. Without them the console handed onDecisions an
    // empty snapshot, so the runner saw no scan rows and produced NO decisions — the bug behind
    // the empty botsim-decisions.log.
    auto *books = new trading::console::ScanBooks();
    trading::console::wireScanBooks(client, feeds, books, &app);
    // Runner-specific on top of the shared wiring: every reference-series arrival also feeds
    // the runner's confluence reads live (the shared books hold the very same hash).
    QObject::connect(&feeds, &MarketFeeds::referenceSeries, &app,
                     [books, &runner](const QString &, const QList<double> &) {
                         runner.setReferenceSeries(books->reference);
                     });

    // The all-instruments scan that produces decisions, on the same ~5-minute cadence the GUI
    // uses. The bot focuses on the two indices, but the scan still covers the catalog so the
    // risk caps and correlation groups see the whole book.
    client.setTradableSymbols(trading::tradableSymbols());
    feeds.setTradableSymbols(trading::tradableSymbols());
    const auto scan = [&client, &feeds, books] {
        books->rows.clear();   // a fresh scan; last cycle's rows must not linger
        client.scanInstruments();
        feeds.fetchIntradaySeries();
        feeds.fetchReferenceSeries();
    };
    QObject::connect(&client, &EtoroClient::screenerFinished, &app, [&runner, books] {
        // Build the SAME MarketSnapshot MainWindow does and hand it to the runner, so the console
        // runs the real decision pipeline instead of an empty one.
        const trading::MarketSnapshot snap = trading::console::snapshotFrom(*books);
        runner.onDecisions(trading::computeDecisionRows(snap), snap);
    });
    client.start();
    feeds.start(30000);
    scan();
    auto *scanTimer = new QTimer(&app);
    QObject::connect(scanTimer, &QTimer::timeout, &app, scan);
    scanTimer->start(5 * 60 * 1000);

    // The static screen, redrawn in place a few times a second — often enough that P/L looks
    // live, rarely enough to cost nothing.
    g_raw.enter();
    std::signal(SIGINT, restoreOnSignal);
    std::signal(SIGTERM, restoreOnSignal);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] { g_raw.leave(); });


    std::fputs(kClear, stdout);
    auto *redraw = new QTimer(&app);
    QObject::connect(redraw, &QTimer::timeout, &app,
                     [&] { drawScreen(runner, books->reference, log); });
    redraw->start(400);

    // Keyboard: raw stdin through a socket notifier, so the event loop keeps running the bot
    // while we wait for a key. Arrow keys arrive as an escape sequence; j/k and PageUp/Down
    // are the no-escape fallbacks.
    auto *keys = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, &app);
    QObject::connect(keys, &QSocketNotifier::activated, &app, [&log, &app] {
        char buf[8];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (handleKeys(buf, n, log)) {
            app.quit();
        }
    });

    return QCoreApplication::exec();
}
