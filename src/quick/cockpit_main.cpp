// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// TradingCockpit — the second front end (REQ-F-038).
//
// The same domain and service libraries as TradingApp, the same CockpitModel, the same QML.
// What differs is the presentation stack: Qt Quick end to end, with no QWidget anywhere.
//
// WHY A SECOND EXECUTABLE AND NOT A SECOND WINDOW. Qt Charts and Qt Graphs declare seventeen
// classes with identical names in the same namespace — QValueAxis, QAbstractAxis, QLineSeries
// and fourteen more. Linking BOTH into one binary makes the QML type registration ambiguous,
// and the failure is silent and total: every Qt Graphs QML type stops resolving ("ValueAxis
// is not a type"), so the price chart is unavailable, so the whole cockpit component fails to
// load and renders as a blank white rectangle with no other symptom. Measured with a minimal
// two-target probe on 2026-08-07: identical source, the only difference being whether
// Qt6::Charts was on the link line, and the Charts build could not resolve a single Graphs
// type. Instantiation order makes no difference — the link alone decides it.
//
// TradingApp's Widgets UI is built on Qt Charts and its trading path is the audited one, so
// the candlestick view lives HERE, in a process that links Qt Graphs and not Qt Charts. The
// QQuickWidget panel inside TradingApp shows the same cockpit with the chart replaced by a
// stated note (see the Loader in Main.qml) rather than pretending the constraint away.
//
// THIS FRONT END CAN TRADE, and everything that stands in front of that is SHARED with the
// Widgets window rather than rewritten here:
//
//   * The REQ-N-005 double-press gate is trading::confirmPress — one implementation, called
//     by both front ends and tested headless (tst_confirmgate). A safety gate with two
//     implementations is, in practice, the weaker of the two.
//   * The order itself goes through the same EtoroClient, which is also the only object in
//     this process that can reach an order endpoint. The view-model has no broker: it emits
//     "a human authorised this" and the composition root decides what to do about it.
//   * Whether the ticket may be used at all — credentials, market open, the amount bounds —
//     is CockpitModel::ticketBlockedReason, pinned by TS-COCKPIT-010.
//
// Without credentials Config::hasCredentials() answers false, the ticket is blocked with
// that reason spelled out, and no order can be attempted at all.

#include "domain/Candles.h"
#include "domain/InstrumentCatalog.h"
#include "domain/Models.h"
#include "domain/IndexConfluence.h"
#include "domain/LeadSignal.h"
#include "domain/PredictionLedger.h"
#include "services/Config.h"
#include "services/EconomicCalendar.h"
#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"
#include "ui/CockpitModel.h"

#include <QGuiApplication>
#include <QHash>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

#include <functional>
#include <memory>

namespace {

// How often the series sweep runs. The confluence recomputes from the reference series, so it
// is only ever as live as this sweep — tightened from 30 s to 15 s so the meter feels real-time
// (Issue 5) while still being gentle on the nineteen-ticker reference fetch. Fast price ticks
// (quotesUpdated) also re-push between sweeps, so cards and marks move on every quote.
constexpr qint32 kRefreshMs = 15000;

// Everything the cockpit shows, assembled from the feeds as they arrive.
//
// A plain struct of books plus one push function, rather than a class with behaviour: all the
// judgement already lives in trading::indexReads, trading::leadSignal and CockpitModel, and
// duplicating any of it here would create a second answer to a question that has one.
struct Books {
    QHash<QString, QList<double>> referenceSeries;
    QHash<QString, trading::VolumeSeries> referenceVolumes;
    QHash<QString, QList<double>> intradayBySymbol;
    QHash<QString, QList<trading::Candle>> candlesBySymbol;
};

void push(const Books &books, const QString &symbol, trading::ui::CockpitModel *model,
          bool simulation)
{
    model->setSimulation(simulation);

    // readInputsFor exists precisely so the two books cannot be searched the wrong way round:
    // the references are keyed by Yahoo TICKER, the futures proxies by APP SYMBOL.
    const trading::IndexReads reads = trading::indexReads(
        symbol, trading::readInputsFor(symbol, books.referenceSeries, books.referenceVolumes,
                                       books.intradayBySymbol));

    trading::LeadInputs in;
    in.symbol = symbol;
    in.reads = reads;
    model->setSignal(symbol, trading::leadSignal(in), reads);

    // The same four cards the Widgets cockpit shows, built by the same shared rule.
    model->setCards(trading::ui::referenceCards(books.referenceSeries,
                                                books.intradayBySymbol));

    // UNCALIBRATED with its real sample count, not a placeholder: this front end has no
    // ledger of its own, so the honest number of resolved samples here is zero and the view
    // must say so rather than omitting the line.
    model->setCalibration(0, trading::kMinSamplesPerBucket, 0.0);

    // Absent, not empty: an instrument whose sweep has not answered has no entry here, and
    // the model then states "no bars" instead of drawing an axis through zero.
    model->setCandles(books.candlesBySymbol.value(symbol));
}


// The feed wiring. Its own function because main() is on the metrics ratchet and this is one
// self-contained job: books in, republish out.
// const: this function only CONNECTS to the feeds, it never drives them. QObject::connect
// takes a const sender, so the signature can say so — and saying so is the difference
// between "wires up the feeds" and "may also start or reconfigure them".
void connectFeeds(const MarketFeeds &feeds, QObject *ctx, Books &books,
                  const std::function<void()> &republish)
{
    QObject::connect(&feeds, &MarketFeeds::referenceSeries, ctx,
                     [&books, republish](const QString &ticker, const QList<double> &closes) {
                         static_cast<void>(books.referenceSeries.insert(ticker, closes));
                         republish();
                     });
    QObject::connect(&feeds, &MarketFeeds::referenceVolumeSeries, ctx,
                     [&books, republish](const QString &ticker,
                                         const trading::VolumeSeries &bars) {
                         static_cast<void>(books.referenceVolumes.insert(ticker, bars));
                         republish();
                     });
    QObject::connect(&feeds, &MarketFeeds::intradayCloses, ctx,
                     [&books, republish](const QString &sym, const QList<double> &closes) {
                         static_cast<void>(books.intradayBySymbol.insert(sym, closes));
                         republish();
                     });
    QObject::connect(&feeds, &MarketFeeds::intradayCandles, ctx,
                     [&books, republish](const QString &sym,
                                         const QList<trading::Candle> &candles) {
                         static_cast<void>(books.candlesBySymbol.insert(sym, candles));
                         republish();
                     });
}

// The trading wiring: what the ticket may do, what a confirmed press sends, and the open
// book coming back. Everything money-moving in this process is connected here and nowhere
// else, which is the point of collecting it.
// Everything the trading wiring needs, as ONE bundle. Six loose parameters is what the
// metrics gate flagged, and it is right for the usual reason: a run of same-shaped
// references is an argument-order defect waiting to happen. The same answer ReadInputs and
// OrderContext give elsewhere in this codebase.
struct Session {
    EtoroClient *client;
    trading::ui::CockpitModel *model;
    Books *books;
    std::shared_ptr<QString> symbol;   // SHARED with main's currentSymbol — see the note there
    const Config *config;
};

void connectTrading(Session &session, QObject *ctx)
{
    EtoroClient &client = *session.client;
    trading::ui::CockpitModel &model = *session.model;
    Books &books = *session.books;
    const std::shared_ptr<QString> symbol = session.symbol;   // the shared current instrument
    const Config &config = *session.config;
    // The ticket's own preconditions, refreshed as the account answers. `marketOpen` comes
    // from the broker's tradeability poll rather than from a clock: this application has
    // already measured that an instrument's own exchange hours and its tradeability can
    // disagree, and the broker is the authority on which.
    const auto refreshTicketContext = [&model, &config](bool marketOpen) {
        model.setTradeContext(config.hasCredentials(), marketOpen,
                              /*minAmount=*/0.0, /*maxAmount=*/0.0);
    };
    refreshTicketContext(false);   // shut until the broker says otherwise
    QObject::connect(&client, &EtoroClient::tradeabilityUpdated, ctx,
                     [refreshTicketContext, symbol](const QSet<QString> &tradeable) {
                         refreshTicketContext(tradeable.contains(*symbol));
                     });

    // AUTHORISED, not yet correct: the gate says a human asked for this twice, and the
    // client's own validation is what decides whether it is sendable (REQ-N-009).
    QObject::connect(&model, &trading::ui::CockpitModel::placeRequested, ctx,
                     [&client](bool buy, double amount, qint32 leverage, double stopLoss,
                               double takeProfit) {
                         OrderRequest req;
                         req.isBuy = buy;
                         req.amount = amount;
                         req.leverage = leverage;
                         req.stopLossAmount = stopLoss;
                         req.takeProfitAmount = takeProfit;
                         client.openPosition(req);
                     });
    // The switch itself is wired in main(), where the shared symbol, the feeds and the calendar
    // all are; here we only publish the choices the QML selector offers.
    model.setInstruments(trading::tradableSymbols());
    QObject::connect(&model, &trading::ui::CockpitModel::closeRequested, ctx,
                     [&client](const QString &positionId) {
                         client.closePosition(positionId);
                     });

    // The open book. Marked off the latest candle CLOSE rather than a rates row: this
    // project measured the rates feed running 6-12 minutes behind while the one-minute
    // candle close matched the bid exactly.
    QObject::connect(&client, &EtoroClient::portfolioUpdated, ctx,
                     [&model, &books, symbol](const QList<Position> &positions) {
                         const QList<trading::Candle> bars =
                             books.candlesBySymbol.value(*symbol);
                         const double mark = bars.isEmpty() ? 0.0 : bars.constLast().close;
                         model.setPositions(positions, mark);
                     });

    // (Issue 3) The 13-week closed history behind the cockpit's "Closed" panel. The sweep
    // FETCHES it (fetchClosedTrades) but the result signal was never connected, so the panel
    // stayed empty. Mirrors MainWindow::onClosedTrades -> setClosedHistory.
    QObject::connect(&client, &EtoroClient::closedTradesReady, ctx,
                     [&model](const QList<ClosedTrade> &closed) {
                         model.setClosedHistory(closed, 13);
                     });
}

// The QA capture harness.
void installShotHarness(QQmlApplicationEngine &engine, QObject *ctx)
{
    // QA aid, mirroring TRADINGAPP_SHOT in the Widgets binary: grab the window to a PNG and
    // quit. The Widgets harness walks QApplication::allWidgets(), which finds nothing here —
    // there are no widgets — so this front end needs its own, and it needs one for the same
    // reason: a QML surface that fails to load renders as an empty rectangle, and only a
    // capture distinguishes "loaded and empty" from "did not load".
    const QString shot = qEnvironmentVariable("TRADINGAPP_SHOT");
    if (!shot.isEmpty()) {
        const qint32 delayMs = qEnvironmentVariableIsSet("TRADINGAPP_SHOT_DELAY_MS")
                                   ? qEnvironmentVariableIntValue("TRADINGAPP_SHOT_DELAY_MS")
                                   : 3000;
        QTimer::singleShot(delayMs, ctx, [&engine, shot]() {
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
            if (window == nullptr) {
                qCritical("root object is not a QQuickWindow — nothing to grab");
                QCoreApplication::exit(1);
                return;
            }
            // grabWindow() renders through the scene graph, so this captures what the GPU
            // path actually produced rather than re-rendering the item tree.
            const bool saved = window->grabWindow().save(shot);
            qInfo("cockpit capture %s -> %s", saved ? "written" : "FAILED",
                  qUtf8Printable(shot));
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }
}

} // namespace

int main(int argc, char *argv[])
{
    // WSL has no usable GPU passthrough here, so Mesa's default Zink (OpenGL-on-Vulkan) path
    // probes for a Vulkan device that does not exist, prints the libEGL / "ZINK: failed to
    // choose pdev" warnings, and falls back to software rendering anyway. Force the software
    // rasteriser up front on WSL so that fallback is CLEAN and silent; a machine with a real
    // GPU is left untouched, and an explicit LIBGL_ALWAYS_SOFTWARE from the user always wins.
    // This must run before the QGuiApplication constructor, which initialises the graphics
    // stack — after it, the driver is already chosen.
    if (qEnvironmentVariableIsSet("WSL_DISTRO_NAME")
        && !qEnvironmentVariableIsSet("LIBGL_ALWAYS_SOFTWARE")) {
        qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
        qputenv("GALLIUM_DRIVER", "llvmpipe");
    }

    QGuiApplication app(argc, argv);
    // The SAME identity as the Widgets app (main.cpp), because Config::load and the bot
    // books resolve through QStandardPaths::AppConfigLocation, which is built from these — a
    // different name would read a different config directory and miss the user's credentials
    // and books entirely.
    QCoreApplication::setOrganizationName(QStringLiteral("TradingApp"));
    QCoreApplication::setApplicationName(QStringLiteral("eToro Trader"));
    QCoreApplication::setApplicationVersion(QStringLiteral(TRADINGAPP_VERSION));

    const Config config = Config::load();
    // The current instrument, held in a SHARED cell so every feed handler, the trade wiring and
    // the calendar observe a switch. It used to be a const copy captured by value into each
    // lambda, which is exactly why selecting a new instrument changed nothing — every lambda
    // kept rendering the original symbol. Updating *currentSymbol is now seen everywhere.
    const auto currentSymbol = std::make_shared<QString>(config.symbol);

    trading::ui::CockpitModel model;
    Books books;

    MarketFeeds feeds;

    // The ONE object in this process that can reach an order endpoint. Deliberately not
    // owned by the view-model: a view-model that can place an order is a view-model no test
    // can safely exercise.
    EtoroClient client(config);

    // Every arrival re-pushes the whole picture rather than patching one field. The books are
    // small and the cost is nothing; a partial update that drifts out of step with the rest is
    // a cockpit showing two different moments at once.
    const auto republish = [&books, &model, currentSymbol, &config] {
        push(books, *currentSymbol, &model, !config.hasCredentials());
    };

    connectFeeds(feeds, &app, books, republish);

    Session session{&client, &model, &books, currentSymbol, &config};
    connectTrading(session, &app);

    // (Issue 4) The economic calendar — constructed, scoped to the instrument, wired and
    // started. The cockpit did NONE of this before (it #included the header but never made the
    // object), so the calendar panel was permanently "none scheduled". Mirrors MainWindow.
    EconomicCalendar calendar;
    calendar.setInstrument(*currentSymbol);
    QObject::connect(&calendar, &EconomicCalendar::eventsUpdated, &app,
                     [&model](const QList<EconomicEvent> &events) { model.setEvents(events); });
    calendar.start();

    // (Issue 1) Switching instrument, in ONE place with everything it must re-point: the shared
    // symbol, the broker, the feeds' current symbol, the calendar's instrument, and an
    // immediate re-push so cards/chart/confluence/opening-hours repaint against the new symbol.
    // Previously only client.setSymbol ran, so the display never moved off the first instrument.
    QObject::connect(&model, &trading::ui::CockpitModel::instrumentRequested, &app,
                     [&client, &feeds, &calendar, currentSymbol, republish](const QString &next) {
                         *currentSymbol = next;
                         client.setSymbol(next);
                         feeds.setCurrentSymbol(next);
                         calendar.setInstrument(next);
                         republish();
                     });

    // (Issue 5) Confluence as live as the data allows: re-push on every fast price tick, not
    // only on the series sweep, so the cards and marks move on each quote. The reads themselves
    // recompute from the reference series, so the confluence is only ever as fresh as that
    // sweep — which is why its cadence is tightened (kRefreshMs) rather than left at 30 s.
    QObject::connect(&client, &EtoroClient::quotesUpdated, &app, republish);

    QQmlApplicationEngine engine;
    // setInitialProperties, not a context property: as a context property the model resolved
    // for the root object's own bindings but read as NULL inside the bindings created for
    // child components. `required property var cockpit` plus an initial property makes a
    // missing injection fail loudly at load instead of rendering a half-empty view.
    engine.setInitialProperties({{QStringLiteral("cockpit"), QVariant::fromValue(&model)}});
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/TradingApp/Cockpit/CockpitWindow.qml")));
    if (engine.rootObjects().isEmpty()) {
        // Loud, because the alternative is an invisible failure: no window, exit 0, and
        // nothing on screen to say why.
        qCritical("cockpit QML failed to load — see the QML errors above");
        return 1;
    }

    client.start();
    // THE WHOLE CATALOG, not just the instrument on screen — and this was a real defect.
    //
    // With only the selected symbol registered, the intraday sweep never fetched the futures
    // proxies SP.24-7 and NSDQ100.24-7. Those two series are what the SPX500 and NSDQ100
    // cards display AND what the futuresLead / futuresMomentum reads are computed from, so
    // ONE missing symbol list produced two separate visible symptoms: two cards stuck on an
    // em dash, and two of the nine reads permanently "unmeasurable". The Widgets window
    // passes trading::tradableSymbols() here, and the cockpit must too or it is quietly
    // measuring less than the application it claims to mirror.
    feeds.setTradableSymbols(trading::tradableSymbols());
    client.setTradableSymbols(trading::tradableSymbols());
    feeds.setCurrentSymbol(*currentSymbol);
    feeds.start(kRefreshMs);
    // start() only kicks off the slow single-value feeds (VIX, sentiment, the web quote).
    // The two SERIES sweeps are separate calls — MainWindow drives them from its own timers —
    // so this front end has to drive them too, and on its own timer, or the cockpit would sit
    // permanently at "no bars" while every other panel filled in.
    const auto sweepSeries = [&feeds, &client] {
        feeds.fetchIntradaySeries();
        feeds.fetchReferenceSeries();
        client.fetchClosedTrades(13);   // the 13-week history, refreshed with the sweep
    };
    auto *seriesTimer = new QTimer(&app);
    QObject::connect(seriesTimer, &QTimer::timeout, &app, sweepSeries);
    seriesTimer->start(kRefreshMs);
    sweepSeries();   // now, not in 30 seconds
    // The first push happens before any feed answers, so the window opens in the stated
    // "no bars yet" state rather than blank.
    republish();

    installShotHarness(engine, &app);

    return QGuiApplication::exec();
}
