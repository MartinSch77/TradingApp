#include "ui/MainWindow.h"

#include "domain/DecisionEngine.h"
#include "domain/EventInsight.h"
#include "domain/Forecasting.h"
#include "domain/Indicators.h"
#include "domain/InstrumentCatalog.h"
#include "domain/PositionMath.h"
#include "domain/SignalEnsemble.h"
#include "services/AiAdvisor.h"
#include "services/EconomicCalendar.h"
#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"
#include "ui/Palette.h"
#include "ui/PositionsModel.h"
#include "ui/PriceChart.h"
#include "ui/ScreenerDialog.h"
#include "services/OllamaAdvisor.h"
#include "ui/BotSimPanel.h"
#include "ui/TradeScriptPanel.h"
#include "ui/TradeGauge.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QTime>
#include <QTimeZone>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QKeyEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QSet>
#include <QStandardItemModel>
#include <QTableView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <numeric>
#include <cmath>
#include <utility>

namespace {

// After any buy/sell order, ignore further orders for this long so a double-click
// (or a burst of auto-triggers) can't fire several trades in quick succession.
constexpr qint32 kOrderCooldownMs = 2000;

// Hard cap on the total amount tied up in open trades at once (account currency).
// A new order is rejected if it would push the open-trades total above this.
constexpr double kMaxOpenExposure = 17000.0;

// Open-trades table columns live in PositionsModel::Column; the aliases below
// keep the SL/TP edit handling readable.
constexpr qint32 PosColSl = PositionsModel::ColSl;
constexpr qint32 PosColTp = PositionsModel::ColTp;

// Resting-limit-order table: Side | Instrument | Trigger | Now | Amount | SL | TP | Status.
// A click in the Trigger…TP span opens the order's editor, and "Now" (the instrument's
// live rate) is refreshed in place on every price tick rather than by a rebuild (REQ-F-027).
constexpr qint32 kPendingColumns = 8;
constexpr qint32 kPendingInstrumentColumn = 1;
constexpr qint32 kPendingTriggerColumn = 2;
constexpr qint32 kPendingNowColumn = 3;
constexpr qint32 kPendingTpColumn = 6;

QString colored(const QString &text, const QString &hexColor)
{
    return QStringLiteral("<span style='color:%1'>%2</span>").arg(hexColor, text);
}

// Cell for the ranked table's "Trade plan" column: the SAME verdict the trade-plan
// panel shows (cost + break-even gates included), so the two can never disagree.
QTableWidgetItem *makePlanVerdictItem(const trading::TradePlan &plan)
{
    auto *it = new QTableWidgetItem();
    it->setTextAlignment(Qt::AlignCenter);
    if (!plan.valid) {
        it->setText(QStringLiteral("—"));
        it->setForeground(trading::ui::kGrey);
        it->setToolTip(QStringLiteral("Not enough price history to build a plan."));
    } else if (plan.verdict == QLatin1String("STAY OUT")) {
        it->setText(QStringLiteral("✋ stay out"));
        it->setForeground(trading::ui::kAmber);
        it->setToolTip(QStringLiteral("Plan verdict: STAY OUT — %1.").arg(plan.verdictReason));
    } else {
        it->setText(QStringLiteral("✓ %1").arg(plan.verdict));
        it->setForeground((plan.dir > 0) ? trading::ui::kGreen : trading::ui::kRed);
        it->setToolTip(QStringLiteral("Plan verdict: %1 — positive expected edge after "
                                      "costs; the trade plan below has the sizing.")
                           .arg(plan.verdict));
    }
    return it;
}

// Placeholder cell while the batch plan build for the ranked table is running.
QTableWidgetItem *makePendingPlanItem()
{
    auto *it = new QTableWidgetItem(QStringLiteral("…"));
    it->setTextAlignment(Qt::AlignCenter);
    it->setForeground(trading::ui::kGrey);
    it->setToolTip(QStringLiteral("Costing the trade plan…"));
    return it;
}

// Columns of the decision window's "All instruments, ranked" table — one place
// for the indices so the fill loop and the in-place repaints can't drift apart
// (constants rather than an enum, matching the PosCol aliases above).
constexpr qint32 RankedColInstrument = 0;
constexpr qint32 RankedColCall = 1;
constexpr qint32 RankedColComposite = 2;
constexpr qint32 RankedColConfidence = 3;
constexpr qint32 RankedColWeb = 4;
constexpr qint32 RankedColOpen = 5;
constexpr qint32 RankedColPlan = 6;
constexpr qint32 RankedColCount = 7;


// Cell for the ranked table's "Web signal" column: the TradingView multi-timeframe
// technical rating (the same read that feeds the composite), per instrument.
QTableWidgetItem *makeWebRatingItem(const WebRating &r)
{
    auto *it = new QTableWidgetItem();
    it->setTextAlignment(Qt::AlignCenter);
    if (!r.valid()) {
        it->setText(QStringLiteral("n/a"));
        it->setForeground(trading::ui::kGrey);
        it->setToolTip(QStringLiteral(
            "No TradingView rating for this instrument — no rated web symbol or "
            "proxy exists (or the bulk rating fetch has not returned yet)."));
        return it;
    }
    const double score = r.consensus();
    it->setText(
        QStringLiteral("%1 (%2)").arg(trading::webRatingWord(score)).arg(score, 0, 'f', 2));
    it->setForeground((score >= 0.1) ? trading::ui::kGreen
                                     : ((score <= -0.1) ? trading::ui::kRed
                                                        : trading::ui::kGrey));
    auto tf = [](double v) {
        return std::isnan(v) ? QStringLiteral("n/a") : QStringLiteral("%1").arg(v, 0, 'f', 2);
    };
    // Sequenced into locals: four calls inside one .arg() would be unsequenced.
    const QString t15 = tf(r.m15);
    const QString t60 = tf(r.h1);
    const QString t1d = tf(r.d1);
    const QString tCons = tf(score);
    it->setToolTip(QStringLiteral(
                       "TradingView aggregated technical rating, -1 (Strong Sell) … +1 "
                       "(Strong Buy).\n15m: %1   1h: %2   1D: %3 — consensus %4.\n"
                       "For eToro thematic baskets this is the closest liquid ETF/index "
                       "proxy. Advisory only — it never trades.")
                       .arg(t15, t60, t1d, tCons));
    return it;
}

// Colour for an event-impact direction (+1 bullish, -1 bearish, 0 volatile).
QColor impactColor(qint32 dir)
{
    if (dir > 0) {
        return trading::ui::kGreen;
    }
    if (dir < 0) {
        return trading::ui::kRed;
    }
    return trading::ui::kAmber;
}

// Rich, explanatory mouse-over for one calendar row: what the event is, when it
// is due, the released numbers, and the heuristic read for the traded instrument.
// Width-boxed so the description wraps instead of stretching into one very wide line.
QString eventTooltip(const EconomicEvent &e, const trading::ImpactGuess &guess,
                     const QString &symbol)
{
    const QString when =
        e.when.toLocalTime().toString(QStringLiteral("dddd dd MMMM yyyy  HH:mm t"));
    const QString fc = e.forecast.isEmpty() ? QStringLiteral("—") : e.forecast.toHtmlEscaped();
    const QString prev = e.previous.isEmpty() ? QStringLiteral("—") : e.previous.toHtmlEscaped();

    const QString title = e.title.toHtmlEscaped();
    const QString country = e.country.toHtmlEscaped();
    const QString whenEsc = when.toHtmlEscaped();
    const QString about = trading::eventAbout(e, symbol);
    const QString impact = e.impact.toHtmlEscaped();
    const QString symbolEsc = symbol.toHtmlEscaped();
    const QString guessText = guess.text.toHtmlEscaped();
    return QStringLiteral(
               "<table width=\"340\" cellspacing=\"0\" cellpadding=\"0\"><tr><td>"
               "<b>%1</b> <span style=\"color:#888\">(%2)</span><br>"
               "%3<br><br>%4<br><br>"
               "<b>Impact:</b> %5 &nbsp;&nbsp; <b>Forecast:</b> %6 &nbsp;&nbsp; "
               "<b>Previous:</b> %7<br>"
               "<b>Likely %8 move:</b> %9"
               "</td></tr></table>")
        .arg(title, country, whenEsc, about, impact, fc, prev, symbolEsc, guessText);
}

} // namespace

MainWindow::MainWindow(EtoroClient *client, MarketFeeds *feeds, AiAdvisor *aiAdvisor,
                       EconomicCalendar *calendar, QWidget *parent)
    : QMainWindow(parent)
    , m_client(client)
    , m_feeds(feeds)
    , m_aiAdvisor(aiAdvisor)
    // Declaration order (MainWindow.h), which is the order the members are
    // really initialized in — anything else is a -Wreorder warning.
    , m_pnlAfterCloseTimer(new QTimer(this))
    , m_calendar(calendar)
    , m_eventTimer(new QTimer(this))
    , m_recoTimer(new QTimer(this))
    , m_orderCooldownTimer(new QTimer(this))
{
    buildUi();
    updateOpenTradesSummary();   // "No open trades." until the first portfolio snapshot

    // The QMetaObject::Connection results are intentionally discarded: these are
    // designed-in, program-lifetime connections that are never disconnected by hand.
    static_cast<void>(connect(m_client, &EtoroClient::ready, this, &MainWindow::onReady));
    static_cast<void>(
        connect(m_client, &EtoroClient::feesUpdated, this, [this](const InstrumentFees &fees) {
            m_fees = fees;
            updateOpenCost();
        }));
    static_cast<void>(
        connect(m_client, &EtoroClient::resolveFailed, this, [this](const QString &sym) {
            // Keep BUY/SELL locked (m_instrumentResolving stays true) — the panel must
            // not silently keep trading the previously resolved instrument.
            m_tradeBox->setTitle(
                QStringLiteral("Trade %1 — NOT RESOLVED (re-select to retry)").arg(sym));
            updateTradeButtonsEnabled();
        }));
    static_cast<void>(connect(m_client, &EtoroClient::historyReady, this, &MainWindow::onHistory));
    static_cast<void>(connect(m_client, &EtoroClient::priceUpdated, this, &MainWindow::onPrice));
    // Quotes for the HELD instruments arrive with the same bulk poll as the shown one's
    // price, and a candle repair can land between ticks: re-mark the table on the quote
    // book itself, so a row whose instrument is not the one on screen stays current too.
    static_cast<void>(connect(m_client, &EtoroClient::quotesUpdated, this,
                              &MainWindow::updateOpenTradePnl));
    static_cast<void>(
        connect(m_client, &EtoroClient::portfolioUpdated, this, &MainWindow::onPortfolio));
    static_cast<void>(connect(m_client, &EtoroClient::cashUpdated, this, &MainWindow::onCash));
    static_cast<void>(connect(m_client, &EtoroClient::fxRateUpdated, this, &MainWindow::onFxRate));
    static_cast<void>(
        connect(m_client, &EtoroClient::orderResult, this, &MainWindow::onOrderResult));
    static_cast<void>(
        connect(m_client, &EtoroClient::positionClosed, this, &MainWindow::onPositionClosed));
    static_cast<void>(
        connect(m_client, &EtoroClient::monthlyPnlReady, this, &MainWindow::onMonthlyPnl));
    static_cast<void>(
        connect(m_client, &EtoroClient::monthlyPnlFailed, this, &MainWindow::onMonthlyPnlFailed));
    static_cast<void>(
        connect(m_client, &EtoroClient::closedTradesReady, this, &MainWindow::onClosedTrades));
    // Fees fetched for a non-selected instrument (decision-plan cache miss):
    // re-render the decision window so its cost bill completes.
    static_cast<void>(connect(m_client, &EtoroClient::instrumentFeesUpdated, this,
                              [this](const QString &, const InstrumentFees &) {
                                  if ((m_decisionDialog != nullptr) && m_decisionDialog->isVisible()) {
                                      rebuildDecision();
                                  }
                              }));
    static_cast<void>(connect(m_feeds, &MarketFeeds::vixUpdated, this, &MainWindow::onVix));
    static_cast<void>(
        connect(m_feeds, &MarketFeeds::externalSignalUpdated, this, &MainWindow::onExternalSignal));
    static_cast<void>(
        connect(m_feeds, &MarketFeeds::fearGreedUpdated, this, &MainWindow::onFearGreed));
    static_cast<void>(connect(m_feeds, &MarketFeeds::webQuoteUpdated, this, &MainWindow::onWebQuote));
    static_cast<void>(connect(m_client, &EtoroClient::log, this, &MainWindow::onLog));
    static_cast<void>(
        connect(m_client, &EtoroClient::leverageOptions, this, &MainWindow::onLeverageOptions));
    static_cast<void>(
        connect(m_client, &EtoroClient::tradeabilityUpdated, this, &MainWindow::onTradeability));
    static_cast<void>(connect(m_client, &EtoroClient::screenerRow, this, &MainWindow::onScreenerRow));
    static_cast<void>(
        connect(m_client, &EtoroClient::screenerProgress, this, &MainWindow::onScreenerProgress));
    static_cast<void>(
        connect(m_client, &EtoroClient::screenerFinished, this, &MainWindow::onScreenerFinished));
    connectInstrumentFeeds();
    static_cast<void>(connect(m_feeds, &MarketFeeds::intradayCloses, this,
                              [this](const QString &symbol, const QList<double> &closes) {
                                  static_cast<void>(m_intradayBySymbol.insert(symbol, closes));
                              }));
    connectWorkerResults();

    static_cast<void>(connect(m_buyButton, &QPushButton::clicked, this, &MainWindow::onBuyClicked));
    static_cast<void>(connect(m_sellButton, &QPushButton::clicked, this, &MainWindow::onSellClicked));
    static_cast<void>(
        connect(m_closeButton, &QPushButton::clicked, this, &MainWindow::onCloseClicked));

    // Watch application-wide key presses for the double-tap s/b buy/sell shortcut.
    qApp->installEventFilter(this);

    // Cooldown between orders: while it runs the buy/sell buttons are disabled and
    // placeOrder() rejects any order (manual or auto). Re-enable them when it ends.
    m_orderCooldownTimer->setSingleShot(true);
    m_orderCooldownTimer->setInterval(kOrderCooldownMs);
    static_cast<void>(connect(m_orderCooldownTimer, &QTimer::timeout, this, [this] {
        // Re-enable via the shared path so a closed market keeps the buttons disabled.
        updateTradeButtonsEnabled();
    }));

    // Economic calendar: macro events for the regions that move the selected
    // instrument. Scope it to the startup instrument before the first fetch.
    static_cast<void>(
        connect(m_calendar, &EconomicCalendar::eventsUpdated, this, &MainWindow::onEvents));
    static_cast<void>(connect(m_calendar, &EconomicCalendar::log, this, &MainWindow::onLog));
    // MarketFeeds failures are throttled log lines (a dead VIX/news source must
    // not be silent); the calendar is started by the composition root, after
    // these connections exist.
    static_cast<void>(connect(m_feeds, &MarketFeeds::log, this, &MainWindow::onLog));
    // Age events out of the list ~10 min after they pass, without waiting for the
    // calendar's (30-min) re-fetch. Rebuilds only when an event actually drops off.
    m_eventTimer->setInterval(30 * 1000);  // 30 s granularity on the 10-min rule
    static_cast<void>(
        connect(m_eventTimer, &QTimer::timeout, this, [this] { rebuildEventsView(false); }));
    m_eventTimer->start();

    // After a trade closes, wait ~10 s (so eToro's trade-history API reflects it) then
    // refresh the closed-trade P/L. Single-shot and restarted per close, so closing
    // several marked trades at once triggers just one fetch after the last one.
    m_pnlAfterCloseTimer->setSingleShot(true);
    m_pnlAfterCloseTimer->setInterval(10 * 1000);
    static_cast<void>(connect(m_pnlAfterCloseTimer, &QTimer::timeout, this,
                              [this] { m_client->fetchClosedTrades(closedLookbackWeeks()); }));

    // "Buy / sell now": scan once at startup, then refresh in the background every few
    // minutes. The scan shares eToro's small rate pool with the price poll, so keep the
    // cadence conservative; the Refresh button covers on-demand updates in between.
    m_recoTimer->setInterval(5 * 60 * 1000);
    static_cast<void>(
        connect(m_recoTimer, &QTimer::timeout, this, &MainWindow::startRecommendationScan));
    m_recoTimer->start();
    // The first scan is kicked off from onReady() (once ids are resolving) rather than
    // a fixed delay, so it works whether the app comes up in real or simulation mode.

    // Show the chart and the signals & AI window once the main window is up,
    // offset to the side so they don't cover the controls. The user can drag
    // them anywhere (incl. a 2nd screen).
    QTimer::singleShot(0, this, [this] {
        m_chart->move(frameGeometry().topRight() + QPoint(16, 0));
        m_chart->show();
        m_chart->raise();  // bring it to the front on first appearance
        if (m_signalsWindow != nullptr) {
            m_signalsWindow->move(m_chart->frameGeometry().bottomLeft() + QPoint(0, 16));
            m_signalsWindow->show();
            m_signalsWindow->raise();
        }
    });
}

// Scripted trading (REQ-F-028): the runner exists from construction so an
// armed script keeps executing while its window is closed. It obeys the SAME
// open-exposure cap as manual orders, through the gate injected here — the
// cap and the committed totals belong to the trade panel's guard set.
void MainWindow::setupRunners()
{
    // Both autonomous runners are built here, and both outlive their windows: an
    // armed script keeps placing broker-side orders and the bot simulation keeps
    // its books whether or not anyone is looking at them.

    // --- the paper-trading bot (REQ-F-029) ---------------------------------
    // Reads quotes/spreads/fees and the decision rows the scan produces; it has no
    // route to an order endpoint, which is what makes "simulated money only" a
    // property of the design rather than a promise.
    // The local-model advisor (REQ-F-030) is optional: without an ollamaModel in
    // the configuration it reports itself unconfigured and the bot runs on the
    // composite alone. It is owned here and handed to the runner.
    m_ollama = new OllamaAdvisor(m_client->config().ollamaHost, m_client->config().ollamaModel, this);
    m_botRunner = new BotSimRunner(m_client, m_ollama, this);
    m_botRunner->applyDailyRules(m_client->config().botDailyTarget,
                                 m_client->config().botDailyLossLimit);
    static_cast<void>(connect(m_botRunner, &BotSimRunner::log, this, &MainWindow::onLog));
    static_cast<void>(connect(m_botRunner, &BotSimRunner::tradeOpened, this,
                              &MainWindow::onBotTradeOpened));
    // The local model is a SOURCE like any other, not just the bot's brain: its
    // picks show up in the signals panel and in the decision window (REQ-F-034).
    static_cast<void>(connect(m_botRunner, &BotSimRunner::proposalsUpdated, this,
                              &MainWindow::onLocalModelProposals));
    // Unattended experiments (and the headless QA run): TRADINGAPP_BOT_ARM=1 arms
    // the simulation at startup, so a machine left running — a Raspberry Pi, say —
    // collects days of paper results without anyone clicking anything. Safe by
    // construction: the bot has no route to a real order (REQ-F-029).
    if (qEnvironmentVariableIsSet("TRADINGAPP_BOT_ARM")) {
        m_botRunner->setArmed(true);
    }
    // …and TRADINGAPP_BOT_AI=off|confirm|lead selects how the local model's
    // proposal is used (REQ-F-030), so an unattended experiment can be started in
    // the mode being measured without a click.
    const QString aiMode = qEnvironmentVariable("TRADINGAPP_BOT_AI").trimmed().toLower();
    if (aiMode == QStringLiteral("confirm")) {
        m_botRunner->setAiMode(trading::BotAiMode::Confirm);
    } else if (aiMode == QStringLiteral("lead")) {
        m_botRunner->setAiMode(trading::BotAiMode::Lead);
    } else if (aiMode == QStringLiteral("off")) {
        m_botRunner->setAiMode(trading::BotAiMode::Off);
    }

    // --- the trade-script runner (REQ-F-028) -------------------------------
    m_scriptRunner = new TradeScriptRunner(m_client, this);
    static_cast<void>(
        connect(m_scriptRunner, &TradeScriptRunner::log, this, &MainWindow::onLog));
    m_scriptRunner->setExposureGate([this](double amount, QString *whyNot) {
        const double committed = m_openTradesTotal + pendingExposureTotal();
        if ((committed + amount) > (kMaxOpenExposure + 1e-6)) {
            if (whyNot != nullptr) {
                *whyNot = QStringLiteral("open trades plus resting orders would reach "
                                         "%1%2, over the %1%3 exposure limit")
                              .arg(m_ccy)
                              .arg(toDisplay(committed + amount), 0, 'f', 2)
                              .arg(toDisplay(kMaxOpenExposure), 0, 'f', 2);
            }
            return false;
        }
        return true;
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // The chart and the signals & AI panel are separate top-level windows;
    // close them too so the app exits.
    if (m_chart != nullptr) {
        static_cast<void>(m_chart->close());
    }
    if (m_signalsWindow != nullptr) {
        static_cast<void>(m_signalsWindow->close());
    }
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Thin dispatcher: each leg reports whether it consumed the event; anything
    // not consumed falls through to Qt (typing in spin boxes depends on that).
    if (handleZoomWheel(watched, event)) {
        return true;
    }

    // Reflect each auxiliary window's own show/hide (e.g. its title-bar X) in
    // the matching header toggle.
    QPushButton *toggle = nullptr;
    if (watched == m_chart) {
        toggle = m_chartToggle;
    } else if (watched == m_signalsWindow) {
        toggle = m_signalsToggle;
    } else {
        // not one of the toggled windows
    }
    if (toggle != nullptr) {
        if ((event->type() == QEvent::Close) || (event->type() == QEvent::Hide)) {
            const QSignalBlocker block(toggle);
            toggle->setChecked(false);
        } else if (event->type() == QEvent::Show) {
            const QSignalBlocker block(toggle);
            toggle->setChecked(true);
        } else {
            // other events on these windows are of no interest here
        }
    }
    if (handleQuickKeyEvent(event)) {
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::handleZoomWheel(QObject *watched, QEvent *event)
{
    // Ctrl + mouse wheel zooms the whole UI — both windows' size and all fonts.
    // Requires an active app window (i.e. the user has clicked into the app). The
    // chart is skipped: its own Ctrl+wheel zooms the price/time axis, which we keep.
    if (event->type() == QEvent::Wheel) {
        // Qt idiom: event->type() is checked above, so static_cast is the
        // supported downcast here (see pro-type-static-cast-downcast note in
        // .clang-tidy).
        auto *we = static_cast<QWheelEvent *>(event);
        const bool ctrlHeld = we->modifiers().testFlag(Qt::ControlModifier);
        const qint32 wheelDelta = we->angleDelta().y();
        if (ctrlHeld && (wheelDelta != 0) && (QApplication::activeWindow() != nullptr)) {
            QWidget *w = qobject_cast<QWidget *>(watched);
            const QWidget *top = (w != nullptr) ? w->window() : nullptr;
            if ((top != nullptr) && (top != m_chart)) {
                const double steps = static_cast<double>(wheelDelta) / 120.0;  // one notch = 120
                setUiScale(m_uiScale * std::pow(1.1, steps));  // wheel up → larger
                return true;  // consume so the widget under the cursor doesn't scroll
            }
        }
    }
    return false;
}

bool MainWindow::handleQuickKeyEvent(QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);  // guarded by type() above
        if (!ke->isAutoRepeat() && ((ke->key() == Qt::Key_S) || (ke->key() == Qt::Key_B))) {
            QWidget *fw = QApplication::focusWidget();
            // Numeric spin fields (amount, SL/TP, limit rates) reject letters
            // anyway, so the quick keys stay live there — that is exactly where focus
            // sits after typing an amount (and after startup). A spin box edits
            // through an internal QLineEdit, so recognize it via the parent too.
            const bool inSpinBox =
                (qobject_cast<QAbstractSpinBox *>(fw) != nullptr)
                || ((fw != nullptr)
                    && (qobject_cast<QAbstractSpinBox *>(fw->parentWidget()) != nullptr));
            // Text-capable editors keep the key: a combo's type-ahead search and real
            // line edits genuinely consume letters. An *open* combo dropdown is the
            // tricky case — focus is on the popup's internal list view, so the casts
            // miss it; activePopupWidget() catches any open popup. And no quick-
            // trading from under a modal dialog: keys there belong to the dialog.
            const bool blocked = (QApplication::activePopupWidget() != nullptr)
                                 || (QApplication::activeModalWidget() != nullptr);
            const bool typingText = !inSpinBox
                                    && ((qobject_cast<QComboBox *>(fw) != nullptr)
                                        || (qobject_cast<QLineEdit *>(fw) != nullptr));
            if (!blocked && !typingText) {
                handleQuickKey(ke->key());
                return true;  // consume so it doesn't reach e.g. table keyboard-search
            }
            if (!blocked) {
                // Swallowed by a text editor — say so instead of failing silently,
                // otherwise the shortcut just seems dead.
                appendLog(QStringLiteral("%1 ignored — the cursor is in a text field; "
                                         "click somewhere neutral first, then double-tap.")
                              .arg(ke->key() == Qt::Key_B ? QStringLiteral("B")
                                                          : QStringLiteral("S")));
            }
        }
    }
    return false;
}

namespace {
// The AI advisor's call as a direction the script runner understands
// (+1 BUY / -1 SELL / 0 anything else, including HOLD and errors).
qint32 aiDecisionDir(const AiDecision &d)
{
    if (!d.ok) {
        return 0;
    }
    if (d.action == QStringLiteral("BUY")) {
        return 1;
    }
    return (d.action == QStringLiteral("SELL")) ? -1 : 0;
}
} // namespace

void MainWindow::handleQuickKey(qint32 key)
{
    constexpr qint64 kDoubleTapMs = 1000;  // two taps within one second
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if ((key == m_quickKey) && ((now - m_quickKeyMs) <= kDoubleTapMs)) {
        m_quickKey = 0;  // consumed — the next trigger needs two fresh taps
        // The keyboard shortcut is itself the double-confirm, so place directly
        // (bypassing the buttons' own double-press gate).
        placeOrder(key != Qt::Key_S);
    } else {
        m_quickKey = key;
        m_quickKeyMs = now;
        // Mirror the buttons' arming feedback. This line is also the tell that the
        // key registered at all: with focus in an input field the shortcut is
        // deliberately swallowed, and this line then stays absent from the log.
        const bool isBuy = (key != Qt::Key_S);
        appendLog(QStringLiteral("Press %1 again within 1 s to place %2.")
                      .arg(isBuy ? QStringLiteral("B") : QStringLiteral("S"),
                           isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL")));
    }
}

void MainWindow::onLeverageOptions(const QList<int> &values)
{
    if (values.isEmpty() || (m_leverage == nullptr)) {
        return;
    }

    qint32 maxLeverage = 0;
    {
        const QSignalBlocker blocker(m_leverage);  // repopulate without spurious updates
        m_leverage->clear();
        for (const qint32 v : values) {
            m_leverage->addItem(QString::number(v));
            maxLeverage = std::max(maxLeverage, v);
        }
        m_leverage->setCurrentText(QString::number(maxLeverage));  // default to the max
    }
    updateSignals();  // refresh risk/edge for the new leverage (signals were blocked)
    appendLog(QStringLiteral("Max leverage for %1 is x%2 — leverage set to it.")
                  .arg(m_client->instrument().symbol)
                  .arg(maxLeverage));
}

void MainWindow::onTradeability(const QSet<QString> &tradeableSymbols)
{
    const QString cur = m_client->config().symbol;
    const bool wasOpen = !m_tradeabilityKnown || m_tradeableNow.contains(cur);
    const bool setChanged = !m_tradeabilityKnown || tradeableSymbols != m_tradeableNow;
    m_tradeableNow = tradeableSymbols;
    m_tradeabilityKnown = true;
    // Log only on a transition for the current instrument, so the console isn't spammed
    // by the periodic re-check when nothing changed.
    const bool nowOpen = m_tradeableNow.contains(cur);
    if (nowOpen != wasOpen) {
        appendLog(nowOpen ? QStringLiteral("%1 market opened — trading enabled.").arg(cur)
                          : QStringLiteral("%1 market closed — BUY/SELL disabled.").arg(cur));
    }
    updateTradeButtonsEnabled();
    // Keep the "Buy / sell now" panel to open markets only; rebuild when the set changes
    // (a market opened/closed) so a closed instrument drops off without waiting for a scan.
    if (setChanged) {
        rebuildRecommendations();
        updateDecisionOpenColumn();  // ranked table's "Open" cells follow live
    }
}

void MainWindow::updateTradeButtonsEnabled()
{
    if ((m_buyButton == nullptr) || (m_sellButton == nullptr)) {
        return;
    }
    const QString cur = m_client->config().symbol;
    // Unknown state (before the first eligibility check) counts as open so trading is
    // never blocked just because the check hasn't landed yet.
    const bool marketOpen = !m_tradeabilityKnown || m_tradeableNow.contains(cur);
    const bool overridden = marketClosedOverridden();  // user vouches the market is trading
    const bool cooldown = (m_orderCooldownTimer != nullptr) && m_orderCooldownTimer->isActive();
    const bool enabled = (marketOpen || overridden) && !cooldown && !m_instrumentResolving;
    m_buyButton->setEnabled(enabled);
    m_sellButton->setEnabled(enabled);
    // The limit buttons ignore the market-open verdict — a resting order is meant to be
    // placed ahead of a session — but still respect the cooldown and the resolution gate
    // (the order carries an instrumentId, so it must be the confirmed one).
    if (m_limitBuyButton != nullptr) {
        const bool limitEnabled = !cooldown && !m_instrumentResolving;
        m_limitBuyButton->setEnabled(limitEnabled);
        m_limitSellButton->setEnabled(limitEnabled);
    }
    // The warning row stays up while overriding — the override is a deliberate exception,
    // not a reason to hide that the app still reads this market as closed.
    if (m_marketClosedRow != nullptr) {
        m_marketClosedRow->setVisible(m_tradeabilityKnown && !marketOpen);
    }
    if (m_instrumentResolving) {
        const QString tip = QStringLiteral("%1 is still resolving — trading unlocks once the "
                                           "instrument is confirmed.").arg(cur);
        m_buyButton->setToolTip(tip);
        m_sellButton->setToolTip(tip);
    } else if (overridden) {
        const QString tip = QStringLiteral("%1 reads closed, but \"Trade anyway\" is armed — "
                                           "orders WILL be submitted (press twice within "
                                           "650 ms). eToro rejects them if the market really "
                                           "is closed.").arg(cur);
        m_buyButton->setToolTip(tip);
        m_sellButton->setToolTip(tip);
    } else if (!marketOpen) {
        const QString tip = QStringLiteral("%1's market is currently closed — eToro does not "
                                           "accept opening orders right now. Tick \"Trade "
                                           "anyway\" if you know it is trading.").arg(cur);
        m_buyButton->setToolTip(tip);
        m_sellButton->setToolTip(tip);
    } else {
        const QString dblTip = QStringLiteral(
            "Press twice within 650 ms to place the order (a single press only arms it).");
        m_buyButton->setToolTip(dblTip);
        m_sellButton->setToolTip(dblTip);
    }
}

QGroupBox *MainWindow::buildLimitOrderBox(QWidget *parent)
{
    // A limit order is placed AT ETORO (API orderType "mit" + triggerRate): the broker
    // watches its own feed and executes at market once the rate is touched. That is why
    // this replaced the app's old "armed" price watch — the watch needed the app running,
    // and it fired off quotes this app polls minutes behind the real market (REQ-F-027).
    auto *box = new QGroupBox(QStringLiteral("Limit orders (placed at eToro)"), parent);
    auto *form = new QFormLayout(box);

    const QString rateTip =
        QStringLiteral("Rate at which eToro shall open the trade. eToro fills at \"this rate or "
                       "better\" — lower for a buy, higher for a sell — so a buy waits for the "
                       "price to fall to it and a sell for it to rise. A rate already on that "
                       "side of the market can fill immediately; the log warns first. Size, "
                       "leverage, SL and TP come from the trade panel above, and the SL/TP "
                       "amounts are measured from THIS rate, because that is where the position "
                       "opens. eToro holds the order and executes it even with this app closed.");

    m_limitBuyRate = new QDoubleSpinBox(box);
    m_limitBuyRate->setRange(0.0, 10'000'000.0);
    m_limitBuyRate->setDecimals(2);
    m_limitBuyRate->setSpecialValueText(QStringLiteral("off"));  // shown when 0
    m_limitBuyRate->setToolTip(rateTip);
    m_limitBuyButton = new QPushButton(QStringLiteral("Place limit BUY"), box);
    m_limitBuyButton->setToolTip(
        QStringLiteral("Send a BUY limit order to eToro (press twice within 650 ms)."));
    static_cast<void>(connect(m_limitBuyButton, &QPushButton::clicked, this,
                              [this] { handleOrderButton(true, /*limit=*/true); }));
    auto *buyRow = new QHBoxLayout;
    buyRow->addWidget(m_limitBuyRate, 1);
    buyRow->addWidget(m_limitBuyButton, 0);
    form->addRow(QStringLiteral("Buy at rate"), buyRow);

    m_limitSellRate = new QDoubleSpinBox(box);
    m_limitSellRate->setRange(0.0, 10'000'000.0);
    m_limitSellRate->setDecimals(2);
    m_limitSellRate->setSpecialValueText(QStringLiteral("off"));
    m_limitSellRate->setToolTip(rateTip);
    m_limitSellButton = new QPushButton(QStringLiteral("Place limit SELL"), box);
    m_limitSellButton->setToolTip(
        QStringLiteral("Send a SELL (short) limit order to eToro (press twice within 650 ms)."));
    static_cast<void>(connect(m_limitSellButton, &QPushButton::clicked, this,
                              [this] { handleOrderButton(false, /*limit=*/true); }));
    auto *sellRow = new QHBoxLayout;
    sellRow->addWidget(m_limitSellRate, 1);
    sellRow->addWidget(m_limitSellButton, 0);
    form->addRow(QStringLiteral("Sell at rate"), sellRow);

    buildPendingOrdersView(box, form);

    // The table is a pure view of the client's resting-order list; wired here, with the
    // widgets it feeds, rather than among the constructor's other client connections.
    static_cast<void>(connect(m_client, &EtoroClient::pendingOrdersUpdated, this,
                              &MainWindow::onPendingOrders));
    return box;
}

void MainWindow::buildPendingOrdersView(QWidget *parent, QFormLayout *form)
{
    // What is actually resting at the broker right now. eToro publishes no "list my
    // open orders" endpoint, so this shows the orders THIS app placed (and their live
    // status from the per-order lookup) — see EtoroClient::pendingOrders().
    m_pendingTable = new QTableWidget(0, kPendingColumns, parent);
    m_pendingTable->setHorizontalHeaderLabels({QStringLiteral("Side"),
                                               QStringLiteral("Instr."),
                                               QStringLiteral("Trigger"),
                                               QStringLiteral("Now"),
                                               QStringLiteral("Amount (%1)").arg(m_ccy),
                                               QStringLiteral("SL (%1)").arg(m_ccy),
                                               QStringLiteral("TP (%1)").arg(m_ccy),
                                               QStringLiteral("Status")});
    // Eight columns do not fit a stretched panel width without truncating the headers, so
    // each is sized to its content and the last one takes the slack.
    m_pendingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_pendingTable->horizontalHeader()->setStretchLastSection(true);
    m_pendingTable->verticalHeader()->setVisible(false);
    m_pendingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pendingTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pendingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pendingTable->setMaximumHeight(120);
    m_pendingTable->setToolTip(QStringLiteral(
        "The account's limit orders still waiting at eToro — including ones placed in a "
        "previous session or in eToro's own interface, since they come from the broker's "
        "portfolio. \"Now\" is the instrument's current rate, so how far the order still "
        "has to travel is visible at a glance. Click a Trigger / Now / SL / TP cell (or "
        "use \"Adjust…\") to change the order's values — eToro cannot change a resting "
        "order, so it is cancelled and placed again under a new id. Click an order's Instr. "
        "cell to trade and chart that instrument."));
    // Click routing exactly like the open-trades table: the Instrument cell switches the
    // app to that order's instrument, the value columns open the editor, and any other
    // cell just selects the row (which is what the Cancel button acts on). The switch
    // matters because an order can only be adjusted while its own instrument is the one
    // being traded — the re-placement is priced from it.
    static_cast<void>(connect(m_pendingTable, &QTableWidget::cellClicked, this,
                              [this](int row, int column) {
                                  if (column == kPendingInstrumentColumn) {
                                      switchToPendingOrderInstrument(row);
                                  } else if ((column >= kPendingTriggerColumn)
                                             && (column <= kPendingTpColumn)) {
                                      openPendingOrderEditor(row);
                                  }
                              }));
    form->addRow(m_pendingTable);

    m_editPendingButton = new QPushButton(QStringLiteral("Adjust SL / TP…"), parent);
    m_editPendingButton->setEnabled(false);
    m_editPendingButton->setToolTip(
        QStringLiteral("Change the selected resting order's trigger rate, stop loss and take "
                       "profit. eToro cannot change a resting order, so it is cancelled and "
                       "placed again with the new values — it returns under a new order id. "
                       "Works for any listed order, whichever instrument is on screen."));
    static_cast<void>(connect(m_editPendingButton, &QPushButton::clicked, this,
                              [this] { openPendingOrderEditor(selectedPendingRow()); }));

    m_cancelPendingButton = new QPushButton(QStringLiteral("Cancel selected limit order"), parent);
    m_cancelPendingButton->setEnabled(false);
    m_cancelPendingButton->setToolTip(
        QStringLiteral("Ask eToro to cancel the selected resting order. It can fail if the "
                       "order just triggered — the status line then says what happened."));
    static_cast<void>(connect(m_cancelPendingButton, &QPushButton::clicked, this,
                              &MainWindow::cancelSelectedPendingOrder));

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_editPendingButton, 1);
    buttons->addWidget(m_cancelPendingButton, 1);
    form->addRow(buttons);

    // Track the mark by order id, not by row: the 4 s refresh rebuilds the rows, and an
    // order that triggered meanwhile shifts every row below it.
    static_cast<void>(connect(m_pendingTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QList<QTableWidgetItem *> marked = m_pendingTable->selectedItems();
        const qsizetype row = marked.isEmpty() ? -1 : marked.constFirst()->row();
        m_selectedPendingId = ((row >= 0) && (row < m_pendingShown.size()))
                                  ? m_pendingShown[row].orderId
                                  : QString();
        const bool hasSelection = !m_selectedPendingId.isEmpty();
        m_cancelPendingButton->setEnabled(hasSelection);
        m_editPendingButton->setEnabled(hasSelection);
    }));
}

void MainWindow::switchToPendingOrderInstrument(qint32 row)
{
    if ((row < 0) || (row >= m_pendingShown.size())) {
        return;
    }
    const PendingOrder &order = m_pendingShown[row];
    // "#<id>" means the order sits on an instrument outside the app's selector: it can be
    // listed, adjusted and cancelled, but there is nothing here to switch the app to.
    if (order.symbol.startsWith(QLatin1Char('#'))) {
        appendLog(QStringLiteral("Limit order %1 is on instrument %2, which is not in this "
                                 "app's instrument list — it can be adjusted and cancelled "
                                 "here, but not charted.")
                      .arg(order.orderId, order.symbol),
                  true);
        return;
    }
    if (order.symbol.compare(m_client->config().symbol, Qt::CaseInsensitive) == 0) {
        return;  // already the traded instrument — nothing to do
    }
    appendLog(QStringLiteral("Switching to %1 — the instrument of limit order %2.")
                  .arg(order.symbol, order.orderId));
    selectInstrument(order.symbol);
}

void MainWindow::updatePendingOrderRates()
{
    if ((m_pendingTable == nullptr)
        || (m_pendingTable->rowCount() != static_cast<qint32>(m_pendingShown.size()))) {
        return;
    }
    // In place, cell by cell — this runs on every price tick, and rebuilding the table
    // that often would fight the user's selection and churn allocations (REQ-N-006).
    for (qsizetype row = 0; row < m_pendingShown.size(); ++row) {
        const PendingOrder &order = m_pendingShown[row];
        QTableWidgetItem *cell = m_pendingTable->item(static_cast<qint32>(row),
                                                     kPendingNowColumn);
        if (cell == nullptr) {
            continue;
        }
        const double now = m_client->lastRateFor(order.instrumentId);
        if (now <= 0.0) {
            cell->setText(QStringLiteral("—"));  // no quote for that instrument yet
            cell->setToolTip(QString());
            continue;
        }
        cell->setText(QLocale().toString(now, 'f', trading::priceDecimals(now)));
        // How far the market still has to move for eToro to release this order.
        const double away = order.isBuy ? (now - order.triggerRate) : (order.triggerRate - now);
        const double pct = (now > 0.0) ? ((away / now) * 100.0) : 0.0;
        cell->setToolTip(
            (away > 0.0)
                ? QStringLiteral("%1 still has to %2 by %3 (%4%) to reach the trigger.")
                      .arg(order.symbol,
                           order.isBuy ? QStringLiteral("fall") : QStringLiteral("rise"))
                      .arg(std::abs(away), 0, 'f', trading::priceDecimals(now))
                      .arg(std::abs(pct), 0, 'f', 2)
                : QStringLiteral("%1 is already at or past the trigger — eToro fills at "
                                 "\"trigger or better\", so this order can go at any moment.")
                      .arg(order.symbol));
    }
}

qint32 MainWindow::selectedPendingRow() const
{
    for (qsizetype row = 0; row < m_pendingShown.size(); ++row) {
        if (m_pendingShown[row].orderId == m_selectedPendingId) {
            return static_cast<qint32>(row);
        }
    }
    return -1;
}

void MainWindow::onPendingOrders(const QList<PendingOrder> &orders)
{
    m_pendingShown = orders;
    rebuildPendingOrdersTable();
}

void MainWindow::rebuildPendingOrdersTable()
{
    if (m_pendingTable == nullptr) {
        return;
    }
    // The list is re-read every 4 s, so a rebuild must not silently drop the row the user
    // marked for Cancel / Adjust. The mark is tracked as an ORDER ID (m_selectedPendingId)
    // and restored below; the blocker keeps our own row edits from rewriting it.
    const QSignalBlocker block(m_pendingTable);
    m_pendingTable->setRowCount(static_cast<qint32>(m_pendingShown.size()));
    for (qsizetype row = 0; row < m_pendingShown.size(); ++row) {
        const PendingOrder &order = m_pendingShown[row];
        const auto put = [this, row](qint32 column, const QString &text) {
            auto *cell = new QTableWidgetItem(text);
            cell->setTextAlignment((column == 0) || (column == 1) ? Qt::AlignLeft | Qt::AlignVCenter
                                                                  : Qt::AlignRight
                                                                        | Qt::AlignVCenter);
            m_pendingTable->setItem(static_cast<qint32>(row), column, cell);
        };
        put(0, order.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
        put(1, order.symbol);
        put(2, QLocale().toString(order.triggerRate, 'f',
                                 trading::priceDecimals(order.triggerRate)));
        put(kPendingNowColumn, QString());  // filled by updatePendingOrderRates() below
        // The order carries account-currency figures; the panel talks euro. An unset
        // SL/TP leg reads "—" rather than 0, which would look like a zero-distance stop.
        const auto amountText = [this](double usd) {
            return (usd > 0.0) ? QLocale().toString(toDisplay(usd), 'f', 2)
                               : QStringLiteral("—");
        };
        put(4, QLocale().toString(toDisplay(order.amount), 'f', 2));
        put(5, amountText(order.stopLossAmount));
        put(6, amountText(order.takeProfitAmount));
        put(7, order.status);
    }
    updatePendingOrderRates();

    const qint32 row = selectedPendingRow();
    if (row >= 0) {
        m_pendingTable->selectRow(row);
    } else {
        m_selectedPendingId.clear();  // that order has triggered, been cancelled or refused
    }
    const bool hasSelection = !m_selectedPendingId.isEmpty();
    m_cancelPendingButton->setEnabled(hasSelection);
    m_editPendingButton->setEnabled(hasSelection);
}

void MainWindow::openPendingOrderEditor(qint32 row)
{
    if ((row < 0) || (row >= m_pendingShown.size())) {
        appendLog(QStringLiteral("Select a resting limit order first, then adjust it."));
        return;
    }
    const PendingOrder order = m_pendingShown[row];
    const QString side = order.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Adjust limit %1 %2 — order %3")
                              .arg(side, order.symbol, order.orderId));
    auto *layout = new QVBoxLayout(&dialog);
    auto *fields = new QFormLayout;

    const qint32 decimals = trading::priceDecimals(order.triggerRate);
    auto *rate = new QDoubleSpinBox(&dialog);
    rate->setRange(0.01, 10'000'000.0);
    rate->setDecimals(decimals);
    rate->setSingleStep(std::pow(10.0, -decimals + 1));
    rate->setValue(order.triggerRate);
    rate->setToolTip(QStringLiteral("The rate eToro shall open this trade at."));
    fields->addRow(QStringLiteral("Trigger rate"), rate);

    // SL/TP are entered as display-currency amounts, exactly like the trade panel; the
    // client converts them to rates measured from the (possibly new) trigger rate.
    const auto addAmount = [&dialog, fields, this](const QString &label, double usd) {
        auto *field = new QDoubleSpinBox(&dialog);
        field->setRange(0.0, 1'000'000.0);
        field->setDecimals(2);
        field->setSpecialValueText(QStringLiteral("none"));  // shown at 0
        field->setValue(toDisplay(usd));
        fields->addRow(QStringLiteral("%1 (%2)").arg(label, m_ccy), field);
        return field;
    };
    QDoubleSpinBox *stopLoss = addAmount(QStringLiteral("Stop loss"), order.stopLossAmount);
    QDoubleSpinBox *takeProfit = addAmount(QStringLiteral("Take profit"), order.takeProfitAmount);
    layout->addLayout(fields);

    auto *note = new QLabel(
        QStringLiteral("%1, size %2%3 and leverage x%4 stay as they are. eToro cannot change a "
                       "resting order, so this one is CANCELLED first and then placed again with "
                       "the values above — it comes back under a new order id, and for a moment "
                       "nothing rests at the broker.")
            .arg(order.symbol, m_ccy)
            .arg(toDisplay(order.amount), 0, 'f', 2)
            .arg(order.leverage),
        &dialog);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#777"));
    layout->addWidget(note);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Replace order"));
    static_cast<void>(connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept));
    static_cast<void>(connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject));
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    appendLog(QStringLiteral("Adjusting limit %1 %2 (order %3): rate %4, SL %5%6 / TP %5%7 — "
                             "cancelling and re-placing…")
                  .arg(side, order.symbol, order.orderId)
                  .arg(rate->value(), 0, 'f', decimals)
                  .arg(m_ccy)
                  .arg(stopLoss->value(), 0, 'f', 0)
                  .arg(takeProfit->value(), 0, 'f', 0));
    m_client->modifyPendingOrder(order.orderId, rate->value(), fromDisplay(stopLoss->value()),
                                 fromDisplay(takeProfit->value()));
}

void MainWindow::cancelSelectedPendingOrder()
{
    const qint32 row = selectedPendingRow();
    if (row < 0) {
        appendLog(QStringLiteral("Select a resting limit order first, then cancel it."));
        return;
    }
    const PendingOrder &order = m_pendingShown[row];
    appendLog(QStringLiteral("Cancelling limit %1 %2 @ %3 (order %4)…")
                  .arg(order.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"), order.symbol)
                  .arg(order.triggerRate, 0, 'f', trading::priceDecimals(order.triggerRate))
                  .arg(order.orderId));
    m_client->cancelPendingOrder(order.orderId);
}

QWidget *MainWindow::buildMarketClosedRow(QWidget *parent)
{
    // Shown only when the selected instrument's market is currently closed (eToro
    // rejects opening orders then); the BUY/SELL buttons are disabled alongside it.
    m_marketClosedLabel = new QLabel(
        QStringLiteral("⚠ Market closed — opening trades unavailable right now"), parent);
    m_marketClosedLabel->setStyleSheet(QStringLiteral("color:#e35555; font-weight:bold;"));
    m_marketClosedLabel->setAlignment(Qt::AlignCenter);
    m_marketClosedLabel->setWordWrap(true);
    // The verdict is inferred from a broker feed we do not control, so it can be wrong
    // (it was: a delay added to the public rates feed read as "frozen quote" and locked
    // every instrument mid-session). This override is the way out — REQ-F-026.
    m_marketClosedOverride = new QCheckBox(QStringLiteral("Trade anyway"), parent);
    m_marketClosedOverride->setChecked(false);
    m_marketClosedOverride->setToolTip(QStringLiteral(
        "Re-enable BUY/SELL although the market reads closed. The open/closed state is "
        "inferred from whether eToro's quote timestamps keep advancing, so it can be "
        "wrong — tick this when you know the market is trading. eToro still rejects "
        "orders into a genuinely closed market, and the double-press confirmation "
        "stays in force. Clears itself when you switch instruments."));
    static_cast<void>(connect(m_marketClosedOverride, &QCheckBox::toggled, this,
                              [this](bool on) { onMarketClosedOverrideToggled(on); }));
    // One row, shown/hidden as a unit by updateTradeButtonsEnabled(): the warning is
    // pointless without its way out, and the override is meaningless without the warning.
    m_marketClosedRow = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(m_marketClosedRow);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->addWidget(m_marketClosedLabel, 1);
    rowLayout->addWidget(m_marketClosedOverride);
    m_marketClosedRow->setVisible(false);
    return m_marketClosedRow;
}

void MainWindow::onMarketClosedOverrideToggled(bool on)
{
    // Log both directions: an armed override changes what BUY/SELL will do, so it must
    // be visible in the console rather than only in a checkbox state.
    appendLog(on ? QStringLiteral("Market-closed override ARMED for %1 — BUY/SELL enabled "
                                  "despite the closed verdict.")
                       .arg(m_client->config().symbol)
                 : QStringLiteral("Market-closed override cleared."));
    updateTradeButtonsEnabled();
}

bool MainWindow::marketClosedOverridden() const
{
    if ((m_marketClosedOverride == nullptr) || !m_marketClosedOverride->isChecked()) {
        return false;
    }
    // Only an actual closed verdict is overridden. On an open market the checkbox is
    // hidden and irrelevant, so it must not read as "something is being bypassed".
    return m_tradeabilityKnown && !m_tradeableNow.contains(m_client->config().symbol);
}

QHBoxLayout *MainWindow::buildHeaderRow(QWidget *central, const QString &sym)
{
    // --- Header: instrument name + live price --------------------------------
    auto *header = new QHBoxLayout;
    m_titleLabel = new QLabel(sym, central);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 6);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);

    // Instrument selector, grouped by asset class. The item text is the eToro
    // internalSymbolFull used for lookup; picking one switches the whole app to
    // it. The universe itself lives in the domain InstrumentCatalog (single
    // source of truth) — the UI only renders it, one bold header per group.
    m_instrumentBox = new QComboBox(central);
    m_instrumentBox->setToolTip(QStringLiteral("Switch the traded instrument"));
    auto *instModel = new QStandardItemModel(m_instrumentBox);
    QString currentGroup;
    for (const trading::InstrumentSpec &spec : trading::instrumentCatalog()) {
        if (spec.group != currentGroup) {
            currentGroup = spec.group;
            auto *h = new QStandardItem(currentGroup);
            h->setFlags(Qt::NoItemFlags);  // non-selectable category header
            QFont hf = h->font();
            hf.setBold(true);
            h->setFont(hf);
            instModel->appendRow(h);
        }
        instModel->appendRow(new QStandardItem(spec.symbol));
    }
    m_instrumentBox->setModel(instModel);
    const QStringList tradableSymbols = trading::tradableSymbols();
    m_client->setTradableSymbols(tradableSymbols);  // for id resolution + portfolio filtering
    m_feeds->setTradableSymbols(tradableSymbols);   // for the bulk web-rating/news fetches
    const qint32 curIdx = m_instrumentBox->findText(m_client->config().symbol);
    if (curIdx >= 0) {
        m_instrumentBox->setCurrentIndex(curIdx);
    }
    // textActivated fires only on user selection, not the programmatic setup above.
    static_cast<void>(
        connect(m_instrumentBox, &QComboBox::textActivated, this, [this](const QString &picked) {
            m_autoInstrumentDone = true;  // a manual pick ends the startup auto-load
            selectInstrument(picked);
        }));

    m_priceLabel = new QLabel(QStringLiteral("—"), central);
    QFont priceFont = m_priceLabel->font();
    priceFont.setPointSize(priceFont.pointSize() + 6);
    priceFont.setBold(true);
    m_priceLabel->setFont(priceFont);
    m_priceLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // Available cash for trading, shown beneath the live price.
    m_cashLabel = new QLabel(QStringLiteral("Available for trading: —"), central);
    m_cashLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_cashLabel->setToolTip(QStringLiteral("Free funds available to open new positions"));

    auto *priceCol = new QVBoxLayout;
    priceCol->setSpacing(0);
    priceCol->addWidget(m_priceLabel);
    priceCol->addWidget(m_cashLabel);

    buildHeaderButtons(central);

    header->addWidget(m_titleLabel);
    header->addSpacing(12);
    header->addWidget(new QLabel(QStringLiteral("Instrument:"), central));
    header->addWidget(m_instrumentBox);
    header->addSpacing(8);
    header->addWidget(m_chartToggle);
    header->addWidget(m_signalsToggle);
    header->addWidget(m_screenerButton);
    header->addWidget(m_decisionButton);
    header->addWidget(m_scriptButton);
    header->addWidget(m_botButton);
    header->addWidget(m_closedButton);
    header->addStretch();
    header->addLayout(priceCol);
    return header;
}

void MainWindow::buildHeaderButtons(QWidget *central)
{
    // Small toggle to show/hide the (separate) chart window.
    m_chartToggle = new QPushButton(QStringLiteral("Graph"), central);
    m_chartToggle->setCheckable(true);
    m_chartToggle->setChecked(true);
    m_chartToggle->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    m_chartToggle->setMaximumWidth(96);
    m_chartToggle->setToolTip(QStringLiteral("Show or hide the price chart window"));
    static_cast<void>(connect(m_chartToggle, &QPushButton::toggled, this, [this](bool on) {
        if (m_chart == nullptr) {
            return;
        }
        if (on) {
            m_chart->show();
            m_chart->raise();
        } else {
            m_chart->hide();
        }
    }));

    // Leverage screener: ranks every instrument by max leverage with a buy/sell call.
    m_screenerButton = new QPushButton(QStringLiteral("Screener…"), central);
    m_screenerButton->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    m_screenerButton->setToolTip(QStringLiteral(
        "Rank every tradable instrument by its maximum leverage, each with a BUY/SELL "
        "signal from the same ensemble as the live panel. Double-click a row to trade it."));
    static_cast<void>(
        connect(m_screenerButton, &QPushButton::clicked, this, &MainWindow::openScreener));

    // Closed trades: the same window the closed-trades panel's "All trades…" button
    // opens, reachable from the header too — the history of the app's own instruments
    // is a top-level question, not a detail of the summary panel.
    m_closedButton = new QPushButton(QStringLiteral("Closed trades…"), central);
    m_closedButton->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    m_closedButton->setToolTip(QStringLiteral(
        "Every closed trade over a selectable 7–13-week lookback, restricted to the "
        "instruments this app trades (untick the filter in the window to see the whole "
        "account): side, leverage, invest, open/close rate, eToro's own net P/L and "
        "rollover fees, plus estimated open/close spread costs."));
    static_cast<void>(
        connect(m_closedButton, &QPushButton::clicked, this, &MainWindow::openClosedTrades));

    // Decision window: alternative sources + AI + algorithm → one final call.
    m_decisionButton = new QPushButton(QStringLiteral("Decision…"), central);
    m_decisionButton->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    m_decisionButton->setToolTip(QStringLiteral(
        "Cross-check several independent sources (technical ensemble, TradingView "
        "multi-timeframe rating, news sentiment, VIX/calendar regime, and — if configured "
        "— Claude AI) and get one final buy/sell recommendation with sizing."));
    static_cast<void>(
        connect(m_decisionButton, &QPushButton::clicked, this, &MainWindow::openDecision));

    // Trade script: load a file of conditional orders, dry-run it, arm it.
    m_scriptButton = new QPushButton(QStringLiteral("Script…"), central);
    m_scriptButton->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    m_scriptButton->setToolTip(QStringLiteral(
        "Scripted trading: load a text file of conditional orders (instrument, "
        "BUY/SELL @ trigger, optional time window and signals+AI condition, amount, "
        "SL/TP, leverage). Loading only shows a dry run; arming places the entries "
        "as broker-side limit orders."));
    static_cast<void>(
        connect(m_scriptButton, &QPushButton::clicked, this, &MainWindow::openScript));

    // Trading-bot simulation: paper money, live prices, no order ever placed.
    m_botButton = new QPushButton(QStringLiteral("Bot sim…"), central);
    m_botButton->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    m_botButton->setToolTip(QStringLiteral(
        "Trading-bot simulation (REQ-F-029): the app's own multi-source decision, traded "
        "across ALL instruments with SIMULATED money on live prices — spread, overnight "
        "fees and slippage-free fills charged like the real path, so the P/L is worth "
        "reading. It never places an order at eToro and never moves real funds."));
    static_cast<void>(
        connect(m_botButton, &QPushButton::clicked, this, &MainWindow::openBotSim));

    // Toggle for the combined signals + AI window (both panels moved out of the
    // main window into ONE floating, stay-on-top window shown at startup); same
    // show/hide pattern as the Graph toggle. The window is created further down,
    // hence the null check at toggle time.
    m_signalsToggle = new QPushButton(QStringLiteral("Signals && AI"), central);
    m_signalsToggle->setCheckable(true);
    m_signalsToggle->setChecked(true);  // window is shown once the app is up
    m_signalsToggle->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    m_signalsToggle->setToolTip(
        QStringLiteral("Show or hide the signals & AI window (per-indicator reads, "
                       "forecasts, the ensemble call and the AI decision support for "
                       "the traded instrument). It floats above the other windows."));
    static_cast<void>(connect(m_signalsToggle, &QPushButton::toggled, this, [this](bool on) {
        if (m_signalsWindow == nullptr) {
            return;
        }
        if (on) {
            m_signalsWindow->show();
            m_signalsWindow->raise();
        } else {
            m_signalsWindow->hide();
        }
    }));
}

void MainWindow::buildChartWindow(const QString &sym, qint32 chartW, qint32 chartH)
{
    // --- Price/time chart as its own top-level window ------------------------
    // Independent, PARENTLESS top-level window (native title bar): freely movable
    // by its title bar, resizable, and draggable to another monitor. It must not
    // be parented to the main window — a parented Qt::Window is a transient child
    // that shares/steals the parent's input focus, which blocks editing the
    // amount / stop-loss fields while the chart is open. Lifecycle is handled in
    // closeEvent (the chart is closed with the app).
    m_chart = new PriceChart(nullptr);
    m_chart->setWindowTitle(QStringLiteral("%1 — Price / time + change").arg(sym));
    // Keep the chart window above the other windows so it stays visible while the
    // user works in the controls (or another app).
    m_chart->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    m_chart->resize(chartW, chartH);
    // Keep the Graph toggle in sync if the chart is closed/hidden via its own title bar.
    m_chart->installEventFilter(this);
    // It is shown after the main window appears (see the constructor).
}

QGroupBox *MainWindow::buildTradePanel(QWidget *lower, const QString &sym)
{
    // Trade panel
    m_tradeBox = new QGroupBox(QStringLiteral("Trade %1").arg(sym), lower);
    auto *tradeBox = m_tradeBox;
    auto *tradeForm = new QFormLayout(tradeBox);

    // Approximate trading hours (local time) right under the "Trade <symbol>" title.
    m_tradeHours = new QLabel(tradeBox);
    m_tradeHours->setStyleSheet(QStringLiteral("color:#777"));
    m_tradeHours->setToolTip(QStringLiteral(
        "Approximate market hours for this instrument, shown in your local time. eToro's "
        "API does not publish trading hours, so these are a best-effort static schedule per "
        "asset class; actual CFD hours may extend beyond the underlying cash session."));
    tradeForm->addRow(m_tradeHours);
    updateTradeHours(sym);

    m_amount = new QDoubleSpinBox(tradeBox);
    m_amount->setRange(1.0, 10'000'000.0);
    m_amount->setDecimals(2);
    m_amount->setValue(3750.0);  // default stake per order
    m_amount->setSingleStep(10.0);
    m_amount->setPrefix(m_ccy + QLatin1Char(' '));
    m_amount->setToolTip(QStringLiteral("Cash amount to invest in this order"));
    tradeForm->addRow(QStringLiteral("Amount:"), m_amount);

    m_leverage = new QComboBox(tradeBox);
    m_leverage->addItems({QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("5"),
                          QStringLiteral("10"), QStringLiteral("20")});
    m_leverage->setCurrentText(QStringLiteral("20"));  // SPX500 max leverage
    tradeForm->addRow(QStringLiteral("Leverage (x):"), m_leverage);

    m_stopLoss = new QDoubleSpinBox(tradeBox);
    m_stopLoss->setRange(0.0, 10'000'000.0);
    m_stopLoss->setDecimals(2);
    m_stopLoss->setValue(130.0);  // placeholder until the volatility proposal lands
    m_stopLoss->setPrefix(m_ccy + QLatin1Char(' '));
    m_stopLoss->setToolTip(QStringLiteral(
        "Close the trade automatically at this loss (account currency).\n"
        "Auto-proposed from recent volatility (~1.5σ of a day's move for your amount "
        "and leverage) until you edit it by hand; switching instruments re-enables "
        "the automatic proposal."));

    m_trailingStop = new QCheckBox(QStringLiteral("Trailing"), tradeBox);
    m_trailingStop->setChecked(false);  // trailing stop-loss off by default
    m_trailingStop->setToolTip(QStringLiteral(
        "Trailing stop-loss: the stop follows the price as the trade moves in your "
        "favour (keeping the loss amount above as its distance) and never moves "
        "against you, locking in gains."));
    auto *slRow = new QWidget(tradeBox);
    auto *slRowLayout = new QHBoxLayout(slRow);
    slRowLayout->setContentsMargins(0, 0, 0, 0);
    slRowLayout->addWidget(m_stopLoss, 1);
    slRowLayout->addWidget(m_trailingStop);
    tradeForm->addRow(QStringLiteral("Stop loss:"), slRow);

    m_takeProfit = new QDoubleSpinBox(tradeBox);
    m_takeProfit->setRange(0.0, 10'000'000.0);
    m_takeProfit->setDecimals(2);
    m_takeProfit->setValue(290.0);  // placeholder until the volatility proposal lands
    m_takeProfit->setPrefix(m_ccy + QLatin1Char(' '));
    m_takeProfit->setToolTip(QStringLiteral(
        "Close the trade automatically at this profit (account currency).\n"
        "Auto-proposed at 1.5× the stop-loss (reward:risk 1.5) until you edit it by "
        "hand; switching instruments re-enables the automatic proposal."));
    tradeForm->addRow(QStringLiteral("Take profit:"), m_takeProfit);

    // A hand edit of either SL/TP field ends the automatic volatility proposal
    // (proposeSlTpDefaults sets m_settingSlTp while it fills programmatically).
    const auto slTpEdited = [this] {
        if (!m_settingSlTp) {
            m_slTpAuto = false;
        }
    };
    static_cast<void>(connect(m_stopLoss, &QDoubleSpinBox::valueChanged, this, slTpEdited));
    static_cast<void>(connect(m_takeProfit, &QDoubleSpinBox::valueChanged, this, slTpEdited));

    auto *buttonRow = new QHBoxLayout;
    m_buyButton = new QPushButton(QStringLiteral("BUY"), tradeBox);
    m_sellButton = new QPushButton(QStringLiteral("SELL"), tradeBox);
    m_buyButton->setMinimumHeight(40);
    m_sellButton->setMinimumHeight(40);
    m_buyButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#1f9d55; color:white; font-weight:bold; border-radius:4px; }"
        "QPushButton:hover { background:#25b563; }"
        "QPushButton:disabled { background:#3a3f44; color:#8a8a8a; }"));
    m_sellButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#d64545; color:white; font-weight:bold; border-radius:4px; }"
        "QPushButton:hover { background:#e35555; }"
        "QPushButton:disabled { background:#3a3f44; color:#8a8a8a; }"));
    const QString dblTip = QStringLiteral(
        "Press twice within 650 ms to place the order (a single press only arms it).");
    m_buyButton->setToolTip(dblTip);
    m_sellButton->setToolTip(dblTip);
    buttonRow->addWidget(m_buyButton);
    buttonRow->addWidget(m_sellButton);
    tradeForm->addRow(buttonRow);

    tradeForm->addRow(buildMarketClosedRow(tradeBox));

    buildTradeCostRows(tradeBox, tradeForm);

    tradeBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    return tradeBox;
}

void MainWindow::buildTradeCostRows(QGroupBox *tradeBox, QFormLayout *tradeForm)
{
    // Estimated opening cost: opening a CFD crosses the bid/ask spread, so the
    // position starts down by roughly spread × units. Shown per side (a buy fills
    // near the ask, a sell near the bid) and refreshed by updateOpenCost() as the
    // price, amount or leverage change.
    m_openCost = new QLabel(QStringLiteral("—"), tradeBox);
    m_openCost->setStyleSheet(QStringLiteral("color:#777"));
    m_openCost->setTextFormat(Qt::RichText);
    m_openCost->setToolTip(QStringLiteral(
        "Estimated cost to open this order at the current price — the bid/ask spread "
        "the trade crosses, i.e. roughly spread × units, in your account currency. "
        "A buy opens near the ask, a sell near the bid; the position starts down by "
        "about this much before any market move. Overnight/rollover fees are separate."));
    tradeForm->addRow(QStringLiteral("Opening cost:"), m_openCost);

    // Rollover fees for this order size: charged per night a leveraged position is
    // held; the weekend figure is the one-off triple charge on the rollover night.
    m_feeCost = new QLabel(QStringLiteral("—"), tradeBox);
    m_feeCost->setStyleSheet(QStringLiteral("color:#777"));
    m_feeCost->setTextFormat(Qt::RichText);
    m_feeCost->setToolTip(QStringLiteral(
        "eToro's rollover fees for this order size, per side. Overnight is charged for "
        "each night the position stays open; the weekend figure is the (roughly triple) "
        "one-off charge applied on the weekend rollover night. Negative values are "
        "credits paid to you."));
    tradeForm->addRow(QStringLiteral("Overnight fee:"), m_feeCost);
    updateOpenCost();
}

void MainWindow::buildSignalsWindow(const QString &sym)
{
    // Trading-signals panel — buy/sell/close guidance from the instrument's
    // technicals. Shares ONE parentless top-level window with the AI panel
    // below (same reasoning as the chart: a parented Qt::Window is a transient
    // child that steals the main window's input focus, blocking the trade
    // fields while it is open). Stays on top so it is always visible while
    // trading; shown at startup, toggled via the header "Signals & AI" button,
    // closed with the app in closeEvent.
    m_signalsWindow = new QWidget(nullptr);
    m_signalsWindow->setWindowTitle(QStringLiteral("Trading signals & AI — %1").arg(sym));
    m_signalsWindow->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    auto *signalsWinLayout = new QVBoxLayout(m_signalsWindow);
    m_signalsWindow->installEventFilter(this);  // keep the header toggle in sync
    m_sigBox = new QGroupBox(QStringLiteral("Trading signals — %1").arg(sym),
                             m_signalsWindow);
    signalsWinLayout->addWidget(m_sigBox);
    auto *sigBox = m_sigBox;
    auto *sigForm = new QFormLayout(sigBox);
    sigForm->setVerticalSpacing(2);          // 17 rows — keep them compact so the panel fits
    sigForm->setContentsMargins(9, 6, 9, 6);
    m_sigTrend = new QLabel(QStringLiteral("—"), sigBox);
    m_sigMomentum = new QLabel(QStringLiteral("—"), sigBox);
    m_sigMacd = new QLabel(QStringLiteral("—"), sigBox);
    m_sigBoll = new QLabel(QStringLiteral("—"), sigBox);
    m_sigVol = new QLabel(QStringLiteral("—"), sigBox);
    m_sigVix = new QLabel(QStringLiteral("—"), sigBox);
    m_sigRegime = new QLabel(QStringLiteral("—"), sigBox);
    m_sigNews = new QLabel(QStringLiteral("—"), sigBox);
    m_sigWeb = new QLabel(QStringLiteral("…"), sigBox);
    m_sigCrowd = new QLabel(QStringLiteral("…"), sigBox);
    m_sigLocalAi = new QLabel(QStringLiteral("…"), sigBox);
    m_sigConfluence = new QLabel(QStringLiteral("…"), sigBox);
    m_sigWebQuote = new QLabel(QStringLiteral("…"), sigBox);
    m_sigRegression = new QLabel(QStringLiteral("—"), sigBox);
    m_sigKnn = new QLabel(QStringLiteral("—"), sigBox);
    m_sigStoch = new QLabel(QStringLiteral("—"), sigBox);
    m_sigTrend50 = new QLabel(QStringLiteral("—"), sigBox);
    m_sigRisk = new QLabel(QStringLiteral("—"), sigBox);
    m_sigChange = new QLabel(QStringLiteral("—"), sigBox);
    m_sigPrediction = new QLabel(QStringLiteral("—"), sigBox);
    m_sig3h = new QLabel(QStringLiteral("—"), sigBox);
    m_sig3d = new QLabel(QStringLiteral("—"), sigBox);
    m_sigOverall = new QLabel(QStringLiteral("—"), sigBox);
    QFont sigFont = m_sigOverall->font();
    sigFont.setBold(true);
    m_sigOverall->setFont(sigFont);
    m_sigPrediction->setFont(sigFont);
    m_sigWeb->setFont(sigFont);
    m_sig3h->setFont(sigFont);
    m_sig3d->setFont(sigFont);

    buildSignalRows(sigBox, sigForm);
    buildAiPanel(signalsWinLayout);
}

void MainWindow::buildSignalRows(QGroupBox *sigBox, QFormLayout *sigForm)
{
    // Add a labelled row whose caption and value both carry an explanatory
    // mouse-over describing what the signal means and how to read it.
    auto addSignalRow = [sigBox, sigForm](const QString &caption, QLabel *value,
                                          const QString &tip) {
        auto *cap = new QLabel(caption, sigBox);
        cap->setToolTip(tip);
        value->setToolTip(tip);
        sigForm->addRow(cap, value);
    };
    // Trend (SMA) and MACD kept together as two adjacent lines…
    addSignalRow(QStringLiteral("Trend (SMA 10/30):"), m_sigTrend,
                 QStringLiteral("Fast vs. slow simple moving average (10 vs. 30 bars). Fast above "
                                "slow = up-trend (bullish); fast below = down-trend (bearish)."));
    addSignalRow(QStringLiteral("MACD (12/26/9):"), m_sigMacd,
                 QStringLiteral("Moving-average convergence/divergence histogram. Positive "
                                "(above signal line) = bullish momentum, negative = bearish."));
    // …and Momentum (RSI) and Stochastic %K kept together as the next two lines.
    addSignalRow(QStringLiteral("Momentum (RSI 14):"), m_sigMomentum,
                 QStringLiteral("Relative Strength Index over 14 bars (0–100). Above 70 = "
                                "overbought (pullback risk), below 30 = oversold (bounce risk); "
                                "~50 is neutral."));
    addSignalRow(QStringLiteral("Stochastic %K (14):"), m_sigStoch,
                 QStringLiteral("Where the price is within its 14-bar high–low range (0–100). "
                                "Above 80 = overbought, below 20 = oversold — used for entry "
                                "timing."));
    addSignalRow(QStringLiteral("Bollinger %B (20):"), m_sigBoll,
                 QStringLiteral("Where price sits inside its 20-bar Bollinger Bands. Near 1 = at "
                                "the upper band (stretched high), near 0 = at the lower band "
                                "(stretched low), ~0.5 = mid-band."));
    addSignalRow(QStringLiteral("Volatility (20):"), m_sigVol,
                 QStringLiteral("Typical size of a one-bar move (std-dev of returns over 20 "
                                "bars), in percent. Higher = choppier, larger swings."));
    addSignalRow(QStringLiteral("VIX (fear):"), m_sigVix,
                 QStringLiteral("CBOE Volatility Index (spot), and how far it sits above/below its "
                                "own multi-month average. Elevated / rising VIX = risk-off "
                                "(bearish for indices) and trims signal confidence; low / falling "
                                "VIX = risk-on. Folded into the buy/sell ensemble below."));
    addSignalRow(QStringLiteral("Regime (VIX/events):"), m_sigRegime,
                 QStringLiteral("Market regime from the VIX plus the economic calendar: calm VIX = "
                                "risk-on (mildly bullish for indices), fearful VIX = risk-off; an "
                                "imminent high-impact event flags added volatility. The same regime "
                                "source used in the Decision window."));
    addSignalRow(QStringLiteral("News sentiment:"), m_sigNews,
                 QStringLiteral("Sentiment of recent headlines for this instrument (TradingView "
                                "feed), scored by keyword tone from negative to positive. The same "
                                "news source used in the Decision window; n/a until the news scan "
                                "has run (or for instruments with no web ticker)."));
    buildStatisticalSignalRows(sigBox, sigForm);
}

// The statistical and per-instrument reads, split out of buildSignalRows purely so
// neither function is a hundred lines of table rows.
void MainWindow::buildStatisticalSignalRows(QGroupBox *sigBox, QFormLayout *sigForm)
{
    auto addSignalRow = [sigBox, sigForm](const QString &caption, QLabel *value,
                                          const QString &tip) {
        auto *cap = new QLabel(caption, sigBox);
        cap->setToolTip(tip);
        value->setToolTip(tip);
        sigForm->addRow(cap, value);
    };
    addSignalRow(QStringLiteral("Regression (30):"), m_sigRegression,
                 QStringLiteral("Least-squares trend line over the last 30 bars: slope in %/bar "
                                "(direction & steepness) and R² (0–1) for how well price fits the "
                                "line — higher R² = a cleaner trend."));
    addSignalRow(QStringLiteral("Pattern kNN (10):"), m_sigKnn,
                 QStringLiteral("k-Nearest-Neighbours analog forecast: finds the past 10-bar "
                                "patterns most like now and averages what happened next. Shows the "
                                "expected move and how strongly the analogs agree."));
    addSignalRow(QStringLiteral("Trend filter (SMA 50):"), m_sigTrend50,
                 QStringLiteral("Price vs. its 50-bar simple moving average. Above = "
                                "long-friendly regime (uptrend), below = downtrend."));
    addSignalRow(QStringLiteral("Risk at leverage:"), m_sigRisk,
                 QStringLiteral("Expected ~1-hour price swing multiplied by your selected "
                                "leverage = the swing in your margin. Amber/red flag oversized "
                                "risk for the chosen leverage."));
    addSignalRow(QStringLiteral("Change (window):"), m_sigChange,
                 QStringLiteral("Percentage price change across the currently loaded history "
                                "window (first bar → latest)."));
    addSignalRow(QStringLiteral("Web rating (1h):"), m_sigWeb,
                 QStringLiteral("Real-time technical rating from TradingView for this instrument "
                                "(1-hour timeframe): an aggregate of ~26 indicators shown as Strong "
                                "Sell … Strong Buy. Fetched live from the internet; shows n/a for "
                                "eToro's proprietary baskets with no TradingView equivalent."));
    addSignalRow(QStringLiteral("Confluence (independent):"), m_sigConfluence,
                 QStringLiteral("How many INDEPENDENT reads agree with the current call — the "
                                "futures that lead the cash market (Nasdaq vs S&P), expected "
                                "volatility (^VXN for the Nasdaq, ^VIX otherwise, read by its "
                                "direction rather than its level), the US 10-year yield (rising "
                                "yields press on growth shares), how many of the eight Nasdaq "
                                "heavyweights are up, and where price sits against its own "
                                "opening range. Agreement between independent things is evidence; "
                                "another oscillator over the same closes is not. A read that "
                                "cannot be computed counts as UNMEASURED, never as agreement. "
                                "Heavyweight participation is a stand-in for market breadth, "
                                "which needs per-constituent data this app does not fetch."));
    addSignalRow(QStringLiteral("Local model (Ollama):"), m_sigLocalAi,
                 QStringLiteral("What the LOCAL large language model (Ollama, running on this "
                                "machine — no key, nothing leaves it) says about THIS instrument: "
                                "its side, its confidence and its own one-line reasoning. It is "
                                "asked once per all-instruments scan over the same evidence the "
                                "decision window uses, and it answers only about the instruments "
                                "it considers worth trading — \"no opinion\" is a real answer and "
                                "means it did not name this one. The bot may act on it (see the Bot "
                                "sim window); here it is shown as one source among the others."));
    addSignalRow(QStringLiteral("Crowd (Fear/Greed):"), m_sigCrowd,
                 QStringLiteral("CNN's Fear & Greed index — what the trading crowd is doing right "
                                "now, aggregated from put/call ratios, breadth, momentum and more: "
                                "0 = extreme fear, 100 = extreme greed. Mid readings tilt mildly "
                                "with the crowd; extremes are read contrarian (extreme fear = "
                                "capitulation, extreme greed = froth). Folded into the Decision "
                                "window's composite."));
    addSignalRow(QStringLiteral("Web quote (Yahoo):"), m_sigWebQuote,
                 QStringLiteral("Independent reference quote for this instrument from Yahoo "
                                "Finance, with its exchange timestamp — a cross-check on how fresh "
                                "and close eToro's own rate is. A small gap is normal (the CFD "
                                "tracks futures and carries the spread); a large or growing gap "
                                "means one of the feeds is stale."));
    addSignalRow(QStringLiteral("Prediction (now):"), m_sigPrediction,
                 QStringLiteral("Ensemble vote across all the indicators above: direction, a "
                                "confidence % (how strongly they agree) and the expected per-bar "
                                "move. Confidence is halved when a high-impact event is imminent."));
    addSignalRow(QStringLiteral("Forecast (3h):"), m_sig3h,
                 QStringLiteral("Extrapolates the recent drift over the next ~3 hours (180 "
                                "one-minute bars), with a target price and a ±1σ range that grows "
                                "with the square root of time."));
    addSignalRow(QStringLiteral("AI forecast (3 days):"), m_sig3d,
                 QStringLiteral("Ensemble projection for the next 3 trading days: the indicator "
                                "vote sets the direction and how far within the expected ±1σ range "
                                "price is likely to travel. A model estimate, not investment advice."));
    addSignalRow(QStringLiteral("Signal:"), m_sigOverall,
                 QStringLiteral("Overall call from the same ensemble: BUY or SELL when a clear "
                                "majority of indicators agree, otherwise NEUTRAL."));
}

void MainWindow::buildAiPanel(QVBoxLayout *signalsWinLayout)
{
    // --- AI decision-support panel -------------------------------------------
    // Shares the floating signals window: one always-visible place for both.
    m_aiBox = new QGroupBox(QStringLiteral("AI decision support"), m_signalsWindow);
    signalsWinLayout->addWidget(m_aiBox);
    auto *aiForm = new QFormLayout(m_aiBox);
    aiForm->setVerticalSpacing(2);
    aiForm->setContentsMargins(9, 6, 9, 6);
    m_aiUpProb = new QLabel(QStringLiteral("—"), m_aiBox);
    m_aiMonteCarlo = new QLabel(QStringLiteral("—"), m_aiBox);
    m_aiEdge = new QLabel(QStringLiteral("—"), m_aiBox);
    m_aiRegime = new QLabel(QStringLiteral("—"), m_aiBox);
    m_aiAdvice = new QLabel(QStringLiteral("—"), m_aiBox);
    m_aiAdvice->setWordWrap(true);
    QFont aiFont = m_aiAdvice->font();
    aiFont.setBold(true);
    m_aiUpProb->setFont(aiFont);
    m_aiAdvice->setFont(aiFont);

    auto addAiRow = [this, aiForm](const QString &caption, QLabel *value, const QString &tip) {
        auto *cap = new QLabel(caption, m_aiBox);
        cap->setToolTip(tip);
        value->setToolTip(tip);
        aiForm->addRow(cap, value);
    };
    addAiRow(QStringLiteral("Up-probability:"), m_aiUpProb,
             QStringLiteral("A logistic model over the indicator features (trend, MACD, RSI, "
                            "momentum, regression, kNN, stochastic, long-term trend) → estimated "
                            "probability that the next move is up. A model estimate, not a "
                            "guarantee."));
    addAiRow(QStringLiteral("Monte-Carlo (3h):"), m_aiMonteCarlo,
             QStringLiteral("Thousands of simulated 3-hour paths built by resampling this "
                            "instrument's own recent per-bar returns (a bootstrap — it keeps the "
                            "real return distribution). Shows P(price higher) and the 5–95% "
                            "outcome range."));
    addAiRow(QStringLiteral("Setup edge:"), m_aiEdge,
             QStringLiteral("From the same simulation and your Amount / Leverage / Stop-loss / "
                            "Take-profit: the probability of hitting take-profit before stop-loss "
                            "for the favoured side, vs. the break-even win-rate your reward:risk "
                            "needs. Positive edge = the setup has a statistical advantage."));
    addAiRow(QStringLiteral("Regime:"), m_aiRegime,
             QStringLiteral("Hurst exponent of recent returns. >0.55 trending (trend-following "
                            "favoured), <0.45 mean-reverting (fade extremes favoured), otherwise "
                            "random walk."));
    addAiRow(QStringLiteral("Recommendation:"), m_aiAdvice,
             QStringLiteral("Explicit BUY / SELL / HOLD call synthesised from the signals above, "
                            "with a one-line rationale. Decision support only — always confirm "
                            "and manage your own risk."));
}

QGroupBox *MainWindow::buildPositionsPanel(QWidget *lower)
{
    // Positions panel
    auto *posBox = new QGroupBox(QStringLiteral("Open trades"), lower);
    auto *posLayout = new QVBoxLayout(posBox);

    // Model/view: the poll and the per-tick P/L re-price update the model in
    // place (dataChanged) — no per-refresh item allocations, and open SL/TP
    // editors / checkbox marks survive by construction.
    m_positionsModel = new PositionsModel(posBox);
    m_positionsModel->setDisplay(m_ccy, m_eurPerUsd);
    m_positions = new QTableView(posBox);
    m_positions->setModel(m_positionsModel);
    m_positions->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_positions->verticalHeader()->setVisible(false);
    // Trades are marked with the per-row checkbox in the Position column, so plain
    // row selection is disabled to avoid two competing notions of "chosen".
    m_positions->setSelectionMode(QAbstractItemView::NoSelection);
    // Only the Stop-loss / Take-profit cells are editable (flag set per-item); a
    // double-click or F2 on one of those opens the editor.
    m_positions->setEditTriggers(QAbstractItemView::DoubleClicked
                                 | QAbstractItemView::EditKeyPressed);
    // While an SL/TP editor is open, the model holds back that cell's updates —
    // otherwise every portfolio poll re-fills the editor and wipes the typing
    // (the view re-reads open editors from the model on dataChanged).
    auto *slTpGuard = new SlTpEditGuardDelegate(m_positionsModel, m_positions);
    m_positions->setItemDelegateForColumn(PositionsModel::ColSl, slTpGuard);
    m_positions->setItemDelegateForColumn(PositionsModel::ColTp, slTpGuard);
    m_positions->setToolTip(QStringLiteral(
        "Tick one or more trades, then Close marked trades.\n"
        "Click the Side cell of a trade to open its gauge window.\n"
        "Click the Instrument cell to switch the app to that instrument.\n"
        "Double-click a Stop-loss / Take-profit cell to change it (amount in %1; blank clears it).\n"
        "SL is signed P/L: a negative value closes at a loss, a positive value locks in a "
        "profit (stop on the winning side).")
                                .arg(m_ccy));
    static_cast<void>(connect(m_positionsModel, &PositionsModel::slTpEdited, this,
                              &MainWindow::onPositionSlTpEdited));
    // Click routing (REQ-F-024): the Side cell opens the gauge window (buy value,
    // live value/needle, P/L and SL/TP); the Instrument cell switches the app to
    // that trade's instrument, like picking it from the selector. Other cells do
    // nothing (mark checkbox / editable SL,TP keep their own interactions).
    static_cast<void>(connect(
        m_positions, &QTableView::clicked, this, [this](const QModelIndex &index) {
            const qint32 row = index.row();
            if ((row < 0) || (row >= m_shownPositions.size())) {
                return;
            }
            const qint32 col = index.column();
            if (col == PositionsModel::ColInstrument) {
                const QString tradeSym = m_shownPositions[row].symbol;
                if (!tradeSym.isEmpty()) {
                    m_autoInstrumentDone = true;  // a manual pick ends the startup auto-load
                    selectInstrument(tradeSym);
                }
                return;
            }
            if (col != PositionsModel::ColSide) {
                return;
            }
            if (m_tradeGauge == nullptr) {
                m_tradeGauge = new TradeGaugeDialog(this);
            }
            const Position &p = m_shownPositions[row];
            const bool isCurrent =
                p.symbol.compare(m_client->instrument().symbol, Qt::CaseInsensitive) == 0;
            m_tradeGauge->showTrade(p, (isCurrent && (m_lastPrice > 0.0)) ? m_lastPrice : 0.0,
                                    m_ccy, m_eurPerUsd);
            // Mark the P/L line from the trade's own quote right away, so the window
            // does not open on the snapshot figure and only correct itself a tick later.
            const Quote quote = m_client->quotes().value(p.instrumentId);
            if (quote.isValid()) {
                m_tradeGauge->updatePrice(isCurrent ? m_lastPrice : quote.bid, quote);
            }
        }));
    posLayout->addWidget(m_positions);

    // Account-wide totals of the rows above: what is currently tied up and what the book
    // is worth right now. Updated with every re-price, not only per portfolio poll, so it
    // moves with the P/L column it sums.
    m_openTradesSummary = new QLabel(posBox);
    m_openTradesSummary->setTextFormat(Qt::RichText);
    m_openTradesSummary->setToolTip(QStringLiteral(
        "Totals over the open trades above: the invested amounts as the Amount column "
        "rounds them (so the column adds up to this), and the sum of the P/L column — "
        "each row marked at its own instrument's current rate, the way eToro marks it."));
    posLayout->addWidget(m_openTradesSummary);

    // Close-proposal banner: filled by the watchdog when the live price breaks the
    // forecast corridor against an open trade (or the signal flips hard against it).
    m_closeAdvice = new QLabel(posBox);
    m_closeAdvice->setTextFormat(Qt::RichText);
    m_closeAdvice->setWordWrap(true);
    m_closeAdvice->setVisible(false);
    m_closeAdvice->setToolTip(QStringLiteral(
        "Shown when the live price moves outside the prediction corridor against an "
        "open trade, or the signal ensemble flips against it with high confidence. "
        "A proposal only — nothing is closed automatically; tick the trade and use "
        "Close marked trades if you agree."));
    posLayout->addWidget(m_closeAdvice);

    m_closeButton = new QPushButton(QStringLiteral("Close marked trades"), posBox);
    m_closeButton->setMinimumHeight(34);
    posLayout->addWidget(m_closeButton);
    return posBox;
}

void MainWindow::buildMonthlyPnlPanel(QWidget *lower)
{
    // Closed-trade P/L summary: net profit/loss of the last 7 weeks' closed trades,
    // restricted to the instruments listed in the selector, summed in account currency.
    m_pnlBox = new QGroupBox(QStringLiteral("Closed trades — last 7 weeks (listed instruments)"), lower);
    auto *pnlLayout = new QVBoxLayout(m_pnlBox);
    m_pnlSummary = new QLabel(QStringLiteral("Loading closed-trade P/L…"), m_pnlBox);
    m_pnlSummary->setTextFormat(Qt::RichText);
    m_pnlSummary->setWordWrap(true);
    pnlLayout->addWidget(m_pnlSummary);

    m_pnlTable = new QTableWidget(0, 4, m_pnlBox);
    m_pnlTable->setHorizontalHeaderLabels(
        {QStringLiteral("Instrument"), QStringLiteral("Trades"),
         QStringLiteral("Net P/L (%1)").arg(m_ccy), QStringLiteral("Costs (%1)").arg(m_ccy)});
    m_pnlTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_pnlTable->verticalHeader()->setVisible(false);
    m_pnlTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_pnlTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pnlTable->setFocusPolicy(Qt::NoFocus);
    m_pnlTable->setMaximumHeight(150);
    m_pnlTable->setToolTip(QStringLiteral(
        "Net P/L of trades closed in the window, only for instruments in the "
        "selector. Costs = estimated opening + closing spread cost (at the "
        "instrument's current spread) + the rollover fees eToro reports. "
        "Whole-account totals are shown above for context."));
    pnlLayout->addWidget(m_pnlTable);

    auto *pnlButtons = new QHBoxLayout;
    m_pnlRefresh = new QPushButton(QStringLiteral("Refresh closed-trade P/L"), m_pnlBox);
    static_cast<void>(connect(m_pnlRefresh, &QPushButton::clicked, this,
                              [this] { m_client->fetchClosedTrades(closedLookbackWeeks()); }));
    m_pnlDetails = new QPushButton(QStringLiteral("All trades…"), m_pnlBox);
    m_pnlDetails->setToolTip(QStringLiteral(
        "Every closed trade of the last 7–13 weeks (lookback selectable), with net "
        "P/L, rollover fees and estimated opening/closing spread costs per trade."));
    static_cast<void>(
        connect(m_pnlDetails, &QPushButton::clicked, this, &MainWindow::openClosedTrades));
    pnlButtons->addWidget(m_pnlRefresh);
    pnlButtons->addWidget(m_pnlDetails);
    pnlLayout->addLayout(pnlButtons);
}

void MainWindow::buildBottomRow(QWidget *lower, QVBoxLayout *lowerLayout, const QString &sym)
{
    // Economic-calendar panel: macro events that could move the instrument, next 3 days.
    m_eventsBox = new QGroupBox(
        QStringLiteral("Market events — next 3 trading days (with %1 impact)").arg(sym), lower);
    auto *eventsLayout = new QVBoxLayout(m_eventsBox);
    m_events = new QListWidget(m_eventsBox);
    m_events->setMaximumHeight(120);
    m_events->setSelectionMode(QAbstractItemView::NoSelection);
    m_events->setFocusPolicy(Qt::NoFocus);
    m_events->setToolTip(
        QStringLiteral("Macro events from the regions that tend to move %1").arg(sym));
    m_events->addItem(QStringLiteral("Loading economic calendar…"));
    eventsLayout->addWidget(m_events);

    buildRecommendationsPanel(lower);

    // Activity log, sharing the bottom row: its height is bounded like the
    // events/reco lists so the row stays compact and spare vertical space keeps
    // flowing to the open-trades panel above.
    auto *logBox = new QGroupBox(QStringLiteral("Activity"), lower);
    auto *logLayout = new QVBoxLayout(logBox);
    logLayout->setContentsMargins(6, 4, 6, 4);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setMaximumHeight(120);
    logLayout->addWidget(m_log);

    // Events, activity log and recommendations side by side in one draggable
    // splitter row: market events | Activity | Buy / sell now.
    auto *eventsSplitter = new QSplitter(Qt::Horizontal, lower);
    eventsSplitter->addWidget(m_eventsBox);
    eventsSplitter->addWidget(logBox);
    eventsSplitter->addWidget(m_recoBox);
    eventsSplitter->setChildrenCollapsible(false);  // no pane can be dragged to zero
    eventsSplitter->setStretchFactor(0, 3);  // events grows fastest on resize
    eventsSplitter->setStretchFactor(1, 2);
    eventsSplitter->setStretchFactor(2, 1);
    eventsSplitter->setSizes({300, 200, 120});
    lowerLayout->addWidget(eventsSplitter);
}

void MainWindow::buildRecommendationsPanel(QWidget *lower)
{
    // "Buy / sell now" panel, right of the market events: instruments the signals +
    // web rating currently favour, newest news in each row's hover reasoning.
    m_recoBox = new QGroupBox(QStringLiteral("Buy / sell now"), lower);
    auto *recoLayout = new QVBoxLayout(m_recoBox);
    // Two columns: BUY calls on the left, SELL calls on the right (each strongest first).
    auto makeRecoList = [this]() {
        auto *list = new QListWidget(m_recoBox);
        list->setMaximumHeight(120);
        list->setSelectionMode(QAbstractItemView::NoSelection);
        list->setFocusPolicy(Qt::NoFocus);
        list->setToolTip(QStringLiteral(
            "Instruments the technical ensemble and the TradingView rating currently favour, "
            "strongest first. Hover a row for the reasoning, including recent news.\n"
            "Double-click a row to switch the app to that instrument."));
        // Double-click a recommendation → switch to that instrument (like the screener).
        static_cast<void>(
            connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
                const QString picked =
                    (it != nullptr) ? it->data(Qt::UserRole).toString() : QString();
                if (picked.isEmpty()) {
                    return;
                }
                m_autoInstrumentDone = true;  // a manual pick ends the startup auto-load
                selectInstrument(picked);
            }));
        return list;
    };
    auto *buyHeader = new QLabel(QStringLiteral("BUY"), m_recoBox);
    buyHeader->setStyleSheet(QStringLiteral("color:#25b563; font-weight:bold;"));
    auto *sellHeader = new QLabel(QStringLiteral("SELL"), m_recoBox);
    sellHeader->setStyleSheet(QStringLiteral("color:#e35555; font-weight:bold;"));
    m_recoBuyList = makeRecoList();
    m_recoSellList = makeRecoList();
    m_recoBuyList->addItem(QStringLiteral("Scanning…"));
    m_recoSellList->addItem(QStringLiteral("Scanning…"));

    auto *buyCol = new QVBoxLayout;
    buyCol->setSpacing(2);
    buyCol->addWidget(buyHeader);
    buyCol->addWidget(m_recoBuyList);
    auto *sellCol = new QVBoxLayout;
    sellCol->setSpacing(2);
    sellCol->addWidget(sellHeader);
    sellCol->addWidget(m_recoSellList);
    auto *recoCols = new QHBoxLayout;
    recoCols->addLayout(buyCol);
    recoCols->addLayout(sellCol);
    recoLayout->addLayout(recoCols);

    m_recoRefresh = new QPushButton(QStringLiteral("Refresh"), m_recoBox);
    m_recoRefresh->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    static_cast<void>(connect(m_recoRefresh, &QPushButton::clicked, this,
                              &MainWindow::startRecommendationScan));
    recoLayout->addWidget(m_recoRefresh);
}

void MainWindow::buildUi()
{
    // Seed every instrument-specific label from the configured symbol; onReady()
    // refreshes them on each (re)resolution, so nothing stays pinned to SPX500.
    const QString sym = m_client->config().symbol;
    setWindowTitle(QStringLiteral("eToro Trader — %1").arg(sym));

    // Size the controls window and the (separate) chart window to the current
    // screen instead of a fixed 1000×760 / 940×560: the two sit side by side (the
    // chart is placed to the right of the controls in the constructor), so split
    // the available width between them, leaving a small gap. Sizes are a fraction
    // of the screen but clamped — a floor so nothing is unusably small on a laptop,
    // and a ceiling so the windows don't become absurdly large on a 4K display.
    // If a small screen can't fit both at the target width, the chart is shrunk to
    // whatever width is left so the pair still fits.
    const QScreen *scr = (screen() != nullptr) ? screen() : QGuiApplication::primaryScreen();
    const QRect avail = (scr != nullptr) ? scr->availableGeometry() : QRect(0, 0, 1000, 760);
    constexpr qint32 kWindowGap = 16;  // matches the chart's placement offset

    const qint32 winH = qBound(560, qRound(avail.height() * 0.85), 900);
    const qint32 usableW = avail.width() - kWindowGap;
    const qint32 mainW = qBound(720, qRound(usableW * 0.42), 1200);
    qint32 chartW = qBound(560, qRound(usableW * 0.50), 1400);
    if ((mainW + kWindowGap + chartW) > avail.width()) {
        chartW = qMax(480, avail.width() - kWindowGap - mainW);
    }
    const qint32 chartH = winH;

    // Remembered as the 1.0 baseline for the Ctrl+wheel UI zoom (see applyUiScale).
    m_baseMainSize = QSize(mainW, winH);
    m_baseChartSize = QSize(chartW, chartH);

    resize(mainW, winH);

    // The account is USD-based, but the UI is shown in euro: amounts are converted
    // with the live EURUSD rate (see toDisplay/fromDisplay). Until the first rate
    // arrives, values are shown at parity so nothing reads as a bogus number.
    m_ccy = QStringLiteral("€");

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);

    root->addLayout(buildHeaderRow(central, sym));

    // --- Mode badge ----------------------------------------------------------
    m_modeLabel = new QLabel(central);
    m_modeLabel->setAlignment(Qt::AlignCenter);
    m_modeLabel->setContentsMargins(6, 4, 6, 4);
    root->addWidget(m_modeLabel);

    buildChartWindow(sym, chartW, chartH);

    // --- Controls below the chart --------------------------------------------
    auto *lower = new QWidget(central);
    auto *lowerLayout = new QVBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 0, 0, 0);

    auto *controlsRow = new QHBoxLayout;

    QGroupBox *tradeBox = buildTradePanel(lower, sym);

    // Limit orders, held by eToro itself (REQ-F-027). Built in its own function to
    // keep buildUi() off the metrics ratchet.
    QGroupBox *limitBox = buildLimitOrderBox(lower);

    buildSignalsWindow(sym);

    // Left column: trade panel and limit orders (the signals panel lives in its
    // own window now, toggled from the header).
    auto *leftCol = new QVBoxLayout;
    leftCol->addWidget(tradeBox);
    leftCol->addWidget(limitBox);
    leftCol->addStretch();
    controlsRow->addLayout(leftCol, 0);

    // Recompute the AI signals live when the trade parameters that feed the
    // setup-edge calculation change.
    static_cast<void>(
        connect(m_amount, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSignals));
    static_cast<void>(
        connect(m_stopLoss, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSignals));
    static_cast<void>(
        connect(m_takeProfit, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSignals));
    static_cast<void>(
        connect(m_leverage, &QComboBox::currentTextChanged, this, &MainWindow::updateSignals));

    QGroupBox *posBox = buildPositionsPanel(lower);

    buildMonthlyPnlPanel(lower);

    // Right column: open trades (stretches) with the closed-trade summary beneath
    // it (the AI decision support lives in its own window now).
    auto *rightCol = new QVBoxLayout;
    rightCol->addWidget(posBox, 1);
    rightCol->addWidget(m_pnlBox, 0);
    controlsRow->addLayout(rightCol, 1);
    lowerLayout->addLayout(controlsRow);

    buildBottomRow(lower, lowerLayout, sym);

    root->addWidget(lower);

    setCentralWidget(central);
}

// ---------------------------------------------------------------------------
// Client signal handlers
// ---------------------------------------------------------------------------

void MainWindow::onReady(const Instrument &instrument)
{
    m_instrumentResolving = false;  // unlock BUY/SELL — the panel now matches the selection
    const QString name = instrument.displayName.isEmpty() ? instrument.symbol
                                                          : instrument.displayName;
    m_titleLabel->setText(QStringLiteral("%1  (%2)").arg(instrument.symbol, name));
    m_chart->setTitle(QStringLiteral("%1 price").arg(instrument.symbol));
    m_chart->setWindowTitle(QStringLiteral("%1 — Price / time + change").arg(instrument.symbol));
    m_tradeBox->setTitle(QStringLiteral("Trade %1").arg(instrument.symbol));
    updateTradeHours(instrument.symbol);
    updateTradeButtonsEnabled();  // reflect the new instrument's market-open state
    m_sigBox->setTitle(QStringLiteral("Trading signals — %1").arg(instrument.symbol));
    if (m_signalsWindow != nullptr) {
        m_signalsWindow->setWindowTitle(
            QStringLiteral("Trading signals & AI — %1").arg(instrument.symbol));
    }
    m_eventsBox->setTitle(
        QStringLiteral("Market events — next 3 trading days (with %1 impact)")
            .arg(instrument.symbol));
    m_events->setToolTip(
        QStringLiteral("Macro events from the regions that tend to move %1").arg(instrument.symbol));
    // Re-scope the calendar to the regions that move the new instrument (fetches only
    // if the region set actually changed).
    m_calendar->setInstrument(instrument.symbol);
    // Kick off the first "buy / sell now" scan once the app is live — but a few seconds
    // later, so the heavier multi-instrument scan doesn't pile onto the startup burst
    // on eToro's small shared rate pool (price/portfolio/history load first).
    if (!m_recoKickoff) {
        m_recoKickoff = true;
        QTimer::singleShot(8000, this, &MainWindow::startRecommendationScan);
    }
    // Re-render the cached event rows so their per-event read-out / tooltips name the
    // new instrument (the calendar itself refreshes on its own, slower, schedule).
    if (!m_eventList.isEmpty()) {
        onEvents(m_eventList);
    }

    const QString mode = m_client->config().modeLabel();
    m_modeLabel->setText(mode);
    QString bg = QStringLiteral("#3a4a63");  // simulation / neutral
    if (m_client->config().isLive()) {
        bg = QStringLiteral("#a11212");
    } else if (m_client->config().hasCredentials()) {
        bg = QStringLiteral("#b26a00");
    } else {
        // no credentials — keep the neutral simulation badge colour
    }
    m_modeLabel->setStyleSheet(
        QStringLiteral("QLabel { background:%1; color:white; font-weight:bold; border-radius:4px; }")
            .arg(bg));

    // In live mode, mark the window title as a real-money reminder and append the
    // eToro username (config `username` / ETORO_USERNAME) — not the OS login name.
    if (m_client->config().isLive()) {
        const QString user = m_client->config().username.trimmed();
        QString title = QStringLiteral("eToro Trader — %1 — LIVE (REAL MONEY)").arg(instrument.symbol);
        if (!user.isEmpty()) {
            title += QStringLiteral(" — %1").arg(user);
        }
        setWindowTitle(title);
    } else {
        setWindowTitle(QStringLiteral("eToro Trader — %1").arg(instrument.symbol));
    }

    // Auto-load the closed-trade summary once (the panel also has a Refresh button).
    // The client names the trades when the walk completes, so the concurrent
    // listed-instrument id resolution doesn't need a head start; the short delay
    // just keeps the first seconds of API traffic for the price/portfolio setup.
    if (!m_pnlAutoFetched) {
        m_pnlAutoFetched = true;
        QTimer::singleShot(1500, this,
                           [this] { m_client->fetchClosedTrades(closedLookbackWeeks()); });
    }
}

void MainWindow::onHistory(const QList<Candle> &candles)
{
    m_chart->setHistory(candles);
    // Resample the (ascending) history to one close per clock-hour — the last close
    // in each hour. The chart keeps the full minute detail; the signals run on this
    // hourly series so they match the screener's timeframe.
    m_hourlyCloses.clear();
    m_hourlyCloses.reserve(candles.size());
    qint64 curHourMs = -1;
    for (const Candle &c : candles) {
        if ((c.close <= 0.0) || !c.timestamp.isValid()) {
            continue;
        }
        const qint64 hourMs = (c.timestamp.toMSecsSinceEpoch() / 3600000) * 3600000;
        if (hourMs != curHourMs) {   // new hour → new bar
            m_hourlyCloses.append(c.close);
            curHourMs = hourMs;
        } else {                     // same hour → keep the latest close
            m_hourlyCloses.last() = c.close;
        }
    }
    if (!candles.isEmpty()) {
        m_lastPrice = candles.last().close;
    }

    // setHistory() clears the old instrument's entry bullets; the next
    // onPortfolio() re-supplies the ones for the instrument now on screen.
    updateSignals();
}

void MainWindow::onPrice(const QDateTime &time, double price)
{
    m_lastPrice = price;
    m_priceLabel->setText(QLocale().toString(price, 'f', trading::priceDecimals(price)));
    m_chart->addPoint(time, price);

    // Seed the limit-order rates off the first known price: buy 0.5% below, sell 1%
    // above (the usual "enter on a better price" side). Done once, so the user can
    // still edit them afterwards. The instrument's own precision decides the decimals:
    // 2 is right for an index at 5900 and useless for EURUSD at 1.1373.
    // A rate the user is typing is never touched — same rule as the SL/TP proposal
    // (REQ-F-012), and focus can sit on the spin box or its internal line edit.
    if (!m_limitRateDefaultsSet && (price > 0.0)) {
        m_limitRateDefaultsSet = true;
        const QWidget *fw = QApplication::focusWidget();
        const qint32 decimals = trading::priceDecimals(price);
        const auto seed = [fw, decimals](QDoubleSpinBox *field, double value) {
            if ((fw != nullptr) && ((fw == field) || field->isAncestorOf(fw))) {
                return;
            }
            field->setDecimals(decimals);
            field->setSingleStep(std::pow(10.0, -decimals + 1));
            field->setValue(value);
        };
        seed(m_limitBuyRate, price * 0.995);  // buy 0.5% below the market
        seed(m_limitSellRate, price * 1.01);  // sell 1% above it
    }

    updateOpenTradePnl();       // live-refresh open-trade P/L from the quote book
    updatePendingOrderRates();  // …and the resting orders' "Now" column
    // Keep the trade-gauge needle live while it shows a trade on this instrument.
    if ((m_tradeGauge != nullptr) && m_tradeGauge->isVisible()
        && (m_tradeGauge->symbol().compare(m_client->instrument().symbol,
                                           Qt::CaseInsensitive) == 0)) {
        m_tradeGauge->updatePrice(
            price, m_client->quotes().value(m_client->instrument().instrumentId));
    }
    updateSignals();
    // No app-side price watch fires orders any more: conditional entries are eToro's
    // own limit orders (REQ-F-027), triggered by the broker off its own feed.
    checkCloseProposals(price);  // propose closing trades the prediction turned against
    refreshChartEventMarker();  // brings the event line in/out as its window opens/closes
}

void MainWindow::updateOpenCost()
{
    if (m_openCost == nullptr) {
        return;
    }

    const double bid = m_client->lastBid();
    const double ask = m_client->lastAsk();
    // Amount is entered in the display currency (euro).
    const double amount = (m_amount != nullptr) ? m_amount->value() : 0.0;
    const double leverage = (m_leverage != nullptr) ? m_leverage->currentText().toDouble() : 0.0;

    // A real two-sided quote is needed to know the spread the trade crosses; it is
    // absent in simulation mode (only a synthetic mid), so say so rather than invent one.
    if ((bid <= 0.0) || (ask <= 0.0) || (ask <= bid) || (amount <= 0.0) || (leverage <= 0.0)) {
        m_openCost->setText(QStringLiteral(
            "<span style='color:#9a9a9a'>awaiting live bid/ask…</span>"));
        if (m_feeCost != nullptr) {
            m_feeCost->setText(QStringLiteral("<span style='color:#9a9a9a'>—</span>"));
        }
        return;
    }

    const double spread = ask - bid;
    // Opening cost = how far the entry price sits from mid, i.e. HALF the spread (a buy
    // fills at the ask = mid + spread/2, a sell at the bid = mid − spread/2), times the
    // account-currency value per price point. eToro attributes half the spread to
    // opening and half to closing, so its order dialog shows spread/2 × units; charging
    // the full spread here double-counted it (≈2× eToro's figure). Value per point uses
    // the same FX-free identity as the SL/TP maths (amount × leverage / rate, no FX rate
    // needed), so feeding the euro amount in yields the cost already in euro — the
    // display↔account conversion cancels out. Buy/sell differ a hair via ask vs bid.
    const double halfSpread = spread / 2.0;
    const double costBuy = (halfSpread * amount * leverage) / ask;
    const double costSell = (halfSpread * amount * leverage) / bid;

    const QLocale loc;
    const QString costBuyText = loc.toString(costBuy, 'f', 2);
    const QString costSellText = loc.toString(costSell, 'f', 2);
    const QString spreadText = loc.toString(spread, 'f', trading::priceDecimals(ask));
    m_openCost->setText(
        QStringLiteral("Buy <b>%1%2</b> &nbsp;·&nbsp; Sell <b>%1%3</b> "
                       "<span style='color:#9a9a9a'>(spread %4)</span>")
            .arg(m_ccy, costBuyText, costSellText, spreadText));

    // Rollover fees scale with the position's unit count. The fees are quoted in
    // USD per unit; multiplying by units derived from the euro amount lands in
    // euro via the same conversion-cancels identity as the spread cost above.
    if (m_feeCost != nullptr) {
        if (!m_fees.isValid()) {
            m_feeCost->setText(QStringLiteral("<span style='color:#9a9a9a'>—</span>"));
        } else {
            const double unitsBuy = (amount * leverage) / ask;
            const double unitsSell = (amount * leverage) / bid;
            const QString nightBuy = loc.toString(m_fees.buyOvernight * unitsBuy, 'f', 2);
            const QString nightSell = loc.toString(m_fees.sellOvernight * unitsSell, 'f', 2);
            const QString weekendBuy = loc.toString(m_fees.buyWeekend * unitsBuy, 'f', 2);
            const QString weekendSell = loc.toString(m_fees.sellWeekend * unitsSell, 'f', 2);
            m_feeCost->setText(
                QStringLiteral("Buy <b>%1%2</b> &nbsp;·&nbsp; Sell <b>%1%3</b> per night "
                               "<span style='color:#9a9a9a'>(weekend %1%4 / %1%5)</span>")
                    .arg(m_ccy, nightBuy, nightSell, weekendBuy, weekendSell));
        }
    }
}

void MainWindow::setUiScale(double scale)
{
    scale = std::clamp(scale, 0.6, 2.5);  // 60%–250%: readable, never off-screen
    if (qFuzzyCompare(scale, m_uiScale)) {
        return;
}
    m_uiScale = scale;
    applyUiScale();
}

void MainWindow::applyUiScale()
{
    // Fonts: scale every widget in our windows/dialogs from a captured baseline, so
    // widgets that set their own font (title, price, bold signals) keep their
    // relative size and repeated zooms don't compound. Bases are captured lazily on
    // first sight — always before the widget is scaled — which also picks up the
    // lazily-built screener / decision dialogs the first time they're zoomed.
    for (QWidget *root : {static_cast<QWidget *>(this), static_cast<QWidget *>(m_chart),
                          static_cast<QWidget *>(m_screenerDialog),
                          static_cast<QWidget *>(m_decisionDialog)}) {
        if (root == nullptr) {
            continue;
        }
        QList<QWidget *> widgets = root->findChildren<QWidget *>();
        widgets.prepend(root);
        for (QWidget *w : widgets) {  // capture all bases before mutating any font
            if (!m_baseFonts.contains(w)) {
                static_cast<void>(m_baseFonts.insert(w, w->font()));
            }
        }
        for (QWidget *w : widgets) {
            const QFont base = m_baseFonts.value(w);
            QFont f = base;
            if (base.pointSizeF() > 0.0) {
                f.setPointSizeF(base.pointSizeF() * m_uiScale);
            } else if (base.pixelSize() > 0) {
                f.setPixelSize(std::max(1, qRound(base.pixelSize() * m_uiScale)));
            } else {
                // no explicit size on the base font — leave it as captured
            }
            w->setFont(f);
        }
    }

    // Windows: resize both from their baseline, clamped to the screen so a zoom-in
    // can't push them larger than the display. A resize below a window's content
    // minimum is simply ignored by Qt, so zooming out shrinks only as far as the
    // (now smaller) fonts allow.
    const QScreen *scr = (screen() != nullptr) ? screen() : QGuiApplication::primaryScreen();
    const QRect avail = (scr != nullptr) ? scr->availableGeometry() : QRect(0, 0, 1000, 760);
    const auto scaled = [this, &avail](QSize base) {
        return QSize(qMin(qRound(base.width() * m_uiScale), avail.width()),
                     qMin(qRound(base.height() * m_uiScale), avail.height()));
    };
    if (m_baseMainSize.isValid()) {
        resize(scaled(m_baseMainSize));
    }
    if ((m_chart != nullptr) && m_baseChartSize.isValid()) {
        m_chart->resize(scaled(m_baseChartSize));
    }
}

// Everything computed OFF the GUI thread reports back here: the AI advisor and the three
// QtConcurrent futures (Monte-Carlo outlook, the per-row plan verdicts, one instrument's
// plan). Out of the constructor so its wiring list stays within the metrics budget.
void MainWindow::connectWorkerResults()
{
    setupRunners();  // same reason this function exists: ctor metrics budget
    static_cast<void>(
        connect(m_aiAdvisor, &AiAdvisor::decisionReady, this, &MainWindow::onAiDecision));
    static_cast<void>(connect(&m_mcWatcher, &QFutureWatcher<trading::McOutlook>::finished,
                              this, [this] {
                                  m_mcBusy = false;
                                  renderMonteCarlo(m_mcWatcher.result());
                              }));
    static_cast<void>(connect(&m_rowPlanWatcher,
                              &QFutureWatcher<QHash<QString, trading::TradePlan>>::finished,
                              this, [this]() { applyRowPlanVerdicts(); }));
    static_cast<void>(connect(&m_planWatcher, &QFutureWatcher<trading::TradePlan>::finished,
                              this, [this] {
                                  renderTradePlanResult(m_planWatcher.result(),
                                                        m_planPendingSymbol,
                                                        m_planPendingIsCurrent);
                              }));
}

// Re-price the open-trades P/L column from the live quote book, in place, so it tracks
// the market between the ~3 s portfolio polls. Every row is marked at ITS instrument's
// current bid (long) or ask (short) with eToro's own identity, so the column reads the
// same as eToro's own screen; a row without a current quote keeps eToro's snapshot
// figure and says so rather than being marked off a price published minutes ago.
void MainWindow::updateOpenTradePnl()
{
    if (m_positionsModel == nullptr) {
        return;
    }
    m_positionsModel->repriceOpenPnl(m_client->quotes(), QDateTime::currentDateTimeUtc());
    updateOpenTradesSummary();
}

void MainWindow::updateOpenTradesSummary()
{
    if ((m_openTradesSummary == nullptr) || (m_positionsModel == nullptr)) {
        return;
    }
    const qint32 trades = m_positionsModel->rowCount();
    if (trades == 0) {
        m_openTradesSummary->setText(QStringLiteral("<b>No open trades.</b>"));
        return;
    }
    const double invested = m_positionsModel->totalInvestedDisplay();
    const double pnl = m_positionsModel->totalPnlDisplay();
    const bool live = m_positionsModel->allPnlLive();
    const QString sign = (pnl < 0.0) ? QStringLiteral("-") : QStringLiteral("+");
    const QColor pnlColor = (pnl >= 0.0) ? trading::ui::kGreen : trading::ui::kRed;
    const QString color = (live ? pnlColor : trading::ui::kGrey).name();
    // "*" carries the same meaning as in the P/L column: at least one row has no current
    // quote, so the total is eToro's last snapshot for that part rather than a live mark.
    const QString marker = live ? QString() : QStringLiteral(" *");
    // Both numbers in their own local: several calls inside one .arg() list are evaluated
    // in an unspecified order (MISRA C++ 2023 4.6.1).
    const QString investedText = QLocale().toString(invested, 'f', 2);
    const QString pnlText = QLocale().toString(std::abs(pnl), 'f', 2);
    m_openTradesSummary->setText(
        QStringLiteral("<b>Invested: %1%2 &nbsp;·&nbsp; P/L: <span style='color:%3'>%4%1%5%6"
                       "</span></b> &nbsp;·&nbsp; %7 open trade%8")
            .arg(m_ccy, investedText, color, sign, pnlText, marker)
            .arg(trades)
            .arg((trades == 1) ? QString() : QStringLiteral("s")));
}

void MainWindow::onPortfolio(const QList<Position> &positions)
{
    // Show trades in a fixed order by position number, so the rows never reshuffle
    // just because the API returned them differently between polls (a reshuffle would
    // force a full rebuild, clearing the ticked checkboxes and any open SL/TP editor).
    // Sorted copy: the parameter is const and the row order must stay stable.
    QList<Position> sorted = positions;
    const auto sortBegin = sorted.begin();
    const auto sortEnd = sorted.end();
    std::sort(sortBegin, sortEnd, [](const Position &a, const Position &b) {
        const qint64 aId = a.positionId.toLongLong();
        const qint64 bId = b.positionId.toLongLong();
        return aId < bId;
    });

    // Mark the entry of each open trade on the shown instrument as a bullet on the
    // chart (green=buy, red=sell). The portfolio can span several instruments in
    // real mode, so filter to the one on screen; the chart clamps older entries
    // into the visible window so they always show.
    const QString shown = m_client->instrument().symbol;
    QList<QPointF> buyEntries;
    QList<QPointF> sellEntries;
    for (const Position &p : std::as_const(sorted)) {
        if ((p.openRate <= 0.0) || (p.symbol.compare(shown, Qt::CaseInsensitive) != 0)) {
            continue;
        }
        const qreal x = p.openTime.isValid()
                            ? static_cast<qreal>(p.openTime.toMSecsSinceEpoch())
                            : static_cast<qreal>(QDateTime::currentMSecsSinceEpoch());
        (p.isBuy ? buyEntries : sellEntries).append(QPointF(x, p.openRate));
    }
    m_chart->setOpenTrades(buyEntries, sellEntries);

    // Authoritative open-trades exposure, used by placeOrder() to enforce the cap.
    m_openTradesTotal = std::accumulate(
        sorted.cbegin(), sorted.cend(), 0.0,
        [](double acc, const Position &p) { return acc + p.amount; });

    refreshClosedTradesForVanished(sorted);

    // Row-indexed snapshot so the SL/TP editors and the click handler can map a
    // row back to its trade. MUST stay in the same order the model is filled with
    // below, or a click lands on a different trade than the one shown.
    m_shownPositions = sorted;

    // Override a position's SL/TP with the value the user just submitted, until the
    // server snapshot converges to it (or the pin times out) — see m_pendingSlTp.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kPendingTtlMs = 12000;  // give the server up to ~12s to reflect it
    auto ratesClose = [](double a, double b) {
        const double scale = std::max({std::abs(a), std::abs(b), 1.0});
        return std::abs(a - b) <= scale * 1e-4;  // absorbs 2dp/5dp round-trip rounding
    };
    auto withPending = [this, nowMs, &ratesClose](const Position &src) -> Position {
        Position p = src;
        const auto it = m_pendingSlTp.find(p.positionId);
        if (it == m_pendingSlTp.end()) {
            return p;
        }
        const bool converged =
            ratesClose(p.stopLossRate, it->slRate) && ratesClose(p.takeProfitRate, it->tpRate);
        if (converged || ((nowMs - it->sinceMs) > kPendingTtlMs)) {
            static_cast<void>(m_pendingSlTp.erase(it));  // server caught up (or we gave up)
            return p;
        }
        p.stopLossRate = it->slRate;    // still pending: show what the user typed
        p.takeProfitRate = it->tpRate;
        return p;
    };

    // Hand the (pin-adjusted) snapshot to the model: same ids in the same order
    // update in place — open SL/TP editors and checkbox marks survive; a changed
    // set resets the model (marks survive by id inside the model).
    QList<Position> pinned;
    pinned.reserve(sorted.size());
    for (const Position &p : std::as_const(sorted)) {
        pinned.append(withPending(p));
    }
    m_positionsModel->setDisplay(m_ccy, m_eurPerUsd);  // keep the FX rate fresh
    m_positionsModel->setPositions(pinned);
    // A new snapshot may have brought positions the model has already judged.
    pushAiOpinionsToPositions();
    // Mark the rows from the quote book right away: a row the poll has just added would
    // otherwise render as "no current quote" until the next tick.
    updateOpenTradePnl();

    // Drop pins for positions no longer open (e.g. closed), so the map can't grow.
    if (!m_pendingSlTp.isEmpty()) {
        QSet<QString> openIds;
        openIds.reserve(sorted.size());
        for (const Position &p : std::as_const(sorted)) {
            static_cast<void>(openIds.insert(p.positionId));
        }
        for (auto it = m_pendingSlTp.begin(); it != m_pendingSlTp.end();) {
            if (openIds.contains(it.key())) {
                ++it;
            } else {
                it = m_pendingSlTp.erase(it);
            }
        }
    }

    // On the first portfolio after startup, adopt the instrument shown at the top of
    // the open-trades list. One-shot: a later-opened trade — or the portfolio re-fetch
    // triggered by the switch itself — must never override the current view.
    if (!m_autoInstrumentDone) {
        m_autoInstrumentDone = true;
        if (!sorted.isEmpty()) {
            const QString top = sorted.first().symbol;
            if (!top.isEmpty()
                && (top.compare(m_client->config().symbol, Qt::CaseInsensitive) != 0)) {
                selectInstrument(top);
            }
        }
    }
}

void MainWindow::onPositionSlTpEdited(qint32 row, qint32 column, const QString &text)
{
    if ((row < 0) || (row >= m_shownPositions.size())) {
        return;
    }
    const qint32 col = column;
    const Position p = m_shownPositions[row];  // copy: the snapshot may change under us
    if ((p.units <= 0.0) || (p.openRate <= 0.0)) {
        appendLog(QStringLiteral("Can't set SL/TP yet — trade has no open rate/units."), true);
        return;
    }

    // Parse a signed display-currency amount (blank / 0 / unparsable = clear).
    auto parseAmount = [this](const QString &raw) -> double {
        QString t = raw.trimmed();
        static_cast<void>(t.remove(m_ccy));
        // Trailing-stop marker "⇅" — not part of the amount.
        static_cast<void>(t.remove(QChar(0x21C5)));
        t = t.trimmed();
        if (t.startsWith(QLatin1Char('+'))) {
            static_cast<void>(t.remove(0, 1));  // keep an explicit '+' from parsing as failure
        }
        bool ok = false;
        double v = QLocale().toDouble(t, &ok);
        if (!ok) {
            v = t.toDouble(&ok);  // fall back to C-locale parsing
        }
        return ok ? v : 0.0;
    };
    // The edited cell arrives as text; the sibling cell is read from the model.
    auto cellValue = [this, row, col, &text, &parseAmount](qint32 /*r*/, qint32 c) -> double {
        if (c == col) {
            return parseAmount(text);
        }
        return parseAmount(
            m_positionsModel->data(m_positionsModel->index(row, c), Qt::EditRole).toString());
    };
    // SL is a SIGNED target P/L: negative closes at a loss (stop on the losing side),
    // positive closes locked in a profit (stop on the winning side). TP is a profit.
    // Cells are entered in the display currency (euro); convert to the account
    // currency (USD) for the rate math below, keeping the euro values to echo back.
    const double slPnlDisp = cellValue(row, PosColSl);
    const double tpAmtDisp = std::max(0.0, cellValue(row, PosColTp));
    const double slPnl = fromDisplay(slPnlDisp);
    const double tpAmt = fromDisplay(tpAmtDisp);

    // Signed P/L -> trigger rate. Long: rate = open + pnl/valuePerPoint (pnl>0 above
    // open = profit, pnl<0 below = loss); short mirrors it. TP is always on the profit
    // side. valuePerPoint converts the account-currency amount to a price distance in
    // the instrument's quote currency (see accountValuePerPoint) — dividing by raw
    // units mis-scales the stop for non-account-currency instruments (e.g. HKG50/HKD).
    const double perPoint = trading::accountValuePerPoint(p);
    const double slRate = ((slPnl != 0.0) && (perPoint > 0.0))
                              ? (p.isBuy ? (p.openRate + (slPnl / perPoint))
                                         : (p.openRate - (slPnl / perPoint)))
                              : 0.0;
    const double tpRate = ((tpAmt > 0.0) && (perPoint > 0.0))
                              ? (p.isBuy ? (p.openRate + (tpAmt / perPoint))
                                         : (p.openRate - (tpAmt / perPoint)))
                              : 0.0;

    // A stop-loss rate must stay positive; ignore an over-large loss that would push
    // it to/through zero (clears instead of sending a nonsensical rate).
    const double slRateOut = (slRate > 0.0) ? slRate : 0.0;

    // Format a signed amount as e.g. "-$50.00" / "+$50.00".
    auto signedCcy = [this](double v) {
        const QString amount = QLocale().toString(std::abs(v), 'f', 2);
        return ((v < 0.0) ? QStringLiteral("-") : QStringLiteral("+")) + m_ccy + amount;
    };

    // Echo the accepted values immediately: the model renders SL/TP from the
    // rates, so the cells show the normalised amounts without a poll round-trip.
    m_positionsModel->setSlTpRates(row, (slRate > 0.0) ? slRate : 0.0, tpRate);

    const QString slStr = (slPnlDisp != 0.0) ? signedCcy(slPnlDisp) : QStringLiteral("none");
    const QString tpStr = (tpAmtDisp > 0.0)
                              ? (m_ccy + QLocale().toString(tpAmtDisp, 'f', 2))
                              : QStringLiteral("none");
    const QString id = p.positionId;
    const bool trailing = p.trailingStop;

    // Pin the submitted rates so the periodic portfolio poll can't revert the cell
    // before the server reflects the change (onPortfolio clears this once the
    // snapshot converges to it, or after a short timeout).
    m_pendingSlTp[id] = {slRateOut, tpRate, QDateTime::currentMSecsSinceEpoch()};
    // Defer the client call: in simulation it emits portfolioUpdated synchronously,
    // which would rebuild the table (deleting this very item) from inside its own
    // itemChanged emission. A zero-timer runs it after the signal has unwound.
    QTimer::singleShot(0, this, [this, id, slRateOut, tpRate, trailing, slStr, tpStr] {
        m_client->modifyPosition(id, slRateOut, tpRate, trailing);
        appendLog(QStringLiteral("Trade #%1: SL %2 / TP %3 requested.").arg(id, slStr, tpStr));
    });
}

void MainWindow::renderMonteCarlo(const trading::McOutlook &mc)
{
    if ((m_aiMonteCarlo == nullptr) || (m_aiEdge == nullptr)) {
        return;
    }
    const QString green = trading::ui::greenHex();
    const QString red = trading::ui::redHex();
    const QString amber = trading::ui::amberHex();

    if (mc.valid) {
        const QString mcColor = (mc.pUp >= 0.55) ? green : ((mc.pUp <= 0.45) ? red : amber);
        const QString p5Text = QString::number(mc.p5, 'f', trading::priceDecimals(mc.p5));
        const QString p95Text = QString::number(mc.p95, 'f', trading::priceDecimals(mc.p95));
        m_aiMonteCarlo->setText(colored(QStringLiteral("P(up) %1%  ·  range %2–%3")
                                            .arg(std::lround(mc.pUp * 100.0))
                                            .arg(p5Text, p95Text),
                                        mcColor));
    } else {
        m_aiMonteCarlo->setText(QStringLiteral("—"));
    }

    const bool preferLong = m_mcScore >= 0;  // evaluate the ensemble's favoured side
    const double pWin = preferLong ? mc.pWinLong : mc.pWinShort;
    // Win-rate needed to break even for this reward:risk.
    const double breakeven =
        ((m_mcTp + m_mcSl) > 0.0) ? (m_mcSl / (m_mcTp + m_mcSl)) : 0.0;
    const double edge = pWin - breakeven;
    const bool edgeValid =
        mc.valid && (m_mcTp > 0.0) && (m_mcSl > 0.0) && (m_mcExposure > 0.0);
    m_lastMcEdge = edge;
    m_lastMcEdgeValid = edgeValid;
    if (edgeValid) {
        const QString side = preferLong ? QStringLiteral("BUY") : QStringLiteral("SELL");
        const QString eColor = (edge > 0.02) ? green : ((edge < -0.02) ? red : amber);
        const QString verdict = (edge > 0.02)
                                    ? QStringLiteral("favourable")
                                    : ((edge < -0.02) ? QStringLiteral("unfavourable")
                                                      : QStringLiteral("marginal"));
        const qint64 edgePct = std::llround(edge * 100.0);
        const QString edgeStr = ((edgePct >= 0) ? QStringLiteral("+") : QString())
                                + QString::number(edgePct);
        m_aiEdge->setText(colored(
            QStringLiteral("%1 win %2% vs break-even %3% → %4% (%5)")
                .arg(side)
                .arg(std::lround(pWin * 100.0))
                .arg(std::lround(breakeven * 100.0))
                .arg(edgeStr, verdict),
            eColor));
    } else {
        m_aiEdge->setText(colored(
            QStringLiteral("set amount / stop-loss / take-profit"), amber));
    }
}

// Shared values of one updateSignals() pass — the indicator inputs and intermediate
// verdicts its render helpers exchange. Created afresh by updateSignals() and threaded
// through each helper by reference (TradePlan.cpp's PlanContext pattern); defined here
// rather than in the header so the fields stay file-local.
struct MainWindow::SignalsContext {
    QList<double> series;     // hourly closes, forming bar pinned to the live price
    QString green;            // shared row colours (hex)
    QString red;
    QString amber;
    double fast = 0.0;        // SMA 10
    double slow = 0.0;        // SMA 30
    double r = 0.0;           // RSI 14
    double hist = 0.0;        // MACD histogram
    double pctB = 0.0;        // Bollinger %B (20)
    double vol = 0.0;         // per-bar σ of returns (20 bars), in percent
    double momentum = 0.0;    // 10-bar rate of change
    double changePct = 0.0;   // change across the loaded window, in percent
    bool bull = false;        // fast SMA above slow
    trading::Regression reg;  // least-squares trend over the last 30 closes
    trading::Knn kn;          // k-nearest-neighbours analog forecast
    double stochK = 0.0;      // stochastic %K (14)
    double sma50 = 0.0;       // 50-bar SMA trend filter
    bool aboveTrend = false;  // price above the 50-bar SMA
    qint32 score = 0;         // ensemble net vote
    qint32 votes = 0;         // ensemble vote count
    double confidence = 0.0;  // ensemble confidence after the VIX/event haircuts
    qint32 signalDir = 0;     // +1 BUY / -1 SELL / 0 NEUTRAL — feeds the chart arrow
    double pUp = 0.0;         // logistic up-probability
    double hurst = 0.0;       // Hurst exponent of recent returns
    qint32 adviceDir = 0;     // +1 BUY / -1 SELL / 0 HOLD — feeds the chart arrow
};

void MainWindow::renderRegimeAndNewsRows()
{
    // VIX / calendar regime and news sentiment — the same two sources the Decision
    // window uses, surfaced here as signal rows. Independent of the price series, so
    // set them before any early return.
    const QString green = trading::ui::greenHex();
    const QString red = trading::ui::redHex();
    const QString amber = trading::ui::amberHex();
    const QString grey = trading::ui::greyHex();

    const trading::RegimeRead regimeRead = trading::marketRegime(marketSnapshot());
    const double regime = regimeRead.tilt;
    const bool eventRisk = regimeRead.eventRisk;
    if (!m_vixValid && !eventRisk) {
        m_sigRegime->setText(QStringLiteral("gathering data…"));
    } else {
        const QString word = (regime > 0.05)
                                 ? QStringLiteral("Risk-on ▲")
                                 : ((regime < -0.05) ? QStringLiteral("Risk-off ▼")
                                                     : QStringLiteral("Neutral"));
        const QString col = (regime > 0.05) ? green : ((regime < -0.05) ? red : amber);
        QString txt = QStringLiteral("<span style='color:%1'>%2</span>").arg(col, word);
        if (m_vixValid) {
            txt += QStringLiteral(" <span style='color:%1'>(VIX %2)</span>")
                       .arg(grey).arg(m_vix, 0, 'f', 1);
        }
        if (eventRisk) {
            txt += QStringLiteral(" <span style='color:%1'>⚠ event &lt;6h</span>").arg(amber);
        }
        m_sigRegime->setText(txt);
    }

    const QList<NewsHeadline> news = m_newsBySymbol.value(m_client->config().symbol);
    if (news.isEmpty()) {
        m_sigNews->setText(QStringLiteral("<span style='color:%1'>n/a</span>").arg(grey));
    } else {
        const trading::NewsRead newsRead = trading::newsSentimentScore(news);
        const double s = newsRead.score;
        const qint32 newsCount = newsRead.count;
        const QString word = (s > 0.1) ? QStringLiteral("Positive ▲")
                                       : ((s < -0.1) ? QStringLiteral("Negative ▼")
                                                     : QStringLiteral("Neutral"));
        const QString col = (s > 0.1) ? green : ((s < -0.1) ? red : amber);
        m_sigNews->setText(QStringLiteral("<span style='color:%1'>%2</span> "
                                          "<span style='color:%3'>(%4, %5 headlines)</span>")
                               .arg(col, word, grey)
                               .arg(s, 0, 'f', 2)
                               .arg(newsCount));
    }
}

void MainWindow::renderGatheringDataRows()
{
    const QString wait = QStringLiteral("gathering data…");
    for (QLabel *l : {m_sigTrend, m_sigMomentum, m_sigMacd, m_sigBoll, m_sigVol,
                      m_sigRegression, m_sigKnn, m_sigStoch, m_sigTrend50, m_sigRisk,
                      m_sigChange}) {
        l->setText(wait);
    }
    m_sigPrediction->setText(QStringLiteral("—"));
    m_sig3h->setText(QStringLiteral("—"));
    m_sig3d->setText(QStringLiteral("—"));
    m_sigOverall->setText(QStringLiteral("—"));
    for (QLabel *l : {m_aiUpProb, m_aiMonteCarlo, m_aiEdge, m_aiRegime, m_aiAdvice}) {
        l->setText(wait);
    }
    m_chart->setPredictionDirection(0);
    m_forecastTarget = 0.0;  // no corridor → the close watchdog stands down
    m_lastSignalDir = 0;
}

void MainWindow::renderIndicatorRows(SignalsContext &ctx)
{
    ctx.bull = ctx.fast > ctx.slow;
    m_sigTrend->setText(colored(ctx.bull ? QStringLiteral("Bullish ▲") : QStringLiteral("Bearish ▼"),
                                ctx.bull ? ctx.green : ctx.red));

    QString rsiState = QStringLiteral("Neutral");
    QString rsiColor = ctx.amber;
    if (ctx.r >= 70.0) {
        rsiState = QStringLiteral("Overbought");
        rsiColor = ctx.red;
    } else if (ctx.r <= 30.0) {
        rsiState = QStringLiteral("Oversold");
        rsiColor = ctx.green;
    }
    m_sigMomentum->setText(
        colored(QStringLiteral("%1 (%2)").arg(ctx.r, 0, 'f', 1).arg(rsiState), rsiColor));

    m_sigMacd->setText(colored(ctx.hist >= 0.0 ? QStringLiteral("Bullish ▲") : QStringLiteral("Bearish ▼"),
                               ctx.hist >= 0.0 ? ctx.green : ctx.red));

    QString bollState = QStringLiteral("mid-band");
    QString bollColor = ctx.amber;
    if (ctx.pctB >= 0.9) {
        bollState = QStringLiteral("upper — stretched");
        bollColor = ctx.red;
    } else if (ctx.pctB <= 0.1) {
        bollState = QStringLiteral("lower — stretched");
        bollColor = ctx.green;
    }
    m_sigBoll->setText(colored(QStringLiteral("%1 (%2)").arg(ctx.pctB, 0, 'f', 2).arg(bollState), bollColor));

    m_sigVol->setText(colored(QStringLiteral("±%1%/bar").arg(ctx.vol, 0, 'f', 3), ctx.amber));
}

void MainWindow::renderForecastModelRows(SignalsContext &ctx)
{
    // Least-squares regression trend over the last 30 closes (slope + R² fit).
    ctx.reg = trading::linRegForecast(ctx.series, 30);
    const QString regDir = (ctx.reg.slopePct > 0.0)
                               ? QStringLiteral("↑")
                               : ((ctx.reg.slopePct < 0.0) ? QStringLiteral("↓")
                                                           : QStringLiteral("→"));
    const QString slopeText = QString::number(ctx.reg.slopePct, 'f', 3);
    const QString r2Text = QString::number(ctx.reg.r2, 'f', 2);
    m_sigRegression->setText(colored(
        regDir + QLatin1Char(' ') + slopeText + QStringLiteral("%/bar  R² ") + r2Text,
        (ctx.reg.slopePct > 0.0) ? ctx.green : ((ctx.reg.slopePct < 0.0) ? ctx.red : ctx.amber)));

    // k-Nearest-Neighbors analog forecast: match the current 10-bar pattern to
    // history and average what followed the 5 closest analogs.
    ctx.kn = trading::knnForecast(ctx.series, 10, 5);
    const QString knnDir = (ctx.kn.retPct > 0.0)
                               ? QStringLiteral("↑")
                               : ((ctx.kn.retPct < 0.0) ? QStringLiteral("↓")
                                                        : QStringLiteral("→"));
    const QString knnRetText = QString::number(ctx.kn.retPct, 'f', 3);
    const QString knnAgreeText = QString::number(ctx.kn.agree * 100.0, 'f', 0);
    m_sigKnn->setText(colored(
        knnDir + QLatin1Char(' ') + ((ctx.kn.retPct >= 0.0) ? QStringLiteral("+") : QString())
            + knnRetText + QStringLiteral("%  (") + knnAgreeText + QStringLiteral("% agree)"),
        (ctx.kn.retPct > 0.0) ? ctx.green : ((ctx.kn.retPct < 0.0) ? ctx.red : ctx.amber)));
}

void MainWindow::renderTimingAndRiskRows(SignalsContext &ctx)
{
    // Stochastic %K — entry timing (oversold in an uptrend is a good long entry).
    ctx.stochK = trading::stochasticK(ctx.series, 14);
    QString stochState = QStringLiteral("mid");
    QString stochColor = ctx.amber;
    if (ctx.stochK >= 80.0) {
        stochState = QStringLiteral("overbought");
        stochColor = ctx.red;
    } else if (ctx.stochK <= 20.0) {
        stochState = QStringLiteral("oversold");
        stochColor = ctx.green;
    }
    m_sigStoch->setText(
        colored(QStringLiteral("%1 (%2)").arg(ctx.stochK, 0, 'f', 1).arg(stochState), stochColor));

    // Trend filter: price above the 50-bar SMA = long-friendly regime.
    ctx.sma50 = trading::sma(ctx.series, 50);
    ctx.aboveTrend = (ctx.sma50 > 0.0) && (ctx.series.last() > ctx.sma50);
    m_sigTrend50->setText(
        (ctx.sma50 <= 0.0) ? colored(QStringLiteral("n/a"), ctx.amber)
                           : colored(ctx.aboveTrend ? QStringLiteral("above ▲ (uptrend)")
                                                    : QStringLiteral("below ▼ (downtrend)"),
                                     ctx.aboveTrend ? ctx.green : ctx.red));

    // Risk gauge for the selected leverage: expected ~1h move × leverage = the
    // swing in your margin. High leverage makes small moves large P/L. On the hourly
    // series one bar IS one hour, so the per-hour move is just the per-bar σ.
    const double lev = m_leverage->currentText().toDouble();
    const double hourMovePct = ctx.vol;   // per-bar σ = per-hour σ (1 hourly bar = 1h)
    const double marginSwing = hourMovePct * lev;
    const QString riskColor =
        (marginSwing >= 30.0) ? ctx.red : ((marginSwing >= 15.0) ? ctx.amber : ctx.green);
    m_sigRisk->setText(colored(QStringLiteral("±%1%/h → ±%2% margin (x%3)")
                                   .arg(hourMovePct, 0, 'f', 2)
                                   .arg(marginSwing, 0, 'f', 0)
                                   .arg(lev, 0, 'f', 0),
                               riskColor));

    m_sigChange->setText(colored(QStringLiteral("%1%2%")
                                     .arg((ctx.changePct >= 0.0) ? QStringLiteral("+") : QString())
                                     .arg(ctx.changePct, 0, 'f', 2),
                                 (ctx.changePct >= 0.0) ? ctx.green : ctx.red));
}

void MainWindow::renderPredictionRow(SignalsContext &ctx)
{
    // --- Ensemble prediction ("model" vote across the indicators) ----------
    // The directional vote across the indicators is computed by the shared
    // trading::computeEnsemble() (the leverage screener uses the same call, so a symbol
    // ranks identically here and there). It returns the net score, the vote count
    // and the raw confidence; the VIX-level and event-risk confidence haircuts are
    // applied just below. Volatility sets the expected move.
    const trading::Ensemble ens = trading::computeEnsemble(ctx.series, m_vixValid, m_vixChangePct);
    ctx.score = ens.score;
    ctx.votes = ens.votes;
    ctx.confidence = ens.confidence;

    // High absolute VIX = a fearful, choppy tape: trim confidence with the SHARED
    // domain haircut (the screener and the trade planner apply the same one, so a
    // symbol's confidence reads identically in all three panels).
    QString vixNote;
    if (m_vixValid && (m_vix >= 25.0)) {
        vixNote = QStringLiteral(" · VIX %1").arg(m_vix, 0, 'f', 0);
    }
    ctx.confidence = trading::applyVixHaircut(ctx.confidence, m_vixValid, m_vix);

    // Event risk: an imminent calendar event lowers confidence and flags volatility.
    QString eventNote;
    if (m_nextEventTime.isValid()) {
        const qint64 mins = QDateTime::currentDateTime().secsTo(m_nextEventTime) / 60;
        if ((mins >= 0) && (mins <= 60)) {
            ctx.confidence *= 0.5;
            eventNote = QStringLiteral(" ⚠ %1 in %2m").arg(m_nextEventTitle).arg(mins);
        }
    }
    eventNote += vixNote;

    const QString arrow = (ctx.score > 0) ? QStringLiteral("↑")
                                          : ((ctx.score < 0) ? QStringLiteral("↓")
                                                             : QStringLiteral("→"));
    const QString dirWord = (ctx.score > 0) ? QStringLiteral("up")
                                            : ((ctx.score < 0) ? QStringLiteral("down")
                                                               : QStringLiteral("flat"));
    const QString predColor = (ctx.score > 0) ? ctx.green : ((ctx.score < 0) ? ctx.red : ctx.amber);
    const QString confText = QString::number(ctx.confidence, 'f', 0);
    const QString volText = QString::number(ctx.vol, 'f', 2);
    const QString predText = arrow + QLatin1Char(' ') + dirWord + QStringLiteral("  ~")
                             + confText + QStringLiteral("% conf, ±")
                             + volText + QLatin1Char('%') + eventNote;
    m_sigPrediction->setText(colored(predText, predColor));
    // NB: the chart arrow is now driven by the overall Signal + AI Recommendation
    // (set at the end of updateSignals()), not by the raw ensemble score.
}

void MainWindow::render3hForecastRow(const SignalsContext &ctx)
{
    // --- 3-hour forecast ---------------------------------------------------
    // Extrapolate the recent drift over a 3-hourly-bar horizon, with a ±1σ range
    // that scales with √horizon (random-walk diffusion). One bar = one hour now.
    constexpr qint32 kHorizonBars = 3;  // 3 hours = 3 hourly bars
    const qsizetype driftWin = qMin<qsizetype>(ctx.series.size() - 1, 48);  // ~2 days of hours
    const double drift = trading::meanReturn(ctx.series, driftWin);
    const double projPct = (std::pow(1.0 + drift, kHorizonBars) - 1.0) * 100.0;
    const double bandPct = ctx.vol * std::sqrt(static_cast<double>(kHorizonBars));
    const double target = m_lastPrice * std::pow(1.0 + drift, kHorizonBars);
    const QString f3Arrow = (projPct > 0.0)
                                ? QStringLiteral("↑")
                                : ((projPct < 0.0) ? QStringLiteral("↓") : QStringLiteral("→"));
    const QString f3Color = (projPct > 0.0) ? ctx.green : ((projPct < 0.0) ? ctx.red : ctx.amber);
    const QString f3Change = QString::number(projPct, 'f', 2);
    const QString f3Target = QString::number(target, 'f', trading::priceDecimals(target));
    const QString f3Band = QString::number(bandPct, 'f', 1);
    const QString f3Text = f3Arrow + QLatin1Char(' ')
                           + ((projPct >= 0.0) ? QStringLiteral("+") : QString())
                           + f3Change + QStringLiteral("% → ~")
                           + f3Target + QStringLiteral("  (±")
                           + f3Band + QStringLiteral("%)");
    m_sig3h->setText(colored(f3Text, f3Color));
    // The 3h target ± band is the "prediction corridor" the close watchdog
    // (checkCloseProposals) tests open positions against.
    m_forecastTarget = target;
    m_forecastBandPct = bandPct;
}

void MainWindow::render3dForecastRow(SignalsContext &ctx)
{
    // --- AI forecast: next 3 trading days ----------------------------------
    // Ensemble-driven, bounded projection. The indicator vote (score/votes) sets
    // both direction and how far within the expected ±1σ range over the horizon
    // (√-of-time diffusion) price is likely to travel — so it never runs away the
    // way naive per-minute drift compounded over thousands of bars would.
    constexpr qint32 kHoursPerDay = 24;  // most of these are 24/7 CFDs; hourly bars now
    constexpr qint32 kForecastDays = 3;
    constexpr qint32 kDaysHorizon = kForecastDays * kHoursPerDay;  // 72 hourly bars
    const double dayRangePct = ctx.vol * std::sqrt(static_cast<double>(kDaysHorizon));
    const double biasFrac =
        (ctx.votes > 0) ? (ctx.score / static_cast<double>(ctx.votes)) : 0.0;  // [-1, 1]
    const double dayProjPct = biasFrac * dayRangePct;
    const double dayTarget = m_lastPrice * (1.0 + (dayProjPct / 100.0));
    const QString fdArrow = (dayProjPct > 0.0)
                                ? QStringLiteral("↑")
                                : ((dayProjPct < 0.0) ? QStringLiteral("↓")
                                                      : QStringLiteral("→"));
    const QString fdColor = (dayProjPct > 0.0) ? ctx.green : ((dayProjPct < 0.0) ? ctx.red : ctx.amber);
    const QString fdChange = QString::number(dayProjPct, 'f', 2);
    const QString fdTarget = QString::number(dayTarget, 'f', trading::priceDecimals(dayTarget));
    const QString fdBand = QString::number(dayRangePct, 'f', 1);
    const QString fdConf = QString::number(ctx.confidence, 'f', 0);
    const QString fdText = fdArrow + QLatin1Char(' ')
                           + ((dayProjPct >= 0.0) ? QStringLiteral("+") : QString())
                           + fdChange + QStringLiteral("% → ~")
                           + fdTarget + QStringLiteral("  (±")
                           + fdBand + QStringLiteral("%, ")
                           + fdConf + QStringLiteral("% conf)");
    m_sig3d->setText(colored(fdText, fdColor));
}

void MainWindow::renderOverallSignalRow(SignalsContext &ctx)
{
    // Overall signal from the same ensemble (needs a clear majority).
    QString signal = QStringLiteral("NEUTRAL");
    QString sigColor = ctx.amber;
    if (ctx.score >= 2) {
        signal = QStringLiteral("BUY");
        sigColor = ctx.green;
        ctx.signalDir = 1;
    } else if (ctx.score <= -2) {
        signal = QStringLiteral("SELL");
        sigColor = ctx.red;
        ctx.signalDir = -1;
    } else {
        // no clear majority — keep the NEUTRAL defaults
    }
    m_sigOverall->setText(colored(signal, sigColor));
    // Remember the call for the close watchdog (a confident flip against an open
    // position is one of its triggers).
    m_lastSignalDir = ctx.signalDir;
    // Scripted trading: SIGNALS-flagged entries follow the ensemble + AI call.
    m_scriptRunner->setSignalState(
        m_lastSignalDir, aiDecisionDir(m_aiDecision), m_aiAdvisor->isConfigured());
    m_lastSignalConf = ctx.confidence;
}

void MainWindow::renderUpProbabilityRow(SignalsContext &ctx)
{
    // 1) Logistic up-probability: a hand-weighted logistic model over the same
    //    features the indicators expose, squashed through a sigmoid.
    double z = (1.0 * (ctx.bull ? 1.0 : -1.0)) + (0.8 * ((ctx.hist >= 0.0) ? 1.0 : -1.0))
             + (0.05 * (ctx.r - 50.0)) + (0.02 * (50.0 - ctx.stochK));
    if (ctx.vol > 0.0) {
        z += 0.5 * (ctx.momentum / ctx.vol);
        if (ctx.reg.valid) {
            z += 0.5 * (ctx.reg.slopePct / ctx.vol);
        }
        if (ctx.kn.k > 0) {
            z += 0.4 * (ctx.kn.retPct / ctx.vol);
        }
    }
    if (ctx.sma50 > 0.0) {
        z += 0.6 * (ctx.aboveTrend ? 1.0 : -1.0);
    }
    ctx.pUp = trading::sigmoid(0.5 * z);
    const QString upArrow = (ctx.pUp >= 0.55) ? QStringLiteral("▲")
                                              : ((ctx.pUp <= 0.45) ? QStringLiteral("▼")
                                                                   : QStringLiteral("→"));
    const QString upColor = (ctx.pUp >= 0.55) ? ctx.green : ((ctx.pUp <= 0.45) ? ctx.red : ctx.amber);
    m_aiUpProb->setText(colored(
        QStringLiteral("%1 %2% up").arg(upArrow).arg(std::lround(ctx.pUp * 100.0)), upColor));
}

void MainWindow::dispatchMonteCarlo(const SignalsContext &ctx)
{
    // 2)+3) Bootstrap Monte-Carlo over the 3h horizon, reused for the price
    //       outlook and the take-profit-before-stop-loss edge of the user's setup.
    const double amount = m_amount->value();
    const double leverage = m_leverage->currentText().toDouble();
    const double exposure = amount * leverage;
    const double tp = m_takeProfit->value();
    const double sl = m_stopLoss->value();
    const double tpFrac = (exposure > 0.0) ? (tp / exposure) : 0.0;
    const double slFrac = (exposure > 0.0) ? (sl / exposure) : 0.0;
    // The 1200-path bootstrap Monte-Carlo used to run synchronously here — on
    // every price tick, on the GUI thread. It now runs in the global thread
    // pool; renderMonteCarlo() applies the result. While one run is in flight
    // new ticks are skipped — the next tick re-triggers with fresher inputs.
    if (!m_mcBusy && (m_aiMonteCarlo != nullptr)) {
        m_mcBusy = true;
        m_mcScore = ctx.score;
        m_mcTp = tp;
        m_mcSl = sl;
        m_mcExposure = exposure;
        const double mcPrice = m_lastPrice;
        // series is captured by value — a cheap copy-on-write share for the worker.
        m_mcWatcher.setFuture(QtConcurrent::run([series = ctx.series, mcPrice, tpFrac, slFrac] {
            constexpr qint32 kMcHorizonBars = 3;  // 3 hours = 3 hourly bars
            return trading::monteCarlo(series, {.price = mcPrice,
                                                .horizon = kMcHorizonBars,
                                                .tpFrac = tpFrac,
                                                .slFrac = slFrac,
                                                .paths = 1200});
        }));
    }
}

void MainWindow::renderAiRegimeRow(SignalsContext &ctx)
{
    // 4) Market regime from the Hurst exponent.
    ctx.hurst = trading::hurstExponent(ctx.series);
    QString regime = QStringLiteral("Random walk");
    QString regimeColor = ctx.amber;
    if (ctx.hurst >= 0.55) {
        regime = QStringLiteral("Trending");
        regimeColor = ctx.green;
    } else if (ctx.hurst <= 0.45) {
        regime = QStringLiteral("Mean-reverting");
        regimeColor = ctx.amber;
    } else {
        // in between — keep the "Random walk" default
    }
    m_aiRegime->setText(colored(
        QStringLiteral("%1 (H %2)").arg(regime, QString::number(ctx.hurst, 'f', 3)), regimeColor));
}

void MainWindow::renderAdviceRow(SignalsContext &ctx)
{
    // 5) Explicit BUY / SELL / HOLD call, with a one-line rationale.
    const bool bullishLean = (ctx.score >= 2) && (ctx.pUp >= 0.50);
    const bool bearishLean = (ctx.score <= -2) && (ctx.pUp <= 0.50);
    QString advice;
    QString adviceColor = ctx.amber;
    if (bullishLean) {
        advice = QStringLiteral("BUY");
        adviceColor = ctx.green;
        ctx.adviceDir = 1;
    } else if (bearishLean) {
        advice = QStringLiteral("SELL");
        adviceColor = ctx.red;
        ctx.adviceDir = -1;
    } else {
        advice = QStringLiteral("HOLD — no clear edge, stay flat");
    }
    if (bullishLean || bearishLean) {
        if (ctx.hurst >= 0.55) {
            advice += QStringLiteral(" — trend-following favoured");
        } else if (ctx.hurst <= 0.45) {
            advice += QStringLiteral(" — choppy, size down / fade extremes");
        } else {
            // random-walk regime — nothing extra to add
        }
        // From the last completed (async) Monte-Carlo run — at most one tick old.
        if (m_lastMcEdgeValid && (m_lastMcEdge < 0.0)) {
            advice += QStringLiteral("; TP/SL give negative edge — widen TP or tighten entry");
        }
    }
    if (m_nextEventTime.isValid()) {
        const qint64 mins = QDateTime::currentDateTime().secsTo(m_nextEventTime) / 60;
        if ((mins >= 0) && (mins <= 30)) {
            advice += QStringLiteral("  ⚠ high-impact event imminent");
        }
    }
    m_aiAdvice->setText(colored(advice, adviceColor));
}

void MainWindow::updateSignals()
{
    // The local model's row is refreshed here too, so it says what it knows from the
    // first tick — "no opinion yet" is information, "…" is not.
    updateLocalModelSignal();
    updateConfluenceSignal();
    // Keep the estimated opening (spread) cost in step with the live quote and the
    // current amount/leverage — this slot is wired to all three. Done before the
    // early return below so the cost still shows while the signal series fills.
    updateOpenCost();

    // Compute over the HOURLY close series (one bar = one hour), with the last
    // (forming-hour) bar pinned to the live price so the signals still track the
    // market. Hourly — not the minute tail — so a symbol reads the same here as in
    // the leverage screener. Horizon maths below is therefore in hourly bars.
    SignalsContext ctx;
    ctx.series = m_hourlyCloses;
    if (m_lastPrice > 0.0) {
        if (ctx.series.isEmpty()) {
            ctx.series.append(m_lastPrice);
        } else {
            ctx.series.last() = m_lastPrice;
        }
    }

    renderRegimeAndNewsRows();

    constexpr qsizetype kFast = 10;
    constexpr qsizetype kSlow = 30;
    constexpr qsizetype kRsi = 14;

    if (ctx.series.size() < (kSlow + 1)) {
        renderGatheringDataRows();
        return;
    }

    ctx.fast = trading::sma(ctx.series, kFast);
    ctx.slow = trading::sma(ctx.series, kSlow);
    ctx.r = trading::rsi(ctx.series, kRsi);
    ctx.hist = trading::macdHistogram(ctx.series);
    ctx.pctB = trading::bollingerPercentB(ctx.series, 20);
    ctx.vol = trading::volatilityPct(ctx.series, 20);
    ctx.momentum = trading::roc(ctx.series, 10);
    const double first = ctx.series.first();
    ctx.changePct =
        (first > 0.0) ? (((ctx.series.last() - first) / first) * 100.0) : 0.0;

    ctx.green = trading::ui::greenHex();
    ctx.red = trading::ui::redHex();
    ctx.amber = trading::ui::amberHex();

    // --- Individual indicators ---------------------------------------------
    renderIndicatorRows(ctx);
    renderForecastModelRows(ctx);
    renderTimingAndRiskRows(ctx);

    renderPredictionRow(ctx);

    render3hForecastRow(ctx);
    render3dForecastRow(ctx);
    renderOverallSignalRow(ctx);

    // Keep the SL/TP defaults tracking volatility while the user hasn't taken
    // over — done before the edge estimate below so it prices the same values.
    proposeSlTpDefaults(ctx.vol);

    // --- AI decision support ------------------------------------------------
    renderUpProbabilityRow(ctx);
    dispatchMonteCarlo(ctx);
    renderAiRegimeRow(ctx);
    renderAdviceRow(ctx);

    // Chart arrow = agreement of the overall Signal and the AI Recommendation:
    // both bullish → ▲, both bearish → ▼, a lean either way → the leaning arrow,
    // and a conflict (BUY vs SELL) or both flat → no arrow.
    m_chart->setPredictionDirection(ctx.signalDir + ctx.adviceDir);
}

void MainWindow::onCash(double available, const QString &currency)
{
    Q_UNUSED(currency);  // account currency is USD; the UI shows euro (see onFxRate)
    m_availableCash = available;  // USD; converted to euro for display / suggestions

    const QString amount = QLocale().toString(toDisplay(available), 'f', 2);
    m_cashLabel->setText(QStringLiteral("Available for trading: %1 EUR").arg(amount));
}

void MainWindow::onFxRate(double eurPerUsd)
{
    if (eurPerUsd <= 0.0) {
        return;
    }
    const bool firstRate = m_eurPerUsd <= 0.0;
    m_eurPerUsd = eurPerUsd;
    // Refresh the cash line right away; the periodic portfolio poll re-renders the
    // rest of the euro figures within a second or two.
    if (m_availableCash > 0.0) {
        onCash(m_availableCash, QString());
    }
    // The open-trades totals are euro figures too — restate them at the new rate rather
    // than leaving them a poll behind the cells they sum.
    updateOpenTradesSummary();
    if (firstRate) {
        appendLog(QStringLiteral("EUR/USD %1 — account figures shown in euro.")
                      .arg(QLocale().toString(1.0 / eurPerUsd, 'f', 4)));
    }
}

void MainWindow::onEvents(const QList<EconomicEvent> &events)
{
    m_eventList = events;  // kept for the chart's imminent-event line
    rebuildEventsView(/*force=*/true);
    refreshChartEventMarker();
}

void MainWindow::rebuildEventsView(bool force)
{
    // An event stays listed until 10 minutes after its time, then drops off.
    constexpr qint64 kKeepPastSecs = 10LL * 60;
    const QDateTime now = QDateTime::currentDateTime();

    // Recompute the soonest still-upcoming event (event-risk term of the prediction)
    // and count what should currently be visible, in one pass.
    m_nextEventTime = QDateTime();
    m_nextEventTitle.clear();
    qint32 visibleCount = 0;
    for (const EconomicEvent &e : std::as_const(m_eventList)) {
        if (!e.when.isValid()) {
            continue;
        }
        if ((e.when > now) && (!m_nextEventTime.isValid() || (e.when < m_nextEventTime))) {
            m_nextEventTime = e.when;
            m_nextEventTitle = e.title;
        }
        if (e.when.secsTo(now) <= kKeepPastSecs) {  // future, or passed ≤10 min ago
            ++visibleCount;
        }
    }

    // Skip the (flicker-prone) rebuild on a periodic tick when nothing aged out.
    if (!force && (visibleCount == m_shownEventCount)) {
        return;
    }
    m_shownEventCount = visibleCount;

    m_events->clear();
    if (visibleCount == 0) {
        m_events->addItem(m_eventList.isEmpty()
                              ? QStringLiteral("No further high-impact events scheduled this week.")
                              : QStringLiteral("No upcoming events in view."));
        return;
    }
    const QString sym = m_client->config().symbol;  // current instrument, for the read-out
    for (const EconomicEvent &e : std::as_const(m_eventList)) {
        if (!e.when.isValid() || (e.when.secsTo(now) > kKeepPastSecs)) {
            continue;  // passed more than 10 minutes ago — drop it
        }
        // Show the event time in the computer's local timezone, with its label.
        const QString stamp = e.when.toLocalTime().toString(QStringLiteral("ddd dd MMM  HH:mm t"));
        const trading::ImpactGuess guess = trading::guessImpact(e);
        // Prefix the region code (e.g. "US", "EU") since events can now span regions.
        const QString label =
            e.country.isEmpty() ? e.title : QStringLiteral("%1 %2").arg(e.country, e.title);
        QString line = QStringLiteral("%1   %2   [%3]").arg(stamp, label, e.impact);
        if (!e.forecast.isEmpty() || !e.previous.isEmpty()) {
            line += QStringLiteral("   (fc %1 / prev %2)")
                        .arg(e.forecast.isEmpty() ? QStringLiteral("—") : e.forecast,
                             e.previous.isEmpty() ? QStringLiteral("—") : e.previous);
        }
        line += QStringLiteral("   →  %1 %2").arg(sym, guess.text);
        // Activity proposal from the previous/forecast comparison: which side,
        // and whether to act before or after the print (advisory only).
        const trading::EventProposal prop = trading::proposeActivity(e);
        line += QStringLiteral("   ·  %1 %2")
                    .arg(prop.action, prop.actionable ? prop.timing : QString());

        auto *item = new QListWidgetItem(line, m_events);
        item->setForeground(impactColor(guess.dir));  // colour by predicted direction
        const QString baseTip = eventTooltip(e, guess, sym);
        const QString actionEsc = prop.action.toHtmlEscaped();
        const QString timingEsc = prop.timing.toHtmlEscaped();
        const QString rationaleEsc = prop.rationale.toHtmlEscaped();
        item->setToolTip(QStringLiteral("%1<br><b>Proposal:</b> %2 %3.<br><i>%4.</i>")
                             .arg(baseTip, actionEsc, timingEsc, rationaleEsc));
    }
}

void MainWindow::refreshChartEventMarker()
{
    // Mark the chart's event line only while an event is imminent: from 10 minutes
    // before to 5 minutes after it. If several qualify, mark the nearest in time.
    const QDateTime now = QDateTime::currentDateTime();
    const EconomicEvent *active = nullptr;
    qint64 bestAbs = -1;
    for (const EconomicEvent &e : std::as_const(m_eventList)) {
        if (!e.when.isValid()) {
            continue;
        }
        const qint64 secs = now.secsTo(e.when);  // >0 upcoming, <0 already passed
        if ((secs <= (10LL * 60)) && (secs >= (-5LL * 60))) {
            const qint64 a = qAbs(secs);
            if ((bestAbs < 0) || (a < bestAbs)) {
                bestAbs = a;
                active = &e;
            }
        }
    }
    if (active != nullptr) {
        m_chart->setEventMarker(active->when.toMSecsSinceEpoch(), active->title);
    } else {
        m_chart->setEventMarker(0, QString());
    }
}

void MainWindow::onOrderResult(bool ok, const QString &message)
{
    appendLog(message, !ok);
    if (!ok) {
        [[maybe_unused]] const QMessageBox::StandardButton btn =
            QMessageBox::warning(this, QStringLiteral("Order"), message);
    }
}

void MainWindow::onPositionClosed(bool ok, const QString &message)
{
    appendLog(message, !ok);
    if (!ok) {
        [[maybe_unused]] const QMessageBox::StandardButton btn =
            QMessageBox::warning(this, QStringLiteral("Close position"), message);
        return;
    }
    // (Re)arm the delayed closed-trade P/L refresh so the just-closed trade shows up
    // in the summary; restarting collapses a burst of closes into a single fetch.
    m_pnlAfterCloseTimer->start();
}

void MainWindow::refreshClosedTradesForVanished(const QList<Position> &current)
{
    const QStringList justClosed = trading::closedSincePreviousIds(m_shownPositions, current);
    if (justClosed.isEmpty()) {
        return;
    }
    appendLog(QStringLiteral("Open trade%1 %2 no longer open — refreshing closed trades.")
                  .arg(justClosed.size() > 1 ? QStringLiteral("s") : QString(),
                       justClosed.join(QStringLiteral(", "))));
    // Immediately, then again on the delayed timer: the broker's trade history
    // lags its portfolio by seconds, so the first walk can still miss the trade.
    // The throttle protects the history endpoint's small rate pool when several
    // trades close at once (or a close is followed by another poll).
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kMinFetchGapMs = 5000;
    if ((nowMs - m_lastClosedFetchMs) >= kMinFetchGapMs) {
        m_lastClosedFetchMs = nowMs;
        m_client->fetchClosedTrades(closedLookbackWeeks());
    }
    m_pnlAfterCloseTimer->start();
}

void MainWindow::onVix(double level, double changePct)
{
    m_vix = level;
    m_vixChangePct = changePct;  // vs. the multi-month average (see fetchVix)
    m_vixValid = level > 0.0;

    // Colour/label by risk mood: red when fear is high or stretched above its norm,
    // green when calm/below, amber in between.
    const QString mood = (level >= 30.0)
                             ? QStringLiteral("high fear")
                             : ((level >= 20.0)
                                    ? QStringLiteral("elevated")
                                    : ((level >= 15.0) ? QStringLiteral("calm")
                                                       : QStringLiteral("very calm")));
    const QString arrow = (changePct > 2.0)
                              ? QStringLiteral("↑ above avg")
                              : ((changePct < -2.0) ? QStringLiteral("↓ below avg")
                                                    : QStringLiteral("≈ avg"));
    const QString color = ((level >= 25.0) || (changePct > 10.0))
                              ? QStringLiteral("#e35555")
                              : (((level < 16.0) || (changePct < -10.0))
                                     ? QStringLiteral("#25b563")
                                     : QStringLiteral("#e0b000"));
    m_sigVix->setText(QStringLiteral("<span style='color:%1'>%2  (%3, %4, %5%6%)</span>")
                          .arg(color)                    // %1
                          .arg(level, 0, 'f', 2)         // %2
                          .arg(mood, arrow,              // %3, %4
                               (changePct >= 0.0) ? QStringLiteral("+") : QString())  // %5
                          .arg(changePct, 0, 'f', 1));   // %6  (trailing "%)" is literal)
    updateSignals();  // re-fold VIX into the buy/sell ensemble
}

void MainWindow::onExternalSignal(bool available, double score, const QString &rating)
{
    if (!available) {
        m_sigWeb->setText(QStringLiteral(
            "<span style='color:#9a9a9a'>n/a (no TradingView symbol)</span>"));
        return;
    }
    // Colour by the rating bucket, green (buy) → red (sell).
    const QString color =
        (score >= 0.5)
            ? QStringLiteral("#1f9d55")
            : ((score >= 0.1)
                   ? QStringLiteral("#25b563")
                   : ((score > -0.1)
                          ? QStringLiteral("#e0b000")
                          : ((score > -0.5) ? QStringLiteral("#e35555")
                                            : QStringLiteral("#d64545"))));
    m_sigWeb->setText(QStringLiteral("<span style='color:%1'>%2  (%3%4)</span>")
                          .arg(color, rating,
                               (score >= 0.0) ? QStringLiteral("+") : QString())
                          .arg(score, 0, 'f', 2));
}

void MainWindow::onMonthlyPnl(const MonthlyPnl &summary)
{
    const QString ccy = m_ccy;  // figures are USD from the API; shown in euro
    auto colorFor = [](double v) {
        return (v > 0.0) ? QColor(0x25, 0xb5, 0x63)                    // green
                         : ((v < 0.0) ? QColor(0xe3, 0x55, 0x55)       // red
                                      : QColor(0xb0, 0xb0, 0xb0));     // grey
    };
    auto signed2 = [this](double v) {
        const double d = toDisplay(v);
        return QStringLiteral("%1%2").arg((d >= 0.0) ? QStringLiteral("+") : QString(),
                                          QLocale().toString(d, 'f', 2));
    };

    // Fill the per-instrument rows (already sorted by net P/L, descending).
    const auto pnlRows = static_cast<qint32>(summary.perInstrument.size());
    m_pnlTable->setRowCount(pnlRows);
    for (qint32 i = 0; i < pnlRows; ++i) {
        const InstrumentPnl &r = summary.perInstrument[i];
        auto *name = new QTableWidgetItem(r.symbol);
        auto *trades = new QTableWidgetItem(QString::number(r.trades));
        trades->setTextAlignment(Qt::AlignCenter);
        auto *net = new QTableWidgetItem(signed2(r.netProfit));
        net->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        net->setForeground(colorFor(r.netProfit));
        // Costs = estimated open+close spread + reported rollover fees.
        auto *costs = new QTableWidgetItem(
            QLocale().toString(toDisplay(r.fees + r.estSpreadCosts), 'f', 2));
        costs->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        costs->setToolTip(QStringLiteral("open+close spread (est.) %1%2 + rollover fees %1%3")
                              .arg(m_ccy)
                              .arg(toDisplay(r.estSpreadCosts), 0, 'f', 2)
                              .arg(toDisplay(r.fees), 0, 'f', 2));
        m_pnlTable->setItem(i, 0, name);
        m_pnlTable->setItem(i, 1, trades);
        m_pnlTable->setItem(i, 2, net);
        m_pnlTable->setItem(i, 3, costs);
    }

    // Headline summary above the table; the box title names the actual window
    // (the detail dialog can re-fetch with a 7–13 week lookback).
    const QString fromText = summary.fromDate.toString(Qt::ISODate);
    const QString toText = summary.toDate.toString(Qt::ISODate);
    const qint32 weeks = qRound(static_cast<double>(summary.fromDate.daysTo(summary.toDate)) / 7.0);
    m_pnlBox->setTitle(QStringLiteral("Closed trades — last %1 weeks (listed instruments)")
                           .arg(weeks));
    if (summary.accountTrades == 0) {
        m_pnlSummary->setText(QStringLiteral("<b>No closed trades in this window "
                                             "(%1 → %2).</b>")
                                  .arg(fromText, toText));
        return;
    }
    const QString netColor = colorFor(summary.netProfit).name();
    const QString netText = signed2(summary.netProfit);
    // Total costs across the listed instruments: estimated open+close spread
    // plus the rollover fees eToro reports (same figures as the Costs column).
    const double totalCosts =
        std::accumulate(summary.perInstrument.cbegin(), summary.perInstrument.cend(), 0.0,
                        [](double acc, const InstrumentPnl &r) {
                            return acc + r.fees + r.estSpreadCosts;
                        });
    const QString costsText = QLocale().toString(toDisplay(totalCosts), 'f', 2);
    QString html = QStringLiteral(
                       "<b>Net P/L (listed): <span style='color:%1'>%2 %3</span>"
                       " &nbsp;·&nbsp; total costs %4 %3</b>"
                       " &nbsp;·&nbsp; %5 closed trades &nbsp;·&nbsp; %6 → %7")
                       .arg(netColor, netText, ccy, costsText)
                       .arg(summary.trades)
                       .arg(fromText, toText);
    if (summary.perInstrument.isEmpty()) {
        html += QStringLiteral(
            "<br/><span style='color:#9a9a9a'>(no closed trades on listed instruments)</span>");
    }
    m_pnlSummary->setText(html);
}

void MainWindow::onMonthlyPnlFailed(const QString &error)
{
    m_pnlSummary->setText(
        QStringLiteral("<span style='color:#e35555'>%1</span>").arg(error.toHtmlEscaped()));
    if (m_closedSummary != nullptr) {
        m_closedSummary->setText(
            QStringLiteral("<span style='color:#e35555'>%1</span>").arg(error.toHtmlEscaped()));
    }
    appendLog(error, true);
}

void MainWindow::onFearGreed(double score, const QString &rating)
{
    m_fgValid = true;
    m_fg = score;
    m_fgRating = rating;
    if (m_sigCrowd != nullptr) {
        // Colour by the crowd tilt the decision engine derives from the reading
        // (extremes are contrarian), so the row and the composite agree.
        const double tilt = trading::crowdTilt(score);
        const QString col = (tilt > 0.1) ? QStringLiteral("#25b563")
                                         : ((tilt < -0.1) ? QStringLiteral("#e35555")
                                                          : QStringLiteral("#e0b000"));
        m_sigCrowd->setText(QStringLiteral("<span style='color:%1'>%2/100 (%3)</span>")
                                .arg(col)
                                .arg(qRound(score))
                                .arg(rating.toHtmlEscaped()));
    }
}

void MainWindow::onWebQuote(const QString &symbol, double price, const QDateTime &asOf)
{
    m_webQuoteSymbol = symbol;
    m_webQuotePrice = price;
    m_webQuoteTime = asOf;
    if ((m_sigWebQuote == nullptr)
        || (symbol.compare(m_client->config().symbol, Qt::CaseInsensitive) != 0)) {
        return;
    }
    const QString grey = trading::ui::greyHex();
    QString deltaText;
    if (m_lastPrice > 0.0) {
        const double deltaPct = ((price - m_lastPrice) / m_lastPrice) * 100.0;
        deltaText = QStringLiteral(" <span style='color:%1'>(Δ %2%3% vs eToro)</span>")
                        .arg(grey, (deltaPct >= 0.0) ? QStringLiteral("+") : QString())
                        .arg(deltaPct, 0, 'f', 2);
    }
    QString ageText;
    QString col = QStringLiteral("#25b563");
    if (asOf.isValid()) {
        const qint64 ageSecs = asOf.secsTo(QDateTime::currentDateTime());
        col = (ageSecs <= 120) ? QStringLiteral("#25b563")
                               : ((ageSecs <= 900) ? QStringLiteral("#e0b000")
                                                   : QStringLiteral("#e35555"));
        ageText = (ageSecs < 120) ? QStringLiteral(", %1s old").arg(ageSecs)
                                  : QStringLiteral(", %1m old").arg(ageSecs / 60);
    }
    m_sigWebQuote->setText(QStringLiteral("<span style='color:%1'>%2%3</span>%4")
                               .arg(col,
                                    QLocale().toString(price, 'f', trading::priceDecimals(price)),
                                    ageText, deltaText));
}

void MainWindow::proposeSlTpDefaults(double volPct)
{
    if (!m_slTpAuto || (volPct <= 0.0) || (m_stopLoss == nullptr)) {
        return;
    }
    // Never move the fields while the user is editing one of them: a hand edit
    // only disables the automatics once a keystroke produces a VALID value, so
    // a focused, text-selected or just-cleared field would be overwritten by
    // the next poll. Focus can sit on the spin box or its internal QLineEdit,
    // hence the subtree check.
    QWidget *fw = QApplication::focusWidget();
    if ((fw != nullptr)
        && ((fw == m_stopLoss) || (fw == m_takeProfit) || m_stopLoss->isAncestorOf(fw)
            || m_takeProfit->isAncestorOf(fw))) {
        return;
    }
    const double invest = m_amount->value();
    const double lev = m_leverage->currentText().toDouble();
    if ((invest <= 0.0) || (lev <= 0.0)) {
        return;
    }
    // Same geometry as the decision window's plan: stop ≈ 1.5σ of a day's move,
    // take-profit 1.5× the stop (reward:risk 1.5). Capped below the stake so
    // eToro accepts it; rounded to whole euros so the fields read cleanly.
    const double slFrac = trading::proposedSlFraction(volPct, 24);
    const double sl = std::min(std::round(invest * lev * slFrac), std::round(invest * 0.95));
    const double tp = std::round(sl * 1.5);
    if ((std::abs(m_stopLoss->value() - sl) < 0.5)
        && (std::abs(m_takeProfit->value() - tp) < 0.5)) {
        return;  // unchanged — don't re-trigger the valueChanged cascade
    }
    m_settingSlTp = true;
    m_stopLoss->setValue(sl);
    m_takeProfit->setValue(tp);
    m_settingSlTp = false;
}

void MainWindow::checkCloseProposals(double price)
{
    if ((m_closeAdvice == nullptr) || (price <= 0.0)) {
        return;
    }
    const QString sym = m_client->config().symbol;
    constexpr qint64 kLogCooldownMs = 5LL * 60 * 1000;  // one log line per trade per 5 min
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QStringList lines;
    for (const Position &p : std::as_const(m_shownPositions)) {
        // Only the shown instrument has a live price to test against.
        if (p.symbol.compare(sym, Qt::CaseInsensitive) != 0) {
            continue;
        }
        // Trigger 1 — the price left the prediction corridor (3h forecast ±1σ)
        // in the direction that hurts this position.
        QString reason;
        if ((m_forecastTarget > 0.0) && (m_forecastBandPct > 0.0)) {
            const double lower = m_forecastTarget * (1.0 - (m_forecastBandPct / 100.0));
            const double upper = m_forecastTarget * (1.0 + (m_forecastBandPct / 100.0));
            const QString priceText =
                QLocale().toString(price, 'f', trading::priceDecimals(price));
            if (p.isBuy && (price < lower)) {
                const QString lowerText =
                    QLocale().toString(lower, 'f', trading::priceDecimals(lower));
                reason = QStringLiteral("price %1 fell out of the prediction corridor (≥ %2)")
                             .arg(priceText, lowerText);
            } else if (!p.isBuy && (price > upper)) {
                const QString upperText =
                    QLocale().toString(upper, 'f', trading::priceDecimals(upper));
                reason = QStringLiteral("price %1 rose out of the prediction corridor (≤ %2)")
                             .arg(priceText, upperText);
            } else {
                // inside the corridor — check the signal flip below
            }
        }
        // Trigger 2 — the ensemble flipped hard against the position. The
        // direction test is hoisted into a named bool so the decision stays
        // within the 6 conditions clang-18 can instrument for MC/DC.
        const bool signalAgainstPosition =
            p.isBuy ? (m_lastSignalDir < 0) : (m_lastSignalDir > 0);
        if (reason.isEmpty() && (m_lastSignalDir != 0) && (m_lastSignalConf >= 60.0)
            && signalAgainstPosition) {
            reason = QStringLiteral("signal flipped to %1 with %2% confidence")
                         .arg((m_lastSignalDir > 0) ? QStringLiteral("BUY")
                                                    : QStringLiteral("SELL"))
                         .arg(qRound(m_lastSignalConf));
        }
        if (reason.isEmpty()) {
            continue;
        }

        const double plEur = toDisplay(p.profit);
        const QString plColor =
            (plEur >= 0.0) ? QStringLiteral("#25b563") : QStringLiteral("#e35555");
        QString costText;
        if (p.closingCost > 0.0) {
            costText = QStringLiteral(", closing cost ~%1%2")
                           .arg(m_ccy)
                           .arg(toDisplay(p.closingCost), 0, 'f', 2);
        }
        lines << QStringLiteral(
                     "%1 %2%3 (x%4): %5 — P/L <span style='color:%6'>%7%8</span>%9")
                     .arg(p.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"), m_ccy)
                     .arg(toDisplay(p.amount), 0, 'f', 0)
                     .arg(p.leverage, 0, 'f', 0)
                     .arg(reason, plColor,
                          (plEur >= 0.0) ? QStringLiteral("+") : QString())
                     .arg(plEur, 0, 'f', 2)
                     .arg(costText);

        const qint64 last = m_closeAdviceMs.value(p.positionId, 0);
        if ((now - last) >= kLogCooldownMs) {
            static_cast<void>(m_closeAdviceMs.insert(p.positionId, now));
            appendLog(QStringLiteral("CLOSE proposal for %1 %2 position: %3. Tick the trade "
                                     "and press Close marked trades if you agree.")
                          .arg(sym, p.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                               reason));
        }
    }

    if (lines.isEmpty()) {
        m_closeAdvice->setVisible(false);
        return;
    }
    const QString symEsc = sym.toHtmlEscaped();
    const QString joinedLines = lines.join(QStringLiteral("<br/>"));
    m_closeAdvice->setText(
        QStringLiteral("<span style='color:#e0b000'><b>⚠ Close proposal — %1:</b></span><br/>%2")
            .arg(symEsc, joinedLines));
    m_closeAdvice->setVisible(true);
}

// ---------------------------------------------------------------------------
// Closed-trades detail window
// ---------------------------------------------------------------------------

qint32 MainWindow::closedLookbackWeeks() const
{
    if ((m_closedDialog != nullptr) && m_closedDialog->isVisible()) {
        return m_closedWeeks->currentData().toInt();
    }
    return 7;  // dialog closed: the summary panel's default window
}

void MainWindow::openClosedTrades()
{
    if (m_closedDialog == nullptr) {
        m_closedDialog = new QDialog(this);
        m_closedDialog->setWindowTitle(QStringLiteral("Closed trades — details & costs"));
        m_closedDialog->resize(980, 520);
        auto *lay = new QVBoxLayout(m_closedDialog);

        auto *top = new QHBoxLayout;
        top->addWidget(new QLabel(QStringLiteral("Lookback:"), m_closedDialog));
        m_closedWeeks = new QComboBox(m_closedDialog);
        for (qint32 w = 7; w <= 13; ++w) {
            const QString label = QStringLiteral("%1 weeks").arg(w);
            m_closedWeeks->addItem(label, w);
        }
        m_closedWeeks->setCurrentIndex(m_closedWeeks->count() - 1);  // default: 13 weeks
        top->addWidget(m_closedWeeks);
        m_closedRefresh = new QPushButton(QStringLiteral("Refresh"), m_closedDialog);
        top->addWidget(m_closedRefresh);
        // Default ON: the account's history can span hundreds of instruments, while the
        // question this window answers is "how did MY instruments do". Unticking keeps
        // the old behaviour — the whole account, foreign rows greyed as context.
        m_closedListedOnly = new QCheckBox(QStringLiteral("Only this app's instruments"),
                                           m_closedDialog);
        m_closedListedOnly->setChecked(true);
        m_closedListedOnly->setToolTip(QStringLiteral(
            "Show only trades on the instruments in this app's selector. Unticked, every "
            "closed trade of the account is listed, with the foreign ones greyed."));
        static_cast<void>(connect(m_closedListedOnly, &QCheckBox::toggled, this,
                                  [this](bool) { rebuildClosedTradesTable(); }));
        top->addWidget(m_closedListedOnly);
        top->addStretch();
        lay->addLayout(top);

        m_closedSummary = new QLabel(QStringLiteral("Loading closed trades…"), m_closedDialog);
        m_closedSummary->setTextFormat(Qt::RichText);
        m_closedSummary->setWordWrap(true);
        lay->addWidget(m_closedSummary);

        m_closedTable = new QTableWidget(0, 12, m_closedDialog);
        m_closedTable->setHorizontalHeaderLabels(
            {QStringLiteral("Closed"), QStringLiteral("Instrument"), QStringLiteral("Side"),
             QStringLiteral("Lev"), QStringLiteral("Invest (%1)").arg(m_ccy),
             QStringLiteral("Open"), QStringLiteral("Close"),
             QStringLiteral("Net P/L (%1)").arg(m_ccy), QStringLiteral("Fees"),
             QStringLiteral("Open cost*"), QStringLiteral("Close cost*"),
             QStringLiteral("Spread*")});
        m_closedTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_closedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_closedTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_closedTable->verticalHeader()->setVisible(false);
        m_closedTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_closedTable->setToolTip(QStringLiteral(
            "Every closed trade in the window, newest first (grey rows are instruments "
            "not listed in the app's selector).\n"
            "Net P/L and Fees are eToro's own figures (net of spread; fees = rollover).\n"
            "* Open/Close costs are estimates: half the instrument's CURRENT spread × "
            "the trade's notional — eToro does not report historical spreads.\n"
            "* Spread = the spread % the estimate priced with; ⚠ = captured while the "
            "market was closed (frozen quotes widen the spread, inflating the estimate)."));
        lay->addWidget(m_closedTable, 1);

        auto *note = new QLabel(
            QStringLiteral("* estimated from today's spread (half-spread × invest × leverage); "
                           "the actual historical spread is not reported by the API."),
            m_closedDialog);
        note->setStyleSheet(QStringLiteral("color:#9a9a9a"));
        note->setWordWrap(true);
        lay->addWidget(note);

        auto *footer = new QHBoxLayout;
        footer->addStretch();
        auto *closeBtn = new QPushButton(QStringLiteral("Close"), m_closedDialog);
        static_cast<void>(
            connect(closeBtn, &QPushButton::clicked, m_closedDialog, &QDialog::accept));
        footer->addWidget(closeBtn);
        lay->addLayout(footer);

        const auto refresh = [this] {
            m_closedSummary->setText(QStringLiteral("Loading closed trades…"));
            m_client->fetchClosedTrades(m_closedWeeks->currentData().toInt());
        };
        static_cast<void>(connect(m_closedRefresh, &QPushButton::clicked, this, refresh));
        static_cast<void>(
            connect(m_closedWeeks, &QComboBox::currentIndexChanged, this, refresh));
    }

    m_closedDialog->show();
    m_closedDialog->raise();
    m_closedDialog->activateWindow();
    // Fetch for the selected lookback; the reply also refreshes the summary box.
    m_client->fetchClosedTrades(m_closedWeeks->currentData().toInt());
    rebuildClosedTradesTable();  // show whatever was fetched before meanwhile
}

void MainWindow::onClosedTrades(const QList<ClosedTrade> &trades)
{
    m_closedTrades = trades;
    rebuildClosedTradesTable();
}

// The rows the closed-trades window shows: only the app's own instruments while its
// filter is ticked (the default — the account's history can span hundreds of others),
// otherwise every fetched trade, with the foreign ones greyed as context.
QList<ClosedTrade> MainWindow::shownClosedTrades() const
{
    if ((m_closedListedOnly == nullptr) || !m_closedListedOnly->isChecked()) {
        return m_closedTrades;
    }
    QList<ClosedTrade> shown;
    shown.reserve(m_closedTrades.size());
    for (const ClosedTrade &t : std::as_const(m_closedTrades)) {
        if (t.listed) {
            shown << t;
        }
    }
    return shown;
}

void MainWindow::rebuildClosedTradesTable()
{
    if (m_closedTable == nullptr) {
        return;
    }
    double sumNet = 0.0;
    double sumFees = 0.0;
    double sumCosts = 0.0;
    qint32 costRows = 0;

    const QList<ClosedTrade> shown = shownClosedTrades();
    const auto rows = static_cast<qint32>(shown.size());
    m_closedTable->setRowCount(rows);
    for (qint32 i = 0; i < rows; ++i) {
        const ClosedTrade &t = shown[i];
        auto make = [&t](const QString &text) {
            auto *it = new QTableWidgetItem(text);
            if (!t.listed) {
                // The palette constant directly, not the `grey` reference above:
                // naming a reference inside a lambda is an odr-use, so MSVC wants
                // it captured (C3493) while clang calls the capture unnecessary.
                // Using the inline global satisfies both.
                it->setForeground(trading::ui::kGrey);  // not in the selector — context only
            }
            return it;
        };
        auto money = [this](double usd) { return QLocale().toString(toDisplay(usd), 'f', 2); };

        m_closedTable->setItem(i, 0,
            make(t.closeTime.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        m_closedTable->setItem(i, 1, make(t.symbol));
        auto *side = make(t.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
        side->setForeground(t.isBuy ? trading::ui::kGreen : trading::ui::kRed);
        side->setTextAlignment(Qt::AlignCenter);
        m_closedTable->setItem(i, 2, side);
        auto *lev = make(QStringLiteral("x%1").arg(t.leverage, 0, 'f', 0));
        lev->setTextAlignment(Qt::AlignCenter);
        m_closedTable->setItem(i, 3, lev);
        auto *inv = make(money(t.investment));
        inv->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_closedTable->setItem(i, 4, inv);
        m_closedTable->setItem(i, 5,
            make(QLocale().toString(t.openRate, 'f', trading::priceDecimals(t.openRate))));
        m_closedTable->setItem(i, 6,
            make(QLocale().toString(t.closeRate, 'f', trading::priceDecimals(t.closeRate))));
        const double netEur = toDisplay(t.netProfit);
        auto *net = make(QStringLiteral("%1%2")
                             .arg((netEur >= 0.0) ? QStringLiteral("+") : QString(),
                                  QLocale().toString(netEur, 'f', 2)));
        net->setForeground((t.netProfit > 0.0) ? trading::ui::kGreen
                           : ((t.netProfit < 0.0) ? trading::ui::kRed : trading::ui::kGrey));
        net->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_closedTable->setItem(i, 7, net);
        auto *fees = make(money(t.fees));
        fees->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_closedTable->setItem(i, 8, fees);
        auto *oc = make(t.costEstValid ? money(t.openCostEst) : QStringLiteral("—"));
        oc->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_closedTable->setItem(i, 9, oc);
        auto *cc = make(t.costEstValid ? money(t.closeCostEst) : QStringLiteral("—"));
        cc->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_closedTable->setItem(i, 10, cc);
        QString spreadText = QStringLiteral("—");
        if (t.costEstValid && (t.spreadPctUsed > 0.0)) {
            spreadText = QLocale().toString(t.spreadPctUsed, 'f', 3) + QStringLiteral("%")
                         + (t.spreadStale ? QStringLiteral(" ⚠") : QString());
        }
        auto *sp = make(spreadText);
        sp->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (t.spreadStale) {
            sp->setForeground(trading::ui::kAmber);
            sp->setToolTip(QStringLiteral(
                "Priced while the market was closed: frozen quotes carry the widened "
                "after-hours spread, so this estimate overstates the real cost."));
        }
        m_closedTable->setItem(i, 11, sp);

        sumNet += t.netProfit;
        sumFees += t.fees;
        if (t.costEstValid) {
            sumCosts += t.openCostEst + t.closeCostEst;
            ++costRows;
        }
    }

    if (m_closedSummary == nullptr) {
        return;
    }
    if (rows == 0) {
        const bool simulated = !m_client->config().hasCredentials();
        m_closedSummary->setText(
            QStringLiteral("<b>No closed trades in the selected window.</b>%1")
                .arg(simulated ? QStringLiteral(" <span style='color:#9a9a9a'>(simulation "
                                                "mode keeps no per-trade history)</span>")
                               : QString()));
        return;
    }
    const QString netColor = (sumNet >= 0.0) ? QStringLiteral("#25b563")
                                             : QStringLiteral("#e35555");
    const QString netSign = (sumNet >= 0.0) ? QStringLiteral("+") : QString();
    const QString netValue = QLocale().toString(toDisplay(sumNet), 'f', 2);
    const QString netText = QStringLiteral("%1%2 %3").arg(netSign, netValue, m_ccy);
    const QString feesText = QLocale().toString(toDisplay(sumFees), 'f', 2);
    const QString costsText = QLocale().toString(toDisplay(sumCosts), 'f', 2);
    m_closedSummary->setText(
        QStringLiteral("<b>%1 trades · net <span style='color:%2'>%3</span> · "
                       "rollover fees %4%5 · est. spread costs %4%6 (%7 trades priced)</b>")
            .arg(rows)
            .arg(netColor, netText, m_ccy, feesText, costsText)
            .arg(costRows));
}

void MainWindow::onLog(const QString &message, bool isError)
{
    appendLog(message, isError);
}

// ---------------------------------------------------------------------------
// Leverage screener
// ---------------------------------------------------------------------------

void MainWindow::openScript()
{
    if (m_scriptDialog == nullptr) {
        m_scriptDialog = new TradeScriptDialog(m_scriptRunner, this);
    }
    m_scriptDialog->show();
    m_scriptDialog->raise();
    m_scriptDialog->activateWindow();
}

void MainWindow::onLocalModelProposals(const QList<trading::AiProposal> &picks)
{
    m_localPicks = picks;
    // …and WHICH instruments were in front of it. Without that, "no opinion" cannot
    // be told apart from "never shown this one", and the second is not the model
    // being unhelpful — it is the prompt not having room (REQ-F-034).
    m_localAsked = (m_botRunner != nullptr) ? m_botRunner->lastAskedSymbols() : QStringList{};
    pushAiOpinionsToPositions();
    updateLocalModelSignal();
    rebuildDecision();   // the sources table carries the same read per instrument
}

// The local model's read on ONE instrument, or a null proposal when it said nothing
// about it. Its picks are a ranked list, so the first match is its best word on it.
trading::AiProposal MainWindow::localPickFor(const QString &symbol) const
{
    const auto hit = std::find_if(m_localPicks.cbegin(), m_localPicks.cend(),
                                  [&symbol](const trading::AiProposal &p) {
                                      return p.ok
                                             && (p.resolvedSymbol.compare(symbol,
                                                                          Qt::CaseInsensitive)
                                                 == 0);
                                  });
    return (hit != m_localPicks.cend()) ? *hit : trading::AiProposal{};
}

void MainWindow::pushAiOpinionsToPositions()
{
    // The same read the bot uses on its own book, applied to the REAL open trades as
    // ADVICE (REQ-F-034): the hold/close column in the main window. Nothing here
    // closes a real position — that stays a human action behind the double-press
    // gate of REQ-N-005.
    if (m_positionsModel == nullptr) {
        return;
    }
    QHash<QString, trading::HoldVerdict> bySymbol;
    bySymbol.reserve(m_shownPositions.size());
    for (const Position &p : m_shownPositions) {
        if (bySymbol.contains(p.symbol)) {
            continue;
        }
        // Judged exactly as the simulated book's positions are, including the brakes
        // that stop a fresh position being talked out of by a model that changed its
        // mind five minutes in.
        trading::PaperTrade probe;
        probe.symbol = p.symbol;
        probe.isBuy = p.isBuy;
        probe.openTime = p.openTime;
        static_cast<void>(bySymbol.insert(
            p.symbol,
            trading::paperAiHold(probe, m_localPicks, trading::BotAiMode::Lead,
                                 QDateTime::currentDateTime(), trading::BotConfig{})));
    }
    m_positionsModel->setAiOpinions(bySymbol);
}

void MainWindow::updateConfluenceSignal()
{
    if (m_sigConfluence == nullptr) {
        return;
    }
    m_sigConfluence->setTextFormat(Qt::RichText);
    const QString symbol = m_client->config().symbol;
    const trading::IndexReads reads =
        trading::indexReads(symbol, m_referenceSeries, m_intradayBySymbol.value(symbol));
    // Scored for the side the app's own composite currently leans to, so the number
    // answers "does the evidence agree with the call" rather than "is it bullish".
    const qint32 dir = (m_lastSignalDir != 0) ? m_lastSignalDir : 1;
    const trading::Confluence score = trading::confluenceFor(reads, dir);
    if (score.measured() == 0) {
        m_sigConfluence->setText(QStringLiteral("<span style='color:%1'>no reference reads yet"
                                               "</span>")
                                     .arg(trading::ui::greyHex()));
        m_sigConfluence->setToolTip(QStringLiteral("Waiting for the reference series (^VIX, ^VXN, "
                                                  "^TNX and the Nasdaq heavyweights)."));
        return;
    }
    const QString colour = (score.met >= 4) ? QStringLiteral("#25b563")
                                            : ((score.against > score.met)
                                                   ? QStringLiteral("#e35555")
                                                   : QStringLiteral("#e0b000"));
    m_sigConfluence->setText(
        QStringLiteral("<span style='color:%1'>%2 of %3 agree with %4</span>"
                       "<span style='color:%5'> · %6 unmeasured</span>")
            .arg(colour)
            .arg(score.met)
            .arg(score.measured())
            .arg((dir > 0) ? QStringLiteral("BUY") : QStringLiteral("SELL"), trading::ui::greyHex())
            .arg(score.unknown));
    m_sigConfluence->setToolTip(score.reasons.join(u"\n"));
}

void MainWindow::updateLocalModelSignal()
{
    if (m_sigLocalAi == nullptr) {
        return;
    }
    m_sigLocalAi->setTextFormat(Qt::RichText);
    const QString grey = trading::ui::greyHex();
    if ((m_ollama == nullptr) || !m_ollama->isConfigured()) {
        m_sigLocalAi->setText(QStringLiteral("<span style='color:%1'>not configured — set "
                                            "ollamaModel</span>")
                                  .arg(grey));
        return;
    }
    const QString symbol = m_client->config().symbol;
    const trading::AiProposal pick = localPickFor(symbol);
    if (!pick.ok) {
        const bool asked = m_localAsked.contains(symbol, Qt::CaseInsensitive);
        m_sigLocalAi->setText(
            QStringLiteral("<span style='color:%1'>%2</span>")
                .arg(grey,
                     m_localAsked.isEmpty()
                         ? QStringLiteral("waiting for the first answer (%1)")
                               .arg(m_ollama->model())
                         : (asked ? QStringLiteral("looked at it and passed — it named %1 "
                                                   "instead")
                                        .arg(m_localPicks.isEmpty()
                                                 ? QStringLiteral("nothing")
                                                 : m_localPicks.constFirst().resolvedSymbol)
                                  : QStringLiteral("not among the %1 instruments it was shown "
                                                   "this scan (they are ranked by composite)")
                                        .arg(m_localAsked.size()))));
        return;
    }
    const QString word = pick.exitNow
                             ? QStringLiteral("CLOSE")
                             : ((pick.dir > 0) ? QStringLiteral("BUY")
                                               : ((pick.dir < 0) ? QStringLiteral("SELL")
                                                                 : QStringLiteral("HOLD")));
    const QString colour = (pick.dir > 0) ? QStringLiteral("#25b563")
                                          : ((pick.dir < 0) ? QStringLiteral("#e35555")
                                                            : QStringLiteral("#e0b000"));
    m_sigLocalAi->setText(QStringLiteral("<span style='color:%1'>%2 (conf %3)</span>"
                                        "<span style='color:%4'> — %5</span>")
                              .arg(colour, word)
                              .arg(qRound(pick.confidence))
                              .arg(grey, pick.rationale.isEmpty()
                                             ? m_ollama->model()
                                             : pick.rationale.toHtmlEscaped()));
}

void MainWindow::onBotTradeOpened(const QString & /*symbol*/)
{
    // A notice the user has to acknowledge, so an unattended bot cannot open
    // positions unnoticed. Two properties keep it from becoming the opposite of
    // useful: it is NOT modal (a modal box would stop the timers that mark the
    // book and evaluate exits, so the bot would freeze until someone clicked), and
    // only ever ONE is on screen — a single scan can open a dozen trades, and a
    // dozen stacked dialogs is not a notification, it is a lockout.
    if (m_botTradeNotice != nullptr) {
        return;
    }
    auto *box = new QMessageBox(QMessageBox::Information, QStringLiteral("Trading-Bot"),
                                QStringLiteral("Trading-Bot opened a trade"),
                                QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    static_cast<void>(connect(box, &QObject::destroyed, this,
                              [this]() { m_botTradeNotice = nullptr; }));
    m_botTradeNotice = box;
    box->show();
}

void MainWindow::openBotSim()
{
    if (m_botDialog == nullptr) {
        m_botDialog = new BotSimDialog(m_botRunner, this);
    }
    m_botDialog->show();
    m_botDialog->raise();
    m_botDialog->activateWindow();
}

void MainWindow::openScreener()
{
    if (m_screenerDialog == nullptr) {
        m_screenerDialog = new ScreenerDialog(this);
        static_cast<void>(connect(m_screenerDialog, &ScreenerDialog::instrumentChosen, this,
                                  [this](const QString &sym) {
                                      m_autoInstrumentDone = true;  // a manual pick ends the
                                                                    // startup auto-load
                                      selectInstrument(sym);
                                  }));
        static_cast<void>(connect(m_screenerDialog, &ScreenerDialog::rescanRequested, this,
                                  &MainWindow::startScreenerScan));
    }

    // Non-modal so the main window (price, portfolio) keeps updating behind it.
    m_screenerDialog->show();
    m_screenerDialog->raise();
    m_screenerDialog->activateWindow();
    startScreenerScan();
}

void MainWindow::startScreenerScan()
{
    m_screenerRows.clear();
    if (m_screenerDialog != nullptr) {
        m_screenerDialog->scanStarted();
    }
    m_client->scanInstruments();
}

void MainWindow::onScreenerRow(const ScreenerRow &row)
{
    // Replace any existing row for the same symbol (a rescan), else append; then
    // re-rank. The list is small (~26), so rebuilding on each arrival is cheap.
    const auto known = std::find_if(m_screenerRows.begin(), m_screenerRows.end(),
                                    [&row](const ScreenerRow &r) {
                                        return r.symbol == row.symbol;
                                    });
    if (known != m_screenerRows.end()) {
        *known = row;
    } else {
        m_screenerRows.append(row);
    }
    if (m_screenerDialog != nullptr) {  // leverage-screener window, if ever opened
        m_screenerDialog->updateRows(m_screenerRows, m_vixValid, m_vix, m_vixChangePct);
    }
    rebuildRecommendations();     // "buy / sell now" panel
    rebuildDecision();            // decision window (no-op if never opened)
}

void MainWindow::onScreenerProgress(int done, int total)
{
    if (m_screenerDialog != nullptr) {
        m_screenerDialog->scanProgress(done, total);
    }
}

void MainWindow::onScreenerFinished()
{
    if (m_screenerDialog != nullptr) {
        m_screenerDialog->scanFinished(m_screenerRows.size());
    }
    if (m_recoRefresh != nullptr) {
        m_recoRefresh->setEnabled(true);
    }
    if (m_decisionRefresh != nullptr) {
        m_decisionRefresh->setEnabled(true);
    }
    rebuildRecommendations();
    rebuildDecision();

    // The paper-trading bot's decision tick (REQ-F-029): this is the moment the
    // all-instruments data is as complete as it gets, so the composite is computed
    // once here and handed to the runner — which trades it with simulated money.
    // Unconditional on purpose: the bot must keep running with its window closed.
    if (m_botRunner != nullptr) {
        const trading::MarketSnapshot snap = marketSnapshot();
        m_botRunner->onDecisions(trading::computeDecisionRows(snap), snap);
    }

    // If the decision window kicked this scan and Claude is configured, ask it now
    // (once per scan) — the candidate data is as complete as it will get.
    if (m_decisionAiPending && (m_decisionDialog != nullptr) && m_decisionDialog->isVisible()) {
        m_decisionAiPending = false;
        if (m_aiAdvisor->isConfigured()) {
            if (m_decisionAiStatus != nullptr) {
                m_decisionAiStatus->setText(QStringLiteral("Claude (AI): thinking…"));
            }
            const trading::MarketSnapshot snap = marketSnapshot();
            m_aiAdvisor->requestDecision(
                trading::buildDecisionEvidence(trading::computeDecisionRows(snap), snap));
        }
    }
}

void MainWindow::startRecommendationScan()
{
    if (m_recoRefresh != nullptr) {
        m_recoRefresh->setEnabled(false);  // re-enabled on scan finish
    }
    // Reuse the leverage-screener scan (scanInstruments coalesces if one is already
    // running) for the per-instrument ensemble, plus the batched web rating and news.
    m_client->scanInstruments();
    m_feeds->fetchInstrumentRatings();
    m_feeds->fetchInstrumentNews();
    m_feeds->fetchIntradaySeries();
    // …and the reference series that say what the indices are doing: expected
    // volatility, the 10-year yield, and the heavyweights' participation (REQ-F-035).
    m_feeds->fetchReferenceSeries();
}

void MainWindow::onInstrumentRatings(const QHash<QString, WebRating> &ratingBySymbol)
{
    m_ratingBySymbol = ratingBySymbol;
    rebuildRecommendations();
    rebuildDecision();
}

// The three all-instruments feeds: web ratings, news, and the reference series the
// index reads are computed from (REQ-F-035).
void MainWindow::connectInstrumentFeeds()
{
    static_cast<void>(connect(m_feeds, &MarketFeeds::instrumentRatingsUpdated, this,
                              &MainWindow::onInstrumentRatings));
    static_cast<void>(connect(m_feeds, &MarketFeeds::instrumentNewsUpdated, this,
                              &MainWindow::onInstrumentNews));
    static_cast<void>(connect(m_feeds, &MarketFeeds::referenceSeries, this,
                              &MainWindow::onReferenceSeries));
}

void MainWindow::onReferenceSeries(const QString &ticker, const QList<double> &closes)
{
    static_cast<void>(m_referenceSeries.insert(ticker, closes));
    if (m_botRunner != nullptr) {
        m_botRunner->setReferenceSeries(m_referenceSeries);
    }
    // The index reads change what the signals row and the decision sources say, and
    // what the bot is told; none of them are worth a full rebuild per ticker, so the
    // cheap displays refresh and the rest picks it up on the next scan.
    updateConfluenceSignal();
}

void MainWindow::onInstrumentNews(const QString &symbol, const QList<NewsHeadline> &headlines)
{
    static_cast<void>(m_newsBySymbol.insert(symbol, headlines));
    rebuildRecommendations();  // refresh the hover reasoning with the fresh headlines
    rebuildDecision();
    if (symbol == m_client->config().symbol) {
        updateSignals();       // refresh the news-sentiment signal row for the current instrument
    }
}

void MainWindow::rebuildRecommendations()
{
    if ((m_recoBuyList == nullptr) || (m_recoSellList == nullptr)) {
        return;
    }

    const QColor &green = trading::ui::kGreen;
    const QColor &red = trading::ui::kRed;

    auto ago = [](const QDateTime &t) -> QString {
        if (!t.isValid()) {
            return {};
        }
        const qint64 s = t.secsTo(QDateTime::currentDateTime());
        if (s < 60) {
            return QStringLiteral("just now");
        }
        if (s < 3600) {
            return QStringLiteral("%1m ago").arg(s / 60);
        }
        if (s < 86400) {
            return QStringLiteral("%1h ago").arg(s / 3600);
        }
        return QStringLiteral("%1d ago").arg(s / 86400);
    };

    struct Reco {
        QString symbol;
        qint32 dir = 0;          // +1 BUY / -1 SELL
        double confidence = 0.0;
        QString row;             // the list line
        QString tip;             // hover reasoning
    };
    QList<Reco> recos;

    // The panel renders the SAME ranked calls as the decision window: one
    // weighted multi-source composite (trading::computeDecisionRows) instead of
    // a second, hand-tuned blend that could disagree with it on screen. The UI
    // adds presentation only — the row text, the tooltip and the market-open
    // filter; the ensemble lines in the tooltip are recomputed for display and
    // carry no decision weight. Rows arrive sorted by confidence descending.
    const QList<trading::DecisionRow> rows = trading::computeDecisionRows(marketSnapshot());
    for (const trading::DecisionRow &d : rows) {
        if (d.dir == 0) {
            continue;  // nothing actionable for this instrument right now
        }
        // "Buy / sell now" is actionable advice, so drop instruments whose market is
        // closed right now — you couldn't trade them anyway. (Unknown state = show all,
        // so the panel isn't emptied before the first tradeability check lands.)
        if (m_tradeabilityKnown && !m_tradeableNow.contains(d.symbol)) {
            continue;
        }

        const QString side = (d.dir > 0) ? QStringLiteral("BUY") : QStringLiteral("SELL");
        Reco reco;
        reco.symbol = d.symbol;
        reco.dir = d.dir;
        reco.confidence = d.confidence;
        // Side is conveyed by the column and colour, so the row is just symbol + confidence.
        reco.row = QStringLiteral("%1   ·   %2%").arg(d.symbol).arg(qRound(d.confidence));

        QStringList tip;
        tip << QStringLiteral("%1 — %2 (confidence %3%)")
                   .arg(d.symbol, side)
                   .arg(qRound(d.confidence));
        tip << QString();
        const auto sr = std::find_if(m_screenerRows.cbegin(), m_screenerRows.cend(),
                                     [&d](const ScreenerRow &r) { return r.symbol == d.symbol; });
        const trading::Ensemble e =
            ((sr != m_screenerRows.cend()) && sr->ok && !sr->closes.isEmpty())
                ? trading::computeEnsemble(sr->closes, m_vixValid, m_vixChangePct)
                : trading::Ensemble{};
        if (e.valid) {
            const QString bull = (e.dir > 0) ? QStringLiteral("bullish")
                                             : ((e.dir < 0) ? QStringLiteral("bearish")
                                                            : QStringLiteral("mixed"));
            const QString trend = (e.dir > 0) ? QStringLiteral("▲ up")
                                              : ((e.dir < 0) ? QStringLiteral("▼ down")
                                                             : QStringLiteral("→ flat"));
            tip << QStringLiteral("Technical ensemble: %1 — net %2/%3 indicators %4")
                       .arg(e.signal)
                       .arg(std::abs(e.score))
                       .arg(e.votes)
                       .arg(bull);
            tip << QStringLiteral("Trend %1 · volatility ±%2%/bar").arg(trend).arg(e.vol, 0, 'f', 2);
        } else {
            tip << QStringLiteral("Technical ensemble: gathering data…");
        }
        if (d.haveRating) {
            tip << QStringLiteral("TradingView 1h rating: %1 (%2%3) — %4")
                       .arg(trading::webRatingWord(d.rating),
                            (d.rating >= 0.0) ? QStringLiteral("+") : QString())
                       .arg(d.rating, 0, 'f', 2)
                       .arg(((d.rating > 0) == (d.dir > 0)) ? QStringLiteral("confirms")
                                                            : QStringLiteral("disagrees"));
        } else {
            tip << QStringLiteral("TradingView rating: n/a for this instrument");
        }
        const QList<NewsHeadline> news = m_newsBySymbol.value(d.symbol);
        if (!news.isEmpty()) {
            tip << QString() << QStringLiteral("Recent news:");
            for (qsizetype i = 0; (i < news.size()) && (i < 3); ++i) {
                const NewsHeadline &h = news[i];
                QString meta = h.provider;
                const QString a = ago(h.published);
                if (!a.isEmpty()) {
                    meta += meta.isEmpty() ? a : QStringLiteral(", %1").arg(a);
                }
                tip << (meta.isEmpty() ? QStringLiteral("• %1").arg(h.title)
                                       : QStringLiteral("• %1  (%2)").arg(h.title, meta));
            }
        }
        reco.tip = tip.join(QLatin1Char('\n'));
        recos.append(reco);
    }

    m_recoBuyList->clear();
    m_recoSellList->clear();
    for (const Reco &reco : recos) {
        QListWidget *target = (reco.dir > 0) ? m_recoBuyList : m_recoSellList;
        auto *item = new QListWidgetItem(reco.row, target);
        item->setForeground((reco.dir > 0) ? green : red);
        item->setData(Qt::UserRole, reco.symbol);
        item->setToolTip(reco.tip);
    }
    // Per-column placeholder when a side has nothing to show (or the scan is still running).
    const bool scanning = m_screenerRows.isEmpty();
    if (m_recoBuyList->count() == 0) {
        m_recoBuyList->addItem(scanning ? QStringLiteral("Scanning…")
                                        : QStringLiteral("No buy signals now"));
    }
    if (m_recoSellList->count() == 0) {
        m_recoSellList->addItem(scanning ? QStringLiteral("Scanning…")
                                         : QStringLiteral("No sell signals now"));
    }
}

// ---------------------------------------------------------------------------
// Decision window
// ---------------------------------------------------------------------------

namespace {
// One source of truth for how a direction renders (dir > 0 long, dir < 0 short,
// 0 flat): the ranked decision table and the focus panel show it identically.
QColor callColour(qint32 dir)
{
    return (dir > 0) ? trading::ui::kGreen : ((dir < 0) ? trading::ui::kRed : trading::ui::kGrey);
}

QString callWord(qint32 dir)
{
    return (dir > 0) ? QStringLiteral("BUY")
                     : ((dir < 0) ? QStringLiteral("SELL") : QStringLiteral("—"));
}

// The decision row for one symbol, or nullptr. The three renderers below all
// need it, and rows arrive confidence-sorted, so the first match is the ranked
// one. (Pointer into the caller's list — valid while that list lives.)
const trading::DecisionRow *rowForSymbol(const QList<trading::DecisionRow> &rows,
                                         const QString &symbol)
{
    const auto it = std::find_if(rows.cbegin(), rows.cend(),
                                 [&symbol](const trading::DecisionRow &d) {
                                     return d.symbol == symbol;
                                 });
    return (it == rows.cend()) ? nullptr : &*it;
}

// The highest-ranked row that actually calls a direction (dir != 0).
const trading::DecisionRow *firstDirectionalRow(const QList<trading::DecisionRow> &rows)
{
    const auto it = std::find_if(rows.cbegin(), rows.cend(),
                                 [](const trading::DecisionRow &d) { return d.dir != 0; });
    return (it == rows.cend()) ? nullptr : &*it;
}

// The screener row carrying the close series for one symbol, or nullptr.
// (ScreenerRow lives in the global namespace — domain/Models.h is not namespaced.)
const ScreenerRow *screenerRowFor(const QList<ScreenerRow> &rows, const QString &symbol)
{
    const auto it = std::find_if(rows.cbegin(), rows.cend(),
                                 [&symbol](const ScreenerRow &r) { return r.symbol == symbol; });
    return (it == rows.cend()) ? nullptr : &*it;
}

// One row of the decision window's per-source table — exactly the four cells
// renderDecisionFocus writes, plus the read cell's colour.
struct SourceRowSpec {
    QString name;    // column 0: source label
    QString read;    // column 1: the source's directional read
    QColor colour;   // foreground of the read cell (invalid = theme default)
    QString conf;    // column 2: confidence / score figure
    QString note;    // column 3: fine print (basis, counts, warnings)
};

// The per-source spec builders below keep everything unique to one source
// (its dir thresholds, wording, availability gating) in one place each; the
// render loop in renderDecisionFocus is the same for all of them.

SourceRowSpec techSourceRow(const trading::DecisionRow &focus)
{
    const QString conf =
        focus.haveTech ? QStringLiteral("%1%").arg(qRound(focus.techConf)) : QString();
    return {QStringLiteral("Technical ensemble"),
            focus.haveTech ? focus.techLabel : QStringLiteral("n/a"),
            callColour(focus.techDir), conf, QStringLiteral("indicator blend")};
}

SourceRowSpec ratingSourceRow(const trading::DecisionRow &focus)
{
    const qint32 dir = (focus.rating > 0) ? 1 : ((focus.rating < 0) ? -1 : 0);
    return {QStringLiteral("TradingView rating"),
            focus.haveRating ? trading::webRatingWord(focus.rating) : QStringLiteral("n/a"),
            focus.haveRating ? callColour(dir) : trading::ui::kGrey,
            focus.haveRating ? QStringLiteral("%1").arg(focus.rating, 0, 'f', 2) : QString(),
            QStringLiteral("15m / 1h / 1D consensus")};
}

SourceRowSpec newsSourceRow(const trading::DecisionRow &focus)
{
    const qint32 dir = (focus.newsScore > 0.1) ? 1 : ((focus.newsScore < -0.1) ? -1 : 0);
    const QString read =
        focus.haveNews ? ((dir > 0) ? QStringLiteral("positive")
                                    : ((dir < 0) ? QStringLiteral("negative")
                                                 : QStringLiteral("neutral")))
                       : QStringLiteral("n/a");
    return {QStringLiteral("News sentiment"), read,
            focus.haveNews ? callColour(dir) : trading::ui::kGrey,
            focus.haveNews ? QStringLiteral("%1").arg(focus.newsScore, 0, 'f', 2) : QString(),
            QStringLiteral("%1 headlines").arg(focus.newsCount)};
}

SourceRowSpec regimeSourceRow(const trading::DecisionRow &focus, bool vixValid, double vix)
{
    const qint32 dir = (focus.regime > 0.05) ? 1 : ((focus.regime < -0.05) ? -1 : 0);
    const QString read =
        vixValid ? ((vix >= 25.0) ? QStringLiteral("risk-off")
                                  : ((vix < 16.0) ? QStringLiteral("risk-on")
                                                  : QStringLiteral("neutral")))
                 : QStringLiteral("n/a");
    return {QStringLiteral("VIX / calendar regime"), read, callColour(dir),
            vixValid ? QStringLiteral("VIX %1").arg(vix, 0, 'f', 1) : QString(),
            focus.eventRisk ? QStringLiteral("⚠ high-impact event <6h") : QStringLiteral("—")};
}

// Crowd sentiment: what the trading crowd is doing right now (CNN F&G).
SourceRowSpec crowdSourceRow(const trading::DecisionRow &focus, double fg, const QString &fgRating)
{
    const qint32 dir = (focus.crowd > 0.1) ? 1 : ((focus.crowd < -0.1) ? -1 : 0);
    return {QStringLiteral("Crowd (Fear & Greed)"),
            focus.haveCrowd ? QStringLiteral("%1/100 %2").arg(qRound(fg)).arg(fgRating)
                            : QStringLiteral("n/a"),
            focus.haveCrowd ? callColour(dir) : trading::ui::kGrey,
            focus.haveCrowd ? QStringLiteral("%1").arg(focus.crowd, 0, 'f', 2) : QString(),
            QStringLiteral("extremes read contrarian")};
}

// Independent Yahoo Finance intraday momentum (1-minute session bars).
SourceRowSpec yahooSourceRow(const trading::DecisionRow &focus)
{
    const qint32 dir = (focus.yahoo > 0.1) ? 1 : ((focus.yahoo < -0.1) ? -1 : 0);
    const QString read =
        focus.haveYahoo ? ((dir > 0) ? QStringLiteral("above session mean")
                                     : ((dir < 0) ? QStringLiteral("below session mean")
                                                  : QStringLiteral("at session mean")))
                        : QStringLiteral("n/a");
    return {QStringLiteral("Yahoo intraday"), read,
            focus.haveYahoo ? callColour(dir) : trading::ui::kGrey,
            focus.haveYahoo ? QStringLiteral("%1").arg(focus.yahoo, 0, 'f', 2) : QString(),
            QStringLiteral("1-min closes, yahoo finance")};
}

// Claude (AI) row — always its own pick, for reference.
SourceRowSpec aiSourceRow(const AiDecision &ai, bool configured)
{
    const bool actionable =
        ai.ok && (ai.action.compare(QStringLiteral("HOLD"), Qt::CaseInsensitive) != 0);
    const bool buy = ai.action.compare(QStringLiteral("BUY"), Qt::CaseInsensitive) == 0;
    return {QStringLiteral("Claude (AI)"),
            ai.ok ? QStringLiteral("%1 %2").arg(ai.action, ai.symbol) : QStringLiteral("n/a"),
            actionable ? callColour(buy ? 1 : -1) : trading::ui::kGrey,
            ai.ok ? QStringLiteral("%1%").arg(qRound(ai.confidence)) : QString(),
            configured ? (ai.ok ? QStringLiteral("synthesised") : QStringLiteral("pending / n/a"))
                       : QStringLiteral("set anthropicApiKey to enable")};
}

// The local model's row: the same shape as the cloud advisor's, but per instrument
// and with "no opinion" told apart from "not configured" — a model that answered
// about other instruments has said something about this one by omission, and that
// is not the same as never having been asked.
SourceRowSpec localAiSourceRow(const trading::AiProposal &pick, bool configured,
                               const QString &model, bool wasShown, qsizetype shownCount)
{
    if (!configured) {
        return {QStringLiteral("Local model (Ollama)"), QStringLiteral("n/a"),
                trading::ui::kGrey, QString(), QStringLiteral("set ollamaModel to enable")};
    }
    if (!pick.ok) {
        // Two different silences, and conflating them is what makes the feature look
        // broken: it can only speak about instruments the prompt listed.
        const QString note =
            (shownCount == 0)
                ? QStringLiteral("waiting for the first answer (%1)").arg(model)
                : (wasShown ? QStringLiteral("shown this instrument and did not pick it")
                            : QStringLiteral("not among the %1 instruments shown this scan")
                                  .arg(shownCount));
        return {QStringLiteral("Local model (Ollama)"), QStringLiteral("—"), trading::ui::kGrey,
                QString(), note};
    }
    const bool actionable = (pick.dir != 0) && !pick.exitNow;
    const QString word = pick.exitNow
                             ? QStringLiteral("CLOSE")
                             : ((pick.dir > 0) ? QStringLiteral("BUY")
                                               : ((pick.dir < 0) ? QStringLiteral("SELL")
                                                                 : QStringLiteral("HOLD")));
    return {QStringLiteral("Local model (Ollama)"), word,
            actionable ? callColour(pick.dir) : trading::ui::kGrey,
            QStringLiteral("%1%").arg(qRound(pick.confidence)),
            pick.rationale.isEmpty() ? model : pick.rationale};
}

// While no focus row exists yet, the six source rows render as neutral
// placeholders ("scanning…" while the screener has produced nothing at all).
QList<SourceRowSpec> placeholderSourceRows(bool scanning)
{
    static const QStringList names = {
        QStringLiteral("Technical ensemble"), QStringLiteral("TradingView rating"),
        QStringLiteral("News sentiment"),     QStringLiteral("VIX / calendar regime"),
        QStringLiteral("Crowd (Fear & Greed)"), QStringLiteral("Yahoo intraday")};
    QList<SourceRowSpec> specs;
    specs.reserve(names.size());
    for (const QString &name : names) {
        specs.append({name, QStringLiteral("—"), trading::ui::kGrey, QString(),
                      scanning ? QStringLiteral("scanning…") : QString()});
    }
    return specs;
}

}  // namespace

// Capture the latest market data as the plain snapshot the decision engine
// (domain layer) reasons over: the UI gathers, the engine decides.
trading::MarketSnapshot MainWindow::marketSnapshot() const
{
    trading::MarketSnapshot m;
    m.screenerRows = m_screenerRows;
    m.ratingBySymbol = m_ratingBySymbol;
    m.newsBySymbol = m_newsBySymbol;
    m.referenceSeries = m_referenceSeries;
    m.vixValid = m_vixValid;
    m.vix = m_vix;
    m.vixChangePct = m_vixChangePct;
    m.events = m_eventList;
    m.fgValid = m_fgValid;
    m.fearGreed = m_fg;
    m.intradayBySymbol = m_intradayBySymbol;
    return m;
}

void MainWindow::startDecisionScan()
{
    if (m_decisionRefresh != nullptr) {
        m_decisionRefresh->setEnabled(false);  // re-enabled on scan finish
    }
    m_decisionAiPending = true;                // ask Claude when this scan completes
    m_client->scanInstruments();
    m_feeds->fetchInstrumentRatings();
    m_feeds->fetchInstrumentNews();
    m_feeds->fetchIntradaySeries();
    // …and the reference series that say what the indices are doing: expected
    // volatility, the 10-year yield, and the heavyweights' participation (REQ-F-035).
    m_feeds->fetchReferenceSeries();
}

void MainWindow::openDecision()
{
    if (m_decisionDialog == nullptr) {
        m_decisionDialog = new QDialog(this);
        m_decisionDialog->setWindowTitle(QStringLiteral("Trade decision — sources & AI"));
        m_decisionDialog->resize(760, 620);
        auto *lay = new QVBoxLayout(m_decisionDialog);

        auto *intro = new QLabel(
            QStringLiteral("Independent sources are combined into a weighted call per instrument; "
                           "the strongest is the recommendation. With an Anthropic API key set, "
                           "Claude reviews the same evidence and its verdict is shown too."),
            m_decisionDialog);
        intro->setWordWrap(true);
        lay->addWidget(intro);

        m_decisionConclusion = new QLabel(m_decisionDialog);
        m_decisionConclusion->setTextFormat(Qt::RichText);
        m_decisionConclusion->setWordWrap(true);
        m_decisionConclusion->setContentsMargins(4, 6, 4, 6);
        lay->addWidget(m_decisionConclusion);

        // The costed trade plan for the focus instrument: verdict with win
        // probability and risk factor, recommended leverage, SL/TP levels, and
        // the full cost bill (spread both ways, overnight, weekend) netted
        // against the expected gain. Apply pushes it into the trade panel —
        // it never places an order (BUY/SELL keep their double-press gate).
        m_decisionPlanLabel = new QLabel(m_decisionDialog);
        m_decisionPlanLabel->setTextFormat(Qt::RichText);
        m_decisionPlanLabel->setWordWrap(true);
        m_decisionPlanLabel->setContentsMargins(4, 2, 4, 2);
        lay->addWidget(m_decisionPlanLabel);
        m_decisionApply = new QPushButton(
            QStringLiteral("Apply plan to trade panel (no order placed)"), m_decisionDialog);
        m_decisionApply->setEnabled(false);
        m_decisionApply->setToolTip(QStringLiteral(
            "Selects the plan's instrument and fills Amount, Leverage, Stop loss and "
            "Take profit in the trade panel. No order is placed — you still confirm "
            "with the double-press on BUY or SELL."));
        static_cast<void>(connect(m_decisionApply, &QPushButton::clicked, this,
                                  &MainWindow::applyDecisionPlan));
        lay->addWidget(m_decisionApply);

        m_decisionSourcesLabel =
            new QLabel(QStringLiteral("Sources for the recommended instrument:"), m_decisionDialog);
        lay->addWidget(m_decisionSourcesLabel);
        m_decisionSources = new QTableWidget(0, 4, m_decisionDialog);
        m_decisionSources->setHorizontalHeaderLabels(
            {QStringLiteral("Source"), QStringLiteral("Read"), QStringLiteral("Confidence"),
             QStringLiteral("Note")});
        m_decisionSources->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_decisionSources->setSelectionMode(QAbstractItemView::NoSelection);
        m_decisionSources->verticalHeader()->setVisible(false);
        m_decisionSources->horizontalHeader()->setStretchLastSection(true);
        // Eight source rows now (the six market reads plus BOTH advisors), and they
        // are the point of this window: room to read them without scrolling.
        m_decisionSources->setMinimumHeight(270);
        lay->addWidget(m_decisionSources);

        lay->addWidget(new QLabel(QStringLiteral("All instruments, ranked by composite:"),
                                  m_decisionDialog));
        m_decisionRanked = new QTableWidget(0, RankedColCount, m_decisionDialog);
        m_decisionRanked->setHorizontalHeaderLabels(
            {QStringLiteral("Instrument"), QStringLiteral("Call"), QStringLiteral("Composite"),
             QStringLiteral("Confidence"), QStringLiteral("Web signal"),
             QStringLiteral("Open"), QStringLiteral("Trade plan")});
        m_decisionRanked->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_decisionRanked->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_decisionRanked->setSelectionMode(QAbstractItemView::SingleSelection);
        m_decisionRanked->verticalHeader()->setVisible(false);
        m_decisionRanked->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        // Single-click a row → the sources table + headline above follow that instrument.
        static_cast<void>(
            connect(m_decisionRanked, &QTableWidget::itemSelectionChanged, this, [this] {
                if (m_decisionUpdatingRanked) {
                    return;  // programmatic refill, not a user pick
                }
                const auto sel = m_decisionRanked->selectedItems();
                if (sel.isEmpty()) {
                    return;
                }
                const QString sym = m_decisionRanked->item(sel.first()->row(),
                                                           RankedColInstrument)
                                        ->data(Qt::UserRole).toString();
                if (sym.isEmpty() || (sym == m_decisionSelected)) {
                    return;
                }
                m_decisionSelected = sym;
                renderDecisionFocus(trading::computeDecisionRows(marketSnapshot()),
                                    m_decisionSelected);
            }));
        // Double-click → switch the whole app to that instrument.
        static_cast<void>(connect(
            m_decisionRanked, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
                QTableWidgetItem *it = m_decisionRanked->item(row, RankedColInstrument);
                if (it == nullptr) {
                    return;
                }
                const QString sym = it->data(Qt::UserRole).toString();
                if (sym.isEmpty()) {
                    return;
                }
                m_autoInstrumentDone = true;
                selectInstrument(sym);
            }));
        lay->addWidget(m_decisionRanked, 1);

        m_decisionAiStatus = new QLabel(m_decisionDialog);
        m_decisionAiStatus->setWordWrap(true);
        lay->addWidget(m_decisionAiStatus);

        auto *footer = new QHBoxLayout;
        m_decisionRefresh = new QPushButton(QStringLiteral("Refresh"), m_decisionDialog);
        static_cast<void>(connect(m_decisionRefresh, &QPushButton::clicked, this,
                                  &MainWindow::startDecisionScan));
        auto *closeBtn = new QPushButton(QStringLiteral("Close"), m_decisionDialog);
        static_cast<void>(
            connect(closeBtn, &QPushButton::clicked, m_decisionDialog, &QDialog::accept));
        footer->addStretch();
        footer->addWidget(m_decisionRefresh);
        footer->addWidget(closeBtn);
        lay->addLayout(footer);
    }

    m_decisionSelected.clear();  // default to the recommendation each time it's opened
    m_decisionDialog->show();
    m_decisionDialog->raise();
    m_decisionDialog->activateWindow();
    startDecisionScan();  // kick a fresh scan (and the AI request once it finishes)
    rebuildDecision();    // render whatever data is already loaded
}

void MainWindow::onAiDecision(const AiDecision &decision)
{
    m_aiDecision = decision;
    m_scriptRunner->setSignalState(
        m_lastSignalDir, aiDecisionDir(decision), m_aiAdvisor->isConfigured());
    if (m_decisionAiStatus != nullptr) {
        if (decision.ok) {
            m_decisionAiStatus->setText(
                QStringLiteral("Claude (AI): %1 %2 — %3")
                    .arg(decision.action, decision.symbol, decision.rationale));
        } else {
            m_decisionAiStatus->setText(QStringLiteral("Claude (AI): %1").arg(decision.error));
        }
    }
    rebuildDecision();
}

void MainWindow::rebuildDecision()
{
    if ((m_decisionSources == nullptr) || (m_decisionRanked == nullptr)
        || (m_decisionConclusion == nullptr)) {
        return;
    }

    const QList<trading::DecisionRow> rows = trading::computeDecisionRows(marketSnapshot());

    // Fill the ranked table (all instruments), guarded so the programmatic refill isn't
    // mistaken for a user selection.
    m_decisionUpdatingRanked = true;
    const auto rowCount = static_cast<qint32>(rows.size());
    m_decisionRanked->setRowCount(rowCount);
    for (qint32 i = 0; i < rowCount; ++i) {
        const trading::DecisionRow &d = rows[i];
        auto *sym = new QTableWidgetItem(d.symbol);
        sym->setData(Qt::UserRole, d.symbol);
        auto *call = new QTableWidgetItem(callWord(d.dir));
        call->setForeground(callColour(d.dir));
        call->setTextAlignment(Qt::AlignCenter);
        // Mouse-over: what the call means and which source reads produced it.
        QStringList why;
        if (d.haveTech) {
            why << QStringLiteral("technical ensemble %1 (%2%)")
                       .arg(d.techLabel).arg(qRound(d.techConf));
        }
        if (d.haveRating) {
            why << QStringLiteral("TradingView rating %1").arg(d.rating, 0, 'f', 2);
        }
        if (d.haveNews) {
            why << QStringLiteral("news sentiment %1 (%2 headlines)")
                       .arg(d.newsScore, 0, 'f', 2).arg(d.newsCount);
        }
        why << QStringLiteral("market regime %1").arg(d.regime, 0, 'f', 2);
        if (d.haveCrowd) {
            why << QStringLiteral("crowd tilt %1").arg(d.crowd, 0, 'f', 2);
        }
        if (d.haveYahoo) {
            why << QStringLiteral("Yahoo intraday momentum %1").arg(d.yahoo, 0, 'f', 2);
        }
        const QString callMeaning =
            (d.dir > 0) ? QStringLiteral("BUY: the weighted blend of the sources points up.")
            : ((d.dir < 0) ? QStringLiteral("SELL: the weighted blend of the sources points down.")
                           : QStringLiteral("No call: the sources cancel out — no directional "
                                            "edge either way."));
        call->setToolTip(QStringLiteral("%1\nComposite %2 from: %3.%4")
                             .arg(callMeaning)
                             .arg(d.composite, 0, 'f', 2)
                             .arg(why.join(QStringLiteral("; ")),
                                  d.eventRisk ? QStringLiteral(
                                      "\n⚠ Confidence trimmed: high-impact event within 6h.")
                                              : QString()));
        auto *comp = new QTableWidgetItem(QStringLiteral("%1").arg(d.composite, 0, 'f', 2));
        comp->setTextAlignment(Qt::AlignCenter);
        auto *conf = new QTableWidgetItem(QStringLiteral("%1%").arg(qRound(d.confidence)));
        conf->setTextAlignment(Qt::AlignCenter);
        // "Open": live tradeability from the quote-freshness poll (the API has no
        // session flag), the same state that locks the BUY/SELL buttons.
        auto *openIt = new QTableWidgetItem();
        openIt->setTextAlignment(Qt::AlignCenter);
        if (!m_tradeabilityKnown) {
            openIt->setText(QStringLiteral("…"));
            openIt->setForeground(trading::ui::kGrey);
            openIt->setToolTip(QStringLiteral("Waiting for the first tradeability poll."));
        } else if (m_tradeableNow.contains(d.symbol)) {
            openIt->setText(QStringLiteral("● open"));
            openIt->setForeground(trading::ui::kGreen);
            openIt->setToolTip(QStringLiteral("Live quotes are fresh — the market is open "
                                              "for trading right now."));
        } else {
            openIt->setText(QStringLiteral("○ closed"));
            openIt->setForeground(trading::ui::kGrey);
            openIt->setToolTip(QStringLiteral("Quotes are stale — the market is closed; "
                                              "BUY/SELL is locked for this instrument."));
        }
        // "Trade plan": the plan verdict for this row. Costed asynchronously in one
        // batch after the table fills; until then show the last known verdict.
        const auto planIt = m_rowPlans.constFind(d.symbol);
        QTableWidgetItem *planItem = (planIt != m_rowPlans.constEnd())
                                         ? makePlanVerdictItem(*planIt)
                                         : makePendingPlanItem();
        m_decisionRanked->setItem(i, RankedColInstrument, sym);
        m_decisionRanked->setItem(i, RankedColCall, call);
        m_decisionRanked->setItem(i, RankedColComposite, comp);
        m_decisionRanked->setItem(i, RankedColConfidence, conf);
        // "Web signal": the TradingView rating this row fed into the composite.
        m_decisionRanked->setItem(i, RankedColWeb,
                                  makeWebRatingItem(m_ratingBySymbol.value(d.symbol)));
        m_decisionRanked->setItem(i, RankedColOpen, openIt);
        m_decisionRanked->setItem(i, RankedColPlan, planItem);
    }
    dispatchRowPlans(rows);  // fill the "Trade plan" column off the GUI thread

    // Resolve the focus instrument: the user's manual pick if it's still listed, else
    // the recommendation (Claude's actionable pick, else the top composite).
    const bool aiActionable = m_aiDecision.ok
                              && (m_aiDecision.action.compare(QStringLiteral("HOLD"),
                                                              Qt::CaseInsensitive) != 0);
    const trading::DecisionRow *topRow = firstDirectionalRow(rows);
    QString focus;
    if (!m_decisionSelected.isEmpty()
        && (rowForSymbol(rows, m_decisionSelected) != nullptr)) {
        focus = m_decisionSelected;
    } else if (aiActionable && (rowForSymbol(rows, m_aiDecision.symbol) != nullptr)) {
        focus = m_aiDecision.symbol;
    } else if (topRow != nullptr) {
        focus = topRow->symbol;
    } else {
        // nothing actionable yet — focus stays empty and the header shows HOLD
    }

    // Keep the ranked-list highlight in sync with the focus.
    for (qint32 i = 0; i < rowCount; ++i) {
        if (rows[i].symbol == focus) {
            m_decisionRanked->selectRow(i);
            break;
        }
    }
    m_decisionUpdatingRanked = false;

    renderDecisionFocus(rows, focus);
}

void MainWindow::renderDecisionFocus(const QList<trading::DecisionRow> &rows,
                                     const QString &focusSymbol)
{
    if ((m_decisionSources == nullptr) || (m_decisionConclusion == nullptr)) {
        return;
    }

    const trading::DecisionRow *focus = rowForSymbol(rows, focusSymbol);
    const bool manual = !m_decisionSelected.isEmpty() && (focusSymbol == m_decisionSelected);

    if (m_decisionSourcesLabel != nullptr) {
        m_decisionSourcesLabel->setText(
            (focus != nullptr)
                ? QStringLiteral("Sources for %1%2:")
                      .arg(focusSymbol, manual ? QStringLiteral(" (selected)") : QString())
                : QStringLiteral("Sources:"));
    }

    // One spec per table row, in display order; the last row is always Claude's.
    QList<SourceRowSpec> specs =
        (focus != nullptr)
            ? QList<SourceRowSpec>{techSourceRow(*focus), ratingSourceRow(*focus),
                                   newsSourceRow(*focus),
                                   regimeSourceRow(*focus, m_vixValid, m_vix),
                                   crowdSourceRow(*focus, m_fg, m_fgRating),
                                   yahooSourceRow(*focus)}
            : placeholderSourceRows(m_screenerRows.isEmpty());
    specs.append(aiSourceRow(m_aiDecision, m_aiAdvisor->isConfigured()));
    // The local model, per instrument: it is asked about all of them, so unlike the
    // cloud advisor it can speak about whichever row has focus (REQ-F-034).
    specs.append(localAiSourceRow(localPickFor(focusSymbol),
                                  (m_ollama != nullptr) && m_ollama->isConfigured(),
                                  (m_ollama != nullptr) ? m_ollama->model() : QString(),
                                  m_localAsked.contains(focusSymbol, Qt::CaseInsensitive),
                                  m_localAsked.size()));

    auto make = [](const QString &t) {
        auto *it = new QTableWidgetItem(t);
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        return it;
    };
    m_decisionSources->setRowCount(static_cast<qint32>(specs.size()));
    for (qint32 row = 0; row < static_cast<qint32>(specs.size()); ++row) {
        const SourceRowSpec &s = specs.at(row);
        auto *read = make(s.read);
        if (s.colour.isValid()) {
            read->setForeground(s.colour);
        }
        m_decisionSources->setItem(row, 0, make(s.name));
        m_decisionSources->setItem(row, 1, read);
        m_decisionSources->setItem(row, 2, make(s.conf));
        m_decisionSources->setItem(row, 3, make(s.note));
    }

    // Build the costed plan first: the headline's suggested leverage below quotes
    // the plan's volatility-targeted recommendation, so the two never disagree.
    renderTradePlan(focus, focusSymbol);

    renderDecisionConclusion(rows, focus, focusSymbol, manual);
}

// The conclusion's suggested leverage, clamped to the instrument max.
qint32 MainWindow::suggestedFocusLeverage(const trading::DecisionRow &focus,
                                          const QString &focusSymbol, bool aiActionable) const
{
    const qint32 maxLev = focus.maxLev;
    // The plan's volatility-targeted recommendation is the suggested leverage;
    // Claude's figure applies only when the focus IS Claude's own pick, and the
    // legacy instrument-max cap is the last resort while the plan has no data.
    qint32 lev = std::min((maxLev > 0) ? maxLev : 20, 20);
    if (m_lastPlan.valid && (m_lastPlanSymbol == focusSymbol)) {
        lev = m_lastPlan.leverage;
    } else if (aiActionable && (m_aiDecision.symbol == focusSymbol)
               && (m_aiDecision.leverage > 0)) {
        lev = m_aiDecision.leverage;
    }
    if (maxLev > 0) {
        lev = std::min(lev, maxLev);
    }
    return lev;
}

// The "algorithmic top / Claude" context line under the headline; empty when
// neither exists (then appending it to the conclusion html is a no-op).
QString MainWindow::decisionBasisHtml(const QList<trading::DecisionRow> &rows) const
{
    // Overall recommendation basis, for context.
    const trading::DecisionRow *topRow = firstDirectionalRow(rows);
    QStringList basis;
    if (topRow != nullptr) {
        basis << QStringLiteral("algorithmic top: %1 %2 (%3%)")
                     .arg(callWord(topRow->dir), topRow->symbol)
                     .arg(qRound(topRow->confidence));
    }
    if (m_aiDecision.ok) {
        basis << QStringLiteral("Claude: %1 %2 (%3%)")
                     .arg(m_aiDecision.action, m_aiDecision.symbol)
                     .arg(qRound(m_aiDecision.confidence));
    }
    if (basis.isEmpty()) {
        return {};
    }
    return QStringLiteral("<div style='color:#777'>%1</div>")
        .arg(basis.join(QStringLiteral(" · ")));
}

// --- headline conclusion (follows the focus instrument) ---
void MainWindow::renderDecisionConclusion(const QList<trading::DecisionRow> &rows,
                                          const trading::DecisionRow *focus,
                                          const QString &focusSymbol, bool manual)
{
    QString html;
    if (focus == nullptr) {
        html = m_screenerRows.isEmpty()
                   ? QStringLiteral("<b>Scanning instruments…</b>")
                   : QStringLiteral("<b>No actionable trade right now — HOLD.</b>");
    } else {
        const bool aiActionable = m_aiDecision.ok
                                  && (m_aiDecision.action.compare(QStringLiteral("HOLD"),
                                                                  Qt::CaseInsensitive) != 0);
        const qint32 dir = focus->dir;
        const qint32 lev = suggestedFocusLeverage(*focus, focusSymbol, aiActionable);
        const double budget = std::max(0.0, kMaxOpenExposure - m_openTradesTotal);
        double amount =
            std::min(budget, (m_availableCash > 0.0) ? (m_availableCash * 0.10) : 2500.0);
        if (amount <= 0.0) {
            amount = std::min(budget, 2500.0);
        }
        const QColor c = callColour(dir);
        const QString lead = manual ? QStringLiteral("Selected") : QStringLiteral("Now");
        html = QStringLiteral(
                   "<div style='font-size:15px'><b>%1: <span style='color:%2'>%3 %4</span></b> "
                   "&nbsp;·&nbsp; %5% confidence · suggested x%6 leverage, ~%7%8</div>")
                   .arg(lead, c.name(),
                        (dir > 0) ? QStringLiteral("BUY")
                                  : ((dir < 0) ? QStringLiteral("SELL")
                                               : QStringLiteral("HOLD")),
                        focusSymbol)
                   .arg(qRound(focus->confidence))
                   .arg(lev)
                   .arg(m_ccy)
                   .arg(qRound(toDisplay(amount)));
        html += decisionBasisHtml(rows);
        // Claude's rationale only when the focus IS Claude's own pick.
        if (aiActionable && (m_aiDecision.symbol == focusSymbol)
            && !m_aiDecision.rationale.isEmpty()) {
            html += QStringLiteral("<div style='margin-top:4px'>%1</div>")
                        .arg(m_aiDecision.rationale.toHtmlEscaped());
        }
        if (focus->eventRisk) {
            html += QStringLiteral(
                "<div style='color:#e0b000;margin-top:4px'>⚠ High-impact event within 6h — expect "
                "volatility.</div>");
        }
    }
    m_decisionConclusion->setText(html);
}

// Cost a plan for EVERY ranked instrument in one background pass, so the
// "Trade plan" column mirrors the panel's verdict per row. The Monte-Carlo work
// stays off the GUI thread (REQ-N-006); all inputs are snapshotted here.
void MainWindow::dispatchRowPlans(const QList<trading::DecisionRow> &rows)
{
    QList<QPair<QString, trading::PlanInput>> inputs;
    inputs.reserve(rows.size());
    for (const trading::DecisionRow &d : rows) {
        const ScreenerRow *row = screenerRowFor(m_screenerRows, d.symbol);
        if ((row == nullptr) || row->closes.isEmpty()) {
            continue;
        }
        trading::PlanInput in;
        in.closes = row->closes;
        in.price = row->lastPrice;
        in.dir = d.dir;
        in.invest = m_amount->value();
        in.maxLeverage = d.maxLev;
        // Cached spread/fees only — a cache miss keeps the plan's costsComplete
        // false rather than firing one fetch per row (the focus plan refreshes them).
        in.spreadPct = m_client->spreadPctFor(d.symbol);
        const InstrumentFees fees = m_client->feesFor(d.symbol);
        if (fees.isValid()) {
            in.fees = fees;
            in.feesKnown = true;
        }
        in.horizonHours = 24;
        in.now = QDateTime::currentDateTime();
        in.vixValid = m_vixValid;
        in.vix = m_vix;
        in.vixChangePct = m_vixChangePct;
        in.eventRisk = d.eventRisk;
        in.fgValid = m_fgValid;
        in.fearGreed = m_fg;
        inputs.append(qMakePair(d.symbol, in));
    }
    if (inputs.isEmpty()) {
        return;
    }
    m_rowPlanWatcher.setFuture(QtConcurrent::run([inputs]() {
        QHash<QString, trading::TradePlan> plans;
        for (const auto &entry : inputs) {
            static_cast<void>(
                plans.insert(entry.first, trading::buildTradePlan(entry.second)));
        }
        return plans;
    }));
}

// Batch result arrived: cache it and refresh the "Trade plan" cells in place.
void MainWindow::applyRowPlanVerdicts()
{
    m_rowPlans = m_rowPlanWatcher.result();
    // The focus instrument keeps its panel plan: it was built from fresh candles
    // and live spread, the batch row from the screener snapshot and cached costs.
    // The cell and the panel must show the same verdict (REQ-F-021).
    if (!m_lastPlanSymbol.isEmpty() && m_lastPlan.valid) {
        static_cast<void>(m_rowPlans.insert(m_lastPlanSymbol, m_lastPlan));
    }
    if (m_decisionRanked == nullptr) {
        return;
    }
    for (qint32 i = 0; i < m_decisionRanked->rowCount(); ++i) {
        const QTableWidgetItem *symItem = m_decisionRanked->item(i, RankedColInstrument);
        if (symItem == nullptr) {
            continue;
        }
        const auto it = m_rowPlans.constFind(symItem->text());
        if (it != m_rowPlans.constEnd()) {
            m_decisionRanked->setItem(i, RankedColPlan, makePlanVerdictItem(*it));
        }
    }
}

// A market opened/closed: repaint the ranked table's "Open" cells without a rebuild.
void MainWindow::updateDecisionOpenColumn()
{
    if ((m_decisionRanked == nullptr) || !m_tradeabilityKnown) {
        return;
    }
    for (qint32 i = 0; i < m_decisionRanked->rowCount(); ++i) {
        const QTableWidgetItem *symItem = m_decisionRanked->item(i, RankedColInstrument);
        QTableWidgetItem *openIt = m_decisionRanked->item(i, RankedColOpen);
        if ((symItem == nullptr) || (openIt == nullptr)) {
            continue;
        }
        const bool open = m_tradeableNow.contains(symItem->text());
        openIt->setText(open ? QStringLiteral("● open") : QStringLiteral("○ closed"));
        openIt->setForeground(open ? trading::ui::kGreen : trading::ui::kGrey);
        openIt->setToolTip(open ? QStringLiteral("Live quotes are fresh — the market is "
                                                 "open for trading right now.")
                                : QStringLiteral("Quotes are stale — the market is closed; "
                                                 "BUY/SELL is locked for this instrument."));
    }
}

void MainWindow::renderTradePlan(const trading::DecisionRow *focus, const QString &focusSymbol)
{
    if (m_decisionPlanLabel == nullptr) {
        return;
    }
    m_lastPlan = trading::TradePlan{};
    m_lastPlanSymbol.clear();
    if (m_decisionApply != nullptr) {
        m_decisionApply->setEnabled(false);
    }
    if (focus == nullptr) {
        m_decisionPlanLabel->setText(QString());
        return;
    }
    // The close series behind the focus row (the same one its ensemble used).
    const ScreenerRow *row = screenerRowFor(m_screenerRows, focusSymbol);
    if ((row == nullptr) || row->closes.isEmpty()) {
        m_decisionPlanLabel->setText(QString());
        return;
    }

    const bool isCurrent =
        focusSymbol.compare(m_client->config().symbol, Qt::CaseInsensitive) == 0;

    trading::PlanInput in;
    in.closes = row->closes;
    in.price = (isCurrent && (m_lastPrice > 0.0)) ? m_lastPrice : row->lastPrice;
    in.dir = focus->dir;
    in.invest = m_amount->value();
    in.maxLeverage = focus->maxLev;
    if (isCurrent) {
        // The trade panel's leverage list is the instrument's real eligibility set.
        QList<qint32> steps;
        for (qint32 i = 0; i < m_leverage->count(); ++i) {
            steps << m_leverage->itemText(i).toInt();
        }
        in.leverageSteps = steps;
        const double bid = m_client->lastBid();
        const double ask = m_client->lastAsk();
        if ((bid > 0.0) && (ask > bid)) {
            in.spreadPct = ((ask - bid) / ((ask + bid) / 2.0)) * 100.0;
        }
        in.fees = m_fees;
        in.feesKnown = m_fees.isValid();
    }
    // Any listed instrument: the client keeps a per-instrument spread cache warm
    // (bulk-rates tradeability poll) and serves cached rollover fees; a cache
    // miss triggers a one-off fetch and the plan re-renders on arrival.
    if (in.spreadPct <= 0.0) {
        in.spreadPct = m_client->spreadPctFor(focusSymbol);
    }
    if (!in.feesKnown) {
        const InstrumentFees fees = m_client->feesFor(focusSymbol);
        if (fees.isValid()) {
            in.fees = fees;
            in.feesKnown = true;
        } else {
            m_client->requestFees(focusSymbol);
        }
    }
    in.horizonHours = 24;
    in.now = QDateTime::currentDateTime();
    in.vixValid = m_vixValid;
    in.vix = m_vix;
    in.vixChangePct = m_vixChangePct;
    in.eventRisk = focus->eventRisk;
    in.fgValid = m_fgValid;
    in.fearGreed = m_fg;

    // buildTradePlan runs its own Monte-Carlo — dispatch it to the thread pool
    // so a scan or row click never stalls the GUI. The watcher tracks the
    // newest request only, so a superseded plan is dropped automatically.
    m_planPendingSymbol = focusSymbol;
    m_planPendingIsCurrent = isCurrent;
    m_planWatcher.setFuture(QtConcurrent::run(trading::buildTradePlan, in));
}

void MainWindow::renderTradePlanResult(const trading::TradePlan &plan,
                                       const QString &focusSymbol, bool isCurrent)
{
    if (m_decisionPlanLabel == nullptr) {
        return;
    }
    if (!plan.valid) {
        m_decisionPlanLabel->setText(QString());
        return;
    }
    m_lastPlan = plan;
    m_lastPlanSymbol = focusSymbol;

    // The ranked table's "Trade plan" cell for this instrument must show the
    // SAME verdict as this panel: adopt the fresh focus plan — it may have been
    // built from newer closes/spread than the batch row plan (REQ-F-021).
    static_cast<void>(m_rowPlans.insert(focusSymbol, plan));
    if (m_decisionRanked != nullptr) {
        for (qint32 i = 0; i < m_decisionRanked->rowCount(); ++i) {
            const QTableWidgetItem *symItem = m_decisionRanked->item(i, RankedColInstrument);
            if ((symItem != nullptr) && (symItem->text() == focusSymbol)) {
                m_decisionRanked->setItem(i, RankedColPlan, makePlanVerdictItem(plan));
                break;
            }
        }
    }

    const QString green = trading::ui::greenHex();
    const QString red = trading::ui::redHex();
    const QString amber = trading::ui::amberHex();
    const QString grey = trading::ui::greyHex();
    const QString vColor = (plan.verdict == QStringLiteral("BUY"))
                               ? green
                               : ((plan.verdict == QStringLiteral("SELL")) ? red : amber);
    auto eur = [this](double v) {
        return QStringLiteral("%1%2").arg(m_ccy, QLocale().toString(v, 'f', 2));
    };
    auto rate = [](double v) {
        return QLocale().toString(v, 'f', trading::priceDecimals(v));
    };

    QString html = QStringLiteral("<hr/><div><b>Trade plan — %1 (24h horizon):</b> "
                                  "<span style='color:%2;font-size:14px'><b>%3</b></span>")
                       .arg(focusSymbol.toHtmlEscaped(), vColor, plan.verdict);
    if (!plan.verdictReason.isEmpty()) {
        html += QStringLiteral(" <span style='color:%1'>(%2)</span>")
                    .arg(grey, plan.verdictReason.toHtmlEscaped());
    }
    html += QStringLiteral("</div>");

    // Probability & risk line: the three Monte-Carlo outcomes plus the risk factor.
    const QString riskColor =
        (plan.riskFactor >= 4) ? red : ((plan.riskFactor >= 3) ? amber : green);
    const qint32 pExpire =
        std::max(0, 100 - qRound(plan.pWin * 100.0) - qRound(plan.pLose * 100.0));
    html += QStringLiteral(
                "<div>P(take-profit first) <b>%1%</b> · P(stop first) <b>%2%</b> · "
                "expires between <b>%3%</b> <span style='color:%4'>(break-even %5% of "
                "decided paths)</span> · risk factor "
                "<span style='color:%6'><b>%7/5</b></span>%8</div>")
                .arg(qRound(plan.pWin * 100.0))
                .arg(qRound(plan.pLose * 100.0))
                .arg(pExpire)
                .arg(grey)
                .arg(qRound(plan.breakeven * 100.0))
                .arg(riskColor)
                .arg(plan.riskFactor)
                .arg(plan.riskNotes.isEmpty()
                         ? QString()
                         : QStringLiteral(" <span style='color:%1'>— %2</span>")
                               .arg(grey, plan.riskNotes.join(QStringLiteral("; "))
                                              .toHtmlEscaped()));

    // Sizing line: recommended leverage + the proposed SL/TP for this stake.
    const QString slAmountText = eur(plan.slAmount);
    const QString slRateText = rate(plan.slRate);
    const QString tpAmountText = eur(plan.tpAmount);
    const QString tpRateText = rate(plan.tpRate);
    html += QStringLiteral(
                "<div>Stake %1 · recommended leverage <b>x%2</b> "
                "<span style='color:%3'>(±%4% of stake per hour)</span> · "
                "SL <b>%5</b> @ %6 · TP <b>%7</b> @ %8</div>")
                .arg(eur(m_amount->value()))
                .arg(plan.leverage)
                .arg(grey)
                .arg(plan.marginSwingPct, 0, 'f', 1)
                .arg(slAmountText, slRateText, tpAmountText, tpRateText);

    // Cost bill: spread both ways, overnight, weekend — netted against the edge.
    const QString openCostText = eur(plan.openCost);
    const QString closeCostText = eur(plan.closeCost);
    QString costLine =
        QStringLiteral("open %1 + close %2").arg(openCostText, closeCostText);
    if (plan.feePerNight != 0.0) {
        costLine += QStringLiteral(" + %1/night").arg(eur(plan.feePerNight));
    }
    if (plan.crossesWeekend) {
        costLine += QStringLiteral(" + weekend %1").arg(eur(plan.weekendFee));
    }
    const QString netColor = (plan.expectedNet > 0.0) ? green : red;
    const QString nightsPlural = (plan.nights == 1) ? QString() : QStringLiteral("s");
    const QString costsTotal = eur(plan.expectedCosts);
    const QString partialNote =
        plan.costsComplete ? QString()
                           : QStringLiteral(" <span style='color:%1'>(partial — live "
                                            "spread/fees not received yet)</span>")
                                 .arg(amber);
    const QString netSign = (plan.expectedNet >= 0.0) ? QStringLiteral("+") : QString();
    const QString netText = QLocale().toString(plan.expectedNet, 'f', 2);
    html += QStringLiteral(
                "<div>Costs (%1 night%2): %3 = <b>%4</b>%5 · expected edge after costs: "
                "<span style='color:%6'><b>%7%8</b></span></div>")
                .arg(plan.nights)
                .arg(nightsPlural, costLine, costsTotal, partialNote, netColor, netSign, netText);
    if (plan.crossesWeekend) {
        html += QStringLiteral(
                    "<div style='color:%1'>⚠ Holding over the weekend: the rollover night "
                    "charges ~3× the overnight fee and Monday can gap past the stop.</div>")
                    .arg(amber);
    }
    // Data freshness: how the eToro rate compares to the independent web quote.
    if (isCurrent && (m_webQuotePrice > 0.0)
        && (m_webQuoteSymbol.compare(focusSymbol, Qt::CaseInsensitive) == 0)
        && (m_lastPrice > 0.0)) {
        const double deltaPct = ((m_webQuotePrice - m_lastPrice) / m_lastPrice) * 100.0;
        html += QStringLiteral(
                    "<div style='color:%1'>Reference quote (Yahoo): %2, Δ %3%4% vs the "
                    "eToro rate.</div>")
                    .arg(grey, rate(m_webQuotePrice),
                         (deltaPct >= 0.0) ? QStringLiteral("+") : QString())
                    .arg(deltaPct, 0, 'f', 2);
    }

    m_decisionPlanLabel->setText(html);
    // Mouse-over: what this verdict means, why it was reached, and what
    // "expected edge after costs" is — the whole plan block explains itself.
    QString verdictMeaning;
    if (plan.verdict == QStringLiteral("BUY")) {
        verdictMeaning = QStringLiteral(
            "BUY — open a long position: the sources point up and the expected "
            "win, after all costs, is positive.");
    } else if (plan.verdict == QStringLiteral("SELL")) {
        verdictMeaning = QStringLiteral(
            "SELL — open a short position: the sources point down and the expected "
            "win, after all costs, is positive.");
    } else {
        verdictMeaning = QStringLiteral(
            "STAY OUT — do not open a position on this instrument now.");
    }
    if (!plan.verdictReason.isEmpty()) {
        verdictMeaning += QStringLiteral("\nWhy: %1.").arg(plan.verdictReason);
    }
    if (!plan.riskNotes.isEmpty()) {
        verdictMeaning += QStringLiteral("\nRisk notes: %1.")
                              .arg(plan.riskNotes.join(QStringLiteral("; ")));
    }
    m_decisionPlanLabel->setToolTip(QStringLiteral(
        "%1\n\n"
        "Expected edge after costs = P(take-profit first) × TP amount − "
        "P(stop first) × SL amount − the full cost bill (half the spread on "
        "opening + half on closing + rollover per night + the ~3× weekend "
        "rollover when the horizon crosses it).\n"
        "Here: %2 × %3%4 − %5 × %3%6 − %3%7 = %3%8. Positive = the trade is "
        "worth taking after costs; negative = the costs eat the edge.")
        .arg(verdictMeaning)
        .arg(plan.pWin, 0, 'f', 2)
        .arg(m_ccy)
        .arg(plan.tpAmount, 0, 'f', 0)
        .arg(plan.pLose, 0, 'f', 2)
        .arg(plan.slAmount, 0, 'f', 0)
        .arg(plan.expectedCosts, 0, 'f', 2)
        .arg(plan.expectedNet, 0, 'f', 2));
    if (m_decisionApply != nullptr) {
        m_decisionApply->setEnabled(plan.dir != 0);
    }
}

void MainWindow::applyDecisionPlan()
{
    if (!m_lastPlan.valid || m_lastPlanSymbol.isEmpty()) {
        return;
    }
    const trading::TradePlan plan = m_lastPlan;  // survive the re-renders below
    const QString sym = m_lastPlanSymbol;
    if (sym.compare(m_client->config().symbol, Qt::CaseInsensitive) != 0) {
        m_autoInstrumentDone = true;
        selectInstrument(sym);
    }
    // Pick the plan's leverage in the combo (exact match, else the largest below it).
    qint32 bestIdx = -1;
    qint32 bestVal = 0;
    for (qint32 i = 0; i < m_leverage->count(); ++i) {
        const qint32 v = m_leverage->itemText(i).toInt();
        if ((v <= plan.leverage) && (v > bestVal)) {
            bestVal = v;
            bestIdx = i;
        }
    }
    if (bestIdx >= 0) {
        m_leverage->setCurrentIndex(bestIdx);
    }
    // The applied SL/TP are the user's explicit choice now — stop auto-updating them.
    m_settingSlTp = true;
    m_stopLoss->setValue(std::round(plan.slAmount));
    m_takeProfit->setValue(std::round(plan.tpAmount));
    m_settingSlTp = false;
    m_slTpAuto = false;
    appendLog(QStringLiteral(
                  "Plan applied for %1: %2, x%3, SL %4%5 / TP %4%6. No order placed — "
                  "double-press BUY or SELL to execute.")
                  .arg(sym, plan.verdict)
                  .arg(plan.leverage)
                  .arg(m_ccy)
                  .arg(std::round(plan.slAmount), 0, 'f', 0)
                  .arg(std::round(plan.tpAmount), 0, 'f', 0));
}

void MainWindow::updateTradeHours(const QString &symbol)
{
    if (m_tradeHours == nullptr) {
        return;
    }

    // eToro's CFD windows differ from the underlying cash sessions and are published
    // in GMT (etoro.com/trading/market-hours-and-events), shown here in the user's
    // local time. Instruments verified against that page carry eToro's actual hours;
    // the rest fall back to a clearly-labelled exchange-session approximation.
    static const QSet<QString> alwaysOn = {
        QStringLiteral("SP.24-7"), QStringLiteral("NSDQ100.24-7"), QStringLiteral("Gold.24-7"),
        QStringLiteral("OIL.24-7"), QStringLiteral("Crypto10")};
    static const QSet<QString> forex = {QStringLiteral("EURUSD"), QStringLiteral("USDOLLAR")};
    // US & global index CFDs eToro trades almost around the clock (Sun 22:00 → Fri
    // 20:30 GMT, daily ~21:00–22:00 GMT break) — NOT the 09:30–16:00 cash session.
    static const QSet<QString> nearContinuous = {
        QStringLiteral("SPX500"), QStringLiteral("NSDQ100"), QStringLiteral("DJ30"),
        QStringLiteral("RTY"), QStringLiteral("Semiconductors"), QStringLiteral("AI.Leaders"),
        QStringLiteral("Cybersecurity"), QStringLiteral("Quantum"), QStringLiteral("GoldMiners"),
        QStringLiteral("Nuclear")};

    // A GMT wall-clock time (today) rendered in the user's local HH:mm.
    auto localOf = [](qint32 h, qint32 m) {
        const QDate today = QDate::currentDate();
        const QTime gmtTime(h, m);
        const QTimeZone utc = QTimeZone::utc();
        return QDateTime(today, gmtTime, utc).toLocalTime().time().toString(
            QStringLiteral("HH:mm"));
    };
    const QString ltz = QDateTime::currentDateTime().timeZoneAbbreviation();

    if (alwaysOn.contains(symbol)) {
        m_tradeHours->setText(QStringLiteral("Trading hours: 24/7"));
        return;
    }
    if (forex.contains(symbol)) {
        m_tradeHours->setText(QStringLiteral("Trading hours (eToro): Sun–Fri, ~24h"));
        return;
    }
    if (nearContinuous.contains(symbol)) {
        const QString breakStart = localOf(21, 0);
        const QString breakEnd = localOf(22, 0);
        m_tradeHours->setText(
            QStringLiteral("Trading hours (eToro): Sun–Fri, nearly 24h · daily break %1–%2 %3")
                .arg(breakStart, breakEnd, ltz));
        return;
    }
    if (symbol == QLatin1String("HKG50")) {
        const QString hkOpen = localOf(1, 15);
        const QString hkClose = localOf(19, 0);
        m_tradeHours->setText(
            QStringLiteral("Trading hours (eToro): Mon–Fri %1–%2 %3 · intraday breaks apply")
                .arg(hkOpen, hkClose, ltz));
        return;
    }

    // Regional index CFDs not yet verified against eToro's page: show the underlying
    // exchange session as an approximation (eToro's tradeable window may be wider).
    struct Session {
        const char *tz;
        qint32 oh;
        qint32 om;
        qint32 ch;
        qint32 cm;
        const char *days;
    };
    static const QHash<QString, Session> sessions = {
        {QStringLiteral("GER40"),          {"Europe/Berlin", 9, 0, 17, 30, "Mon–Fri"}},
        {QStringLiteral("EUSTX50"),        {"Europe/Berlin", 9, 0, 17, 30, "Mon–Fri"}},
        {QStringLiteral("Switzerland20"),  {"Europe/Zurich", 9, 0, 17, 30, "Mon–Fri"}},
        {QStringLiteral("Sweden30"),       {"Europe/Stockholm", 9, 0, 17, 30, "Mon–Fri"}},
        {QStringLiteral("CHINA50"),        {"Asia/Shanghai", 9, 30, 15, 0, "Mon–Fri"}},
        {QStringLiteral("Canada60"),       {"America/Toronto", 9, 30, 16, 0, "Mon–Fri"}},
        {QStringLiteral("Colombia"),       {"America/Bogota", 9, 30, 15, 55, "Mon–Fri"}},
        {QStringLiteral("RUBBER"),         {"Asia/Tokyo", 9, 0, 15, 15, "Mon–Fri"}},
    };

    const auto it = sessions.find(symbol);
    if (it == sessions.end()) {
        m_tradeHours->setText(QStringLiteral("Trading hours (approx.): Mon–Fri, market hours"));
        return;
    }
    const Session &s = *it;
    const QTimeZone tz(QByteArray(s.tz));
    const QDate d = QDate::currentDate();
    const QString open =
        QDateTime(d, QTime(s.oh, s.om), tz).toLocalTime().time().toString(QStringLiteral("HH:mm"));
    const QString close =
        QDateTime(d, QTime(s.ch, s.cm), tz).toLocalTime().time().toString(QStringLiteral("HH:mm"));
    m_tradeHours->setText(QStringLiteral("Trading hours (approx.): %1 %2–%3 %4")
                              .arg(QString::fromUtf8(s.days), open, close, ltz));
}

// ---------------------------------------------------------------------------
// User actions
// ---------------------------------------------------------------------------

void MainWindow::selectInstrument(const QString &sym)
{
    if (sym.isEmpty()) {
        return;
    }
    // Reflect the choice in the selector without re-triggering textActivated.
    const qint32 idx = m_instrumentBox->findText(sym);
    if ((idx >= 0) && (idx != m_instrumentBox->currentIndex())) {
        const QSignalBlocker block(m_instrumentBox);
        m_instrumentBox->setCurrentIndex(idx);
    }
    m_limitRateDefaultsSet = false;  // reseed the limit-order rates for the new instrument
    m_fees = InstrumentFees{};  // the old instrument's rollover fees no longer apply
    m_slTpAuto = true;          // volatility-proposed SL/TP resume for the new instrument
    m_forecastTarget = 0.0;     // old corridor no longer applies (watchdog stands down)
    m_lastSignalDir = 0;
    m_webQuotePrice = 0.0;      // old instrument's reference quote no longer applies
    // A closed-market override is vouched for ONE market: never let it carry over to the
    // next instrument, whose session may genuinely be shut (REQ-F-026).
    if ((m_marketClosedOverride != nullptr) && m_marketClosedOverride->isChecked()) {
        m_marketClosedOverride->setChecked(false);  // toggled → logs + refreshes the buttons
    }
    if (m_sigWebQuote != nullptr) {
        m_sigWebQuote->setText(QStringLiteral("…"));
    }
    updateOpenCost();
    // Immediate feedback on the header and the chart (window + in-graph title);
    // onReady refreshes them again once the instrument resolves.
    m_titleLabel->setText(sym);
    m_chart->setTitle(QStringLiteral("%1 price").arg(sym));
    m_chart->setWindowTitle(QStringLiteral("%1 — Price / time + change").arg(sym));
    appendLog(QStringLiteral("Switching to %1…").arg(sym));
    // Lock the trade panel until the new instrument actually resolves, so BUY/SELL
    // can never fire against the previously resolved instrument. onReady() clears
    // the lock (synchronously in simulation, after the search reply in real mode).
    const bool switching =
        (sym.compare(m_client->config().symbol, Qt::CaseInsensitive) != 0)
        || !m_client->instrument().isValid();
    if (switching) {
        m_instrumentResolving = true;
        m_tradeBox->setTitle(QStringLiteral("Trade %1 (resolving…)").arg(sym));
    }
    m_client->setSymbol(sym);
    m_feeds->setCurrentSymbol(sym);  // re-fetch the web rating promptly for the new instrument
    updateTradeButtonsEnabled();  // immediate BUY/SELL state from the cached tradeability
}

void MainWindow::onBuyClicked()
{
    handleOrderButton(true);
}

void MainWindow::onSellClicked()
{
    handleOrderButton(false);
}

void MainWindow::handleOrderButton(bool isBuy, bool limit)
{
    // Require a deliberate double-press: a single click only arms the button and
    // prompts; the order fires only if the SAME button is pressed again in time.
    // Market and limit buttons are distinct gates — pressing "BUY" then "Place limit
    // BUY" must not add up to a confirmation of either (REQ-N-005).
    constexpr qint64 kDoublePressMs = 650;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool sameButton = (isBuy == m_orderClickBuy) && (limit == m_orderClickLimit);
    if ((m_orderClickMs != 0) && sameButton && ((now - m_orderClickMs) <= kDoublePressMs)) {
        m_orderClickMs = 0;  // consumed — the next order needs two fresh presses
        if (limit) {
            placeLimitOrder(isBuy);
        } else {
            placeOrder(isBuy);
        }
    } else {
        m_orderClickMs = now;
        m_orderClickBuy = isBuy;
        m_orderClickLimit = limit;
        const double amountEur = m_amount->value();
        // Show the euro order and the USD amount eToro will actually receive, so the
        // conversion is visible before the confirming second press.
        const QString sizes = (m_eurPerUsd > 0.0)
                                  ? QStringLiteral("%1%2 (≈ $%3)")
                                        .arg(m_ccy)
                                        .arg(amountEur, 0, 'f', 2)
                                        .arg(fromDisplay(amountEur), 0, 'f', 2)
                                  : QStringLiteral("%1%2").arg(m_ccy).arg(amountEur, 0, 'f', 2);
        const QString side = isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");
        appendLog(limit
                      ? QStringLiteral("Press \"Place limit %1\" again within 650 ms to rest %2 "
                                       "at eToro.").arg(side, sizes)
                      : QStringLiteral("Press %1 again within 650 ms to place %2.")
                            .arg(side, sizes));
    }
}

void MainWindow::placeLimitOrder(bool isBuy)
{
    // The rate comes from the side's own field; everything else (size, leverage, SL/TP,
    // trailing) is the trade panel's, so a limit order is the same trade — just entered
    // at a rate of your choosing instead of at the market.
    const QDoubleSpinBox *rateField = isBuy ? m_limitBuyRate : m_limitSellRate;
    const double rate = rateField->value();
    const QString side = isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");
    if (rate <= 0.0) {
        appendLog(QStringLiteral("Limit %1 not sent — enter the rate to enter at first.")
                      .arg(side),
                  true);
        return;
    }
    // eToro releases a market-if-touched order as soon as the trigger rate "or better"
    // is published — better meaning LOWER for a buy and HIGHER for a short. A trigger
    // on the wrong side of the current price is therefore already satisfied and can
    // fill straight away instead of resting. Say so before it goes out; the order is
    // still submitted, because only the user knows what they intended.
    const bool alreadyReached =
        (m_lastPrice > 0.0) && (isBuy ? (m_lastPrice <= rate) : (m_lastPrice >= rate));
    if (alreadyReached) {
        appendLog(QStringLiteral("Note: %1 is already at or past a limit %2 of %3 — eToro fills "
                                 "\"trigger rate or better\", so this order may execute "
                                 "immediately at the market instead of waiting.")
                      .arg(QLocale().toString(m_lastPrice, 'f',
                                              trading::priceDecimals(m_lastPrice)),
                           side)
                      .arg(rate, 0, 'f', trading::priceDecimals(rate)),
                  true);
    }
    placeOrder(isBuy, rate);
}

void MainWindow::placeOrder(bool isBuy, double triggerRate)
{
    const double leverage = m_leverage->currentText().toDouble();
    const bool trailingStop = m_trailingStop->isChecked();
    const bool isLimit = (triggerRate > 0.0);
    const QString side = isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");

    // Guard 0 — market closed: the buttons are already disabled then, but the keyboard
    // double-tap reaches placeOrder() directly, so block here too. "Trade anyway"
    // (REQ-F-026) lets the user overrule the verdict; the order is then logged as
    // overridden, because the console is the record of what was sent and why.
    // A LIMIT order is exempt: resting one while the market is shut is the normal way
    // to be positioned for the next session, and eToro parks it until then.
    const QString sym = m_client->config().symbol;
    if (!isLimit && m_tradeabilityKnown && !m_tradeableNow.contains(sym)) {
        if (!marketClosedOverridden()) {
            appendLog(side + QStringLiteral(" blocked — %1's market is closed; eToro isn't "
                                            "accepting opening orders right now. Tick "
                                            "\"Trade anyway\" to submit regardless.").arg(sym),
                      true);
            return;
        }
        appendLog(QStringLiteral("%1 on %2 submitted with the market-closed override — the "
                                 "app reads this market as closed.").arg(side, sym));
    }

    // The trade inputs are in euro (display currency); the eToro API works in the
    // USD account currency, so convert before any exposure check or submission.
    const double amountEur = m_amount->value();
    const double stopLossEur = m_stopLoss->value();
    const double takeProfitEur = m_takeProfit->value();

    // Guard 1 — need the live EUR/USD rate to turn the euro inputs into the USD the
    // API actually receives; without it we could send a wrongly-sized real order.
    if (m_eurPerUsd <= 0.0) {
        appendLog(side + QStringLiteral(" blocked — waiting for the EUR/USD rate; try again "
                                        "in a moment."),
                  true);
        return;
    }
    const double amount = fromDisplay(amountEur);          // USD sent to eToro
    const double stopLoss = fromDisplay(stopLossEur);
    const double takeProfit = fromDisplay(takeProfitEur);

    // Guard 2 — cooldown: reject if another order was placed less than 2s ago.
    if (m_orderCooldownTimer->isActive()) {
        appendLog(QStringLiteral("%1 ignored — wait %2s between orders.")
                      .arg(side, QString::number(kOrderCooldownMs / 1000.0, 'f', 0)),
                  true);
        return;
    }

    // Guard 3 — exposure cap (checked in USD, the account currency; shown in euro):
    // reject if this order would push the total tied up in open trades over the limit.
    // Orders already resting at the broker count: each one WILL become exposure when it
    // triggers, and nobody is at the keyboard then to be warned.
    const double committed = m_openTradesTotal + pendingExposureTotal();
    if ((committed + amount) > (kMaxOpenExposure + 1e-6)) {
        const QString msg =
            QStringLiteral("%1 blocked — open trades plus resting limit orders would reach "
                           "%2%3, over the %2%4 limit (%2%5 already committed).")
                .arg(side, m_ccy)
                .arg(toDisplay(committed + amount), 0, 'f', 2)
                .arg(toDisplay(kMaxOpenExposure), 0, 'f', 2)
                .arg(toDisplay(committed), 0, 'f', 2);
        appendLog(msg, true);
        [[maybe_unused]] const QMessageBox::StandardButton btn =
            QMessageBox::warning(this, QStringLiteral("Open-trades limit"), msg);
        return;
    }

    // No modal confirmation (the buttons' double-press is the gate) — but log the
    // euro order AND the exact USD amount eToro will receive, so the conversion is
    // visible before it goes out.
    appendLog(isLimit
                  ? QStringLiteral("Sending %1 LIMIT order to eToro at rate %8: %2%3 (≈ $%4) "
                                   "(x%5), SL %2%6 / TP %2%7 measured from that rate…")
                        .arg(side, m_ccy)
                        .arg(amountEur, 0, 'f', 2)
                        .arg(amount, 0, 'f', 2)
                        .arg(leverage)
                        .arg(stopLossEur, 0, 'f', 0)
                        .arg(takeProfitEur, 0, 'f', 0)
                        .arg(triggerRate, 0, 'f', trading::priceDecimals(triggerRate))
                  : QStringLiteral("Submitting %1 order: %2%3 (≈ $%4) (x%5), SL %2%6 / TP %2%7…")
                        .arg(side, m_ccy)
                        .arg(amountEur, 0, 'f', 2)
                        .arg(amount, 0, 'f', 2)
                        .arg(leverage)
                        .arg(stopLossEur, 0, 'f', 0)
                        .arg(takeProfitEur, 0, 'f', 0));

    // Start the cooldown (disables the buttons) and optimistically count this order
    // toward the exposure cap *before* submitting, so rapid back-to-back orders
    // can't overshoot the limit before the portfolio refresh lands. This is done
    // before openPosition() because in simulation mode that call synchronously
    // emits portfolioUpdated, whose onPortfolio() recomputes the authoritative
    // total (and overwrites this optimistic value); the periodic refresh reconciles
    // the real-mode case moments later. A limit order opens nothing yet, so it is not
    // counted here — pendingExposureTotal() covers it from the pending list instead.
    if (!isLimit) {
        m_openTradesTotal += amount;
    }
    m_buyButton->setEnabled(false);
    m_sellButton->setEnabled(false);
    m_limitBuyButton->setEnabled(false);
    m_limitSellButton->setEnabled(false);
    m_orderCooldownTimer->start();

    OrderRequest req;
    req.isBuy = isBuy;
    req.amount = amount;
    req.leverage = leverage;
    req.stopLossAmount = stopLoss;
    req.takeProfitAmount = takeProfit;
    req.trailingStop = trailingStop;
    req.triggerRate = triggerRate;  // 0 = market order
    m_client->openPosition(req);
}

double MainWindow::pendingExposureTotal() const
{
    // Account currency, like m_openTradesTotal: what the resting limit orders will tie
    // up once they trigger.
    return std::accumulate(m_pendingShown.cbegin(), m_pendingShown.cend(), 0.0,
                           [](double sum, const PendingOrder &order) {
                               return sum + order.amount;
                           });
}

void MainWindow::onCloseClicked()
{
    const QStringList ids = markedPositionIds();
    if (ids.isEmpty()) {
        [[maybe_unused]] const QMessageBox::StandardButton btn = QMessageBox::information(
            this, QStringLiteral("Close trades"),
            QStringLiteral("Tick one or more open trades to close first."));
        return;
    }
    // Close immediately — no confirmation dialog.
    appendLog((ids.size() == 1)
                  ? QStringLiteral("Closing position #%1…").arg(ids.first())
                  : QStringLiteral("Closing %1 marked positions…").arg(ids.size()));
    for (const QString &id : ids) {
        m_client->closePosition(id);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QStringList MainWindow::markedPositionIds() const
{
    return (m_positionsModel != nullptr) ? m_positionsModel->markedIds() : QStringList{};
}

void MainWindow::appendLog(const QString &message, bool isError)
{
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    const QString prefix = isError ? QStringLiteral("[!]") : QStringLiteral("   ");
    m_log->appendPlainText(QStringLiteral("%1 %2 %3").arg(ts, prefix, message));
}
