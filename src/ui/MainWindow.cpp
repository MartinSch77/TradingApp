#include "ui/MainWindow.h"

#include "domain/DecisionEngine.h"
#include "domain/EventInsight.h"
#include "domain/Forecasting.h"
#include "domain/Indicators.h"
#include "domain/PositionMath.h"
#include "domain/SignalEnsemble.h"
#include "services/AiAdvisor.h"
#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"
#include "ui/PriceChart.h"
#include "ui/ScreenerDialog.h"

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
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

// After any buy/sell order, ignore further orders for this long so a double-click
// (or a burst of auto-triggers) can't fire several trades in quick succession.
constexpr qint32 kOrderCooldownMs = 2000;

// Hard cap on the total amount tied up in open trades at once (account currency).
// A new order is rejected if it would push the open-trades total above this.
constexpr double kMaxOpenExposure = 17000.0;

// Open-trades table columns. Named so the SL/TP edit handling and the render loop
// stay in step when a column is inserted (Close cost sits right after P/L).
enum PosCol {
    PosColMark = 0, PosColInstrument, PosColSide, PosColAmount, PosColLev, PosColOpen,
    PosColUnits, PosColPl, PosColCloseCost, PosColSl, PosColTp, PosColCount
};

QString colored(const QString &text, const QString &hexColor)
{
    return QStringLiteral("<span style='color:%1'>%2</span>").arg(hexColor, text);
}

// Colour for an event-impact direction (+1 bullish, -1 bearish, 0 volatile).
QColor impactColor(qint32 dir)
{
    if (dir > 0) {
        return QColor(0x25, 0xb5, 0x63);
    }
    if (dir < 0) {
        return QColor(0xe3, 0x55, 0x55);
    }
    return QColor(0xe0, 0xb0, 0x00);
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
                       QWidget *parent)
    : QMainWindow(parent)
    , m_client(client)
    , m_feeds(feeds)
    , m_aiAdvisor(aiAdvisor)
{
    buildUi();

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
    static_cast<void>(connect(m_feeds, &MarketFeeds::instrumentRatingsUpdated, this,
                              &MainWindow::onInstrumentRatings));
    static_cast<void>(
        connect(m_feeds, &MarketFeeds::instrumentNewsUpdated, this, &MainWindow::onInstrumentNews));
    static_cast<void>(
        connect(m_aiAdvisor, &AiAdvisor::decisionReady, this, &MainWindow::onAiDecision));

    static_cast<void>(connect(m_buyButton, &QPushButton::clicked, this, &MainWindow::onBuyClicked));
    static_cast<void>(connect(m_sellButton, &QPushButton::clicked, this, &MainWindow::onSellClicked));
    static_cast<void>(
        connect(m_closeButton, &QPushButton::clicked, this, &MainWindow::onCloseClicked));

    // Watch application-wide key presses for the double-tap s/b buy/sell shortcut.
    qApp->installEventFilter(this);

    // Cooldown between orders: while it runs the buy/sell buttons are disabled and
    // placeOrder() rejects any order (manual or auto). Re-enable them when it ends.
    m_orderCooldownTimer = new QTimer(this);
    m_orderCooldownTimer->setSingleShot(true);
    m_orderCooldownTimer->setInterval(kOrderCooldownMs);
    static_cast<void>(connect(m_orderCooldownTimer, &QTimer::timeout, this, [this] {
        // Re-enable via the shared path so a closed market keeps the buttons disabled.
        updateTradeButtonsEnabled();
    }));

    // Economic calendar: macro events for the regions that move the selected
    // instrument. Scope it to the startup instrument before the first fetch.
    m_calendar = new EconomicCalendar(this);
    static_cast<void>(
        connect(m_calendar, &EconomicCalendar::eventsUpdated, this, &MainWindow::onEvents));
    static_cast<void>(connect(m_calendar, &EconomicCalendar::log, this, &MainWindow::onLog));
    m_calendar->setInstrument(m_client->config().symbol);
    m_calendar->start();

    // Age events out of the list ~10 min after they pass, without waiting for the
    // calendar's (30-min) re-fetch. Rebuilds only when an event actually drops off.
    m_eventTimer = new QTimer(this);
    m_eventTimer->setInterval(30 * 1000);  // 30 s granularity on the 10-min rule
    static_cast<void>(
        connect(m_eventTimer, &QTimer::timeout, this, [this] { rebuildEventsView(false); }));
    m_eventTimer->start();

    // After a trade closes, wait ~10 s (so eToro's trade-history API reflects it) then
    // refresh the closed-trade P/L. Single-shot and restarted per close, so closing
    // several marked trades at once triggers just one fetch after the last one.
    m_pnlAfterCloseTimer = new QTimer(this);
    m_pnlAfterCloseTimer->setSingleShot(true);
    m_pnlAfterCloseTimer->setInterval(10 * 1000);
    static_cast<void>(
        connect(m_pnlAfterCloseTimer, &QTimer::timeout, m_client, &EtoroClient::fetchMonthlyPnl));

    // "Buy / sell now": scan once at startup, then refresh in the background every few
    // minutes. The scan shares eToro's small rate pool with the price poll, so keep the
    // cadence conservative; the Refresh button covers on-demand updates in between.
    m_recoTimer = new QTimer(this);
    m_recoTimer->setInterval(5 * 60 * 1000);
    static_cast<void>(
        connect(m_recoTimer, &QTimer::timeout, this, &MainWindow::startRecommendationScan));
    m_recoTimer->start();
    // The first scan is kicked off from onReady() (once ids are resolving) rather than
    // a fixed delay, so it works whether the app comes up in real or simulation mode.

    // Show the chart window once the main window is up, offset to the side so it
    // doesn't cover the controls. The user can drag it anywhere (incl. a 2nd screen).
    QTimer::singleShot(0, this, [this] {
        m_chart->move(frameGeometry().topRight() + QPoint(16, 0));
        m_chart->show();
        m_chart->raise();  // bring it to the front on first appearance
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // The chart is a separate top-level window; close it too so the app exits.
    if (m_chart != nullptr) {
        static_cast<void>(m_chart->close());
    }
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Ctrl + mouse wheel zooms the whole UI — both windows' size and all fonts.
    // Requires an active app window (i.e. the user has clicked into the app). The
    // chart is skipped: its own Ctrl+wheel zooms the price/time axis, which we keep.
    if (event->type() == QEvent::Wheel) {
        auto *we = static_cast<QWheelEvent *>(event);
        const bool ctrlHeld = we->modifiers().testFlag(Qt::ControlModifier);
        const qint32 wheelDelta = we->angleDelta().y();
        if (ctrlHeld && (wheelDelta != 0) && (QApplication::activeWindow() != nullptr)) {
            QWidget *w = qobject_cast<QWidget *>(watched);
            QWidget *top = (w != nullptr) ? w->window() : nullptr;
            if ((top != nullptr) && (top != m_chart)) {
                const double steps = static_cast<double>(wheelDelta) / 120.0;  // one notch = 120
                setUiScale(m_uiScale * std::pow(1.1, steps));  // wheel up → larger
                return true;  // consume so the widget under the cursor doesn't scroll
            }
        }
    }

    // Reflect the chart window's own show/hide (e.g. its title-bar X) in the toggle.
    if ((watched == m_chart) && (m_chartToggle != nullptr)) {
        if ((event->type() == QEvent::Close) || (event->type() == QEvent::Hide)) {
            const QSignalBlocker block(m_chartToggle);
            m_chartToggle->setChecked(false);
        } else if (event->type() == QEvent::Show) {
            const QSignalBlocker block(m_chartToggle);
            m_chartToggle->setChecked(true);
        } else {
            // other events on the chart window are of no interest here
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (!ke->isAutoRepeat() && ((ke->key() == Qt::Key_S) || (ke->key() == Qt::Key_B))) {
            QWidget *fw = QApplication::focusWidget();
            // Numeric spin fields (amount, SL/TP, auto-order prices) reject letters
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
    return QMainWindow::eventFilter(watched, event);
}

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
    const bool cooldown = (m_orderCooldownTimer != nullptr) && m_orderCooldownTimer->isActive();
    const bool enabled = marketOpen && !cooldown && !m_instrumentResolving;
    m_buyButton->setEnabled(enabled);
    m_sellButton->setEnabled(enabled);
    if (m_marketClosedLabel != nullptr) {
        m_marketClosedLabel->setVisible(m_tradeabilityKnown && !marketOpen);
    }
    if (m_instrumentResolving) {
        const QString tip = QStringLiteral("%1 is still resolving — trading unlocks once the "
                                           "instrument is confirmed.").arg(cur);
        m_buyButton->setToolTip(tip);
        m_sellButton->setToolTip(tip);
    } else if (!marketOpen) {
        const QString tip = QStringLiteral("%1's market is currently closed — eToro does not "
                                           "accept opening orders right now.").arg(cur);
        m_buyButton->setToolTip(tip);
        m_sellButton->setToolTip(tip);
    } else {
        const QString dblTip = QStringLiteral(
            "Press twice within 650 ms to place the order (a single press only arms it).");
        m_buyButton->setToolTip(dblTip);
        m_sellButton->setToolTip(dblTip);
    }
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

    // --- Header: instrument name + live price --------------------------------
    auto *header = new QHBoxLayout;
    m_titleLabel = new QLabel(sym, central);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 6);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);

    // Instrument selector, grouped by asset class. The item text is the eToro
    // internalSymbolFull used for lookup; picking one switches the whole app to it.
    m_instrumentBox = new QComboBox(central);
    m_instrumentBox->setToolTip(QStringLiteral("Switch the traded instrument"));
    auto *instModel = new QStandardItemModel(m_instrumentBox);
    QStringList tradableSymbols;
    auto addInstGroup = [instModel, &tradableSymbols](const QString &header,
                                                      const QStringList &symbols) {
        auto *h = new QStandardItem(header);
        h->setFlags(Qt::NoItemFlags);  // non-selectable category header
        QFont hf = h->font();
        hf.setBold(true);
        h->setFont(hf);
        instModel->appendRow(h);
        for (const QString &s : symbols) {
            instModel->appendRow(new QStandardItem(s));
            tradableSymbols << s;
        }
    };
    addInstGroup(QStringLiteral("Indices"),
                 {QStringLiteral("SPX500"), QStringLiteral("SP.24-7"),
                  QStringLiteral("USDOLLAR"), QStringLiteral("NSDQ100"),
                  QStringLiteral("DJ30"), QStringLiteral("GER40"), QStringLiteral("HKG50"),
                  QStringLiteral("CHINA50"), QStringLiteral("EUSTX50"), QStringLiteral("RTY"),
                  QStringLiteral("Switzerland20"), QStringLiteral("Semiconductors"),
                  QStringLiteral("AI.Leaders"), QStringLiteral("Cybersecurity"),
                  QStringLiteral("Quantum"), QStringLiteral("GoldMiners"),
                  QStringLiteral("Crypto10"), QStringLiteral("Canada60"),
                  QStringLiteral("Sweden30"), QStringLiteral("NSDQ100.24-7"),
                  QStringLiteral("Nuclear"), QStringLiteral("Colombia")});
    addInstGroup(QStringLiteral("Forex"), {QStringLiteral("EURUSD")});
    addInstGroup(QStringLiteral("Commodities"),
                 {QStringLiteral("RUBBER"), QStringLiteral("Gold.24-7"), QStringLiteral("OIL.24-7")});
    m_instrumentBox->setModel(instModel);
    m_client->setTradableSymbols(tradableSymbols);  // for id resolution + portfolio filtering
    m_feeds->setTradableSymbols(tradableSymbols);   // for the bulk web-rating/news fetches
    const qint32 curIdx = m_instrumentBox->findText(m_client->config().symbol);
    if (curIdx >= 0) {
        m_instrumentBox->setCurrentIndex(curIdx);
    }
    // textActivated fires only on user selection, not the programmatic setup above.
    static_cast<void>(
        connect(m_instrumentBox, &QComboBox::textActivated, this, [this](const QString &sym) {
            m_autoInstrumentDone = true;  // a manual pick ends the startup auto-load
            selectInstrument(sym);
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

    // Decision window: alternative sources + AI + algorithm → one final call.
    m_decisionButton = new QPushButton(QStringLiteral("Decision…"), central);
    m_decisionButton->setFocusPolicy(Qt::NoFocus);  // don't swallow the b/s trade shortcuts
    m_decisionButton->setToolTip(QStringLiteral(
        "Cross-check several independent sources (technical ensemble, TradingView "
        "multi-timeframe rating, news sentiment, VIX/calendar regime, and — if configured "
        "— Claude AI) and get one final buy/sell recommendation with sizing."));
    static_cast<void>(
        connect(m_decisionButton, &QPushButton::clicked, this, &MainWindow::openDecision));

    header->addWidget(m_titleLabel);
    header->addSpacing(12);
    header->addWidget(new QLabel(QStringLiteral("Instrument:"), central));
    header->addWidget(m_instrumentBox);
    header->addSpacing(8);
    header->addWidget(m_chartToggle);
    header->addWidget(m_screenerButton);
    header->addWidget(m_decisionButton);
    header->addStretch();
    header->addLayout(priceCol);
    root->addLayout(header);

    // --- Mode badge ----------------------------------------------------------
    m_modeLabel = new QLabel(central);
    m_modeLabel->setAlignment(Qt::AlignCenter);
    m_modeLabel->setContentsMargins(6, 4, 6, 4);
    root->addWidget(m_modeLabel);

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

    // --- Controls below the chart --------------------------------------------
    auto *lower = new QWidget(central);
    auto *lowerLayout = new QVBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 0, 0, 0);

    auto *controlsRow = new QHBoxLayout;

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

    // Shown only when the selected instrument's market is currently closed (eToro
    // rejects opening orders then); the BUY/SELL buttons are disabled alongside it.
    m_marketClosedLabel = new QLabel(
        QStringLiteral("⚠ Market closed — opening trades unavailable right now"), tradeBox);
    m_marketClosedLabel->setStyleSheet(QStringLiteral("color:#e35555; font-weight:bold;"));
    m_marketClosedLabel->setAlignment(Qt::AlignCenter);
    m_marketClosedLabel->setWordWrap(true);
    m_marketClosedLabel->setVisible(false);
    tradeForm->addRow(m_marketClosedLabel);

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

    tradeBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    // Conditional (auto) orders — a separately-armed BUY-below and SELL-above.
    auto *autoBox = new QGroupBox(QStringLiteral("Auto orders (conditional)"), lower);
    auto *autoForm = new QFormLayout(autoBox);

    // Style/log wiring shared by both arm buttons.
    auto wireArm = [this](QPushButton *btn, const QString &what) {
        btn->setCheckable(true);
        static_cast<void>(connect(btn, &QPushButton::toggled, this, [this, btn, what](bool on) {
            btn->setText(on ? QStringLiteral("Armed ●") : QStringLiteral("Arm"));
            btn->setStyleSheet(on ? QStringLiteral("QPushButton { background:#b26a00; color:white; "
                                                   "font-weight:bold; border-radius:4px; }")
                                  : QString());
            if (on) {
                appendLog(QStringLiteral("Auto-%1 armed — watching price.").arg(what));
            }
        }));
    };

    m_buyBelow = new QDoubleSpinBox(autoBox);
    m_buyBelow->setRange(0.0, 10'000'000.0);
    m_buyBelow->setDecimals(2);
    m_buyBelow->setValue(0.0);
    m_buyBelow->setSpecialValueText(QStringLiteral("off"));  // shown when 0
    m_buyBelow->setToolTip(QStringLiteral("Auto-BUY when the price falls below this (0 = off)"));
    m_armBuy = new QPushButton(QStringLiteral("Arm"), autoBox);
    m_armBuy->setToolTip(QStringLiteral("Arm the auto-BUY (fires once when price < the value)"));
    wireArm(m_armBuy, QStringLiteral("BUY"));
    auto *buyRow = new QHBoxLayout;
    buyRow->addWidget(m_buyBelow, 1);
    buyRow->addWidget(m_armBuy, 0);
    autoForm->addRow(QStringLiteral("Buy if price <"), buyRow);

    m_sellAbove = new QDoubleSpinBox(autoBox);
    m_sellAbove->setRange(0.0, 10'000'000.0);
    m_sellAbove->setDecimals(2);
    m_sellAbove->setValue(0.0);
    m_sellAbove->setSpecialValueText(QStringLiteral("off"));
    m_sellAbove->setToolTip(QStringLiteral("Auto-SELL when the price rises above this (0 = off)"));
    m_armSell = new QPushButton(QStringLiteral("Arm"), autoBox);
    m_armSell->setToolTip(QStringLiteral("Arm the auto-SELL (fires once when price > the value)"));
    wireArm(m_armSell, QStringLiteral("SELL"));
    auto *sellRow = new QHBoxLayout;
    sellRow->addWidget(m_sellAbove, 1);
    sellRow->addWidget(m_armSell, 0);
    autoForm->addRow(QStringLiteral("Sell if price >"), sellRow);

    // Trading-signals panel — buy/sell/close guidance from the instrument's technicals.
    m_sigBox = new QGroupBox(QStringLiteral("Trading signals — %1").arg(sym), lower);
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

    // --- AI decision-support panel ------------------------------------------
    m_aiBox = new QGroupBox(QStringLiteral("AI decision support"), lower);
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

    // Left column: trade panel, auto-orders, then the (tall) signals panel.
    auto *leftCol = new QVBoxLayout;
    leftCol->addWidget(tradeBox);
    leftCol->addWidget(autoBox);
    leftCol->addWidget(sigBox);
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

    // Positions panel
    auto *posBox = new QGroupBox(QStringLiteral("Open trades"), lower);
    auto *posLayout = new QVBoxLayout(posBox);

    m_positions = new QTableWidget(0, PosColCount, posBox);
    m_positions->setHorizontalHeaderLabels(
        {QStringLiteral("Position"), QStringLiteral("Instrument"), QStringLiteral("Side"),
         QStringLiteral("Amount"), QStringLiteral("Lev"), QStringLiteral("Open"),
         QStringLiteral("Units"), QStringLiteral("P/L (%1)").arg(m_ccy),
         QStringLiteral("Close (%1)").arg(m_ccy), QStringLiteral("SL (%1)").arg(m_ccy),
         QStringLiteral("TP (%1)").arg(m_ccy)});
    m_positions->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_positions->verticalHeader()->setVisible(false);
    // Trades are marked with the per-row checkbox in the Position column, so plain
    // row selection is disabled to avoid two competing notions of "chosen".
    m_positions->setSelectionMode(QAbstractItemView::NoSelection);
    // Only the Stop-loss / Take-profit cells are editable (flag set per-item); a
    // double-click or F2 on one of those opens the editor.
    m_positions->setEditTriggers(QAbstractItemView::DoubleClicked
                                 | QAbstractItemView::EditKeyPressed);
    m_positions->setToolTip(QStringLiteral(
        "Tick one or more trades, then Close marked trades.\n"
        "Double-click a trade to switch the app to its instrument.\n"
        "Double-click a Stop-loss / Take-profit cell to change it (amount in %1; blank clears it).\n"
        "SL is signed P/L: a negative value closes at a loss, a positive value locks in a "
        "profit (stop on the winning side).")
                                .arg(m_ccy));
    static_cast<void>(connect(m_positions, &QTableWidget::itemChanged, this,
                              &MainWindow::onPositionCellChanged));
    // Double-click a trade (outside the mark checkbox / editable SL/TP cells) to switch
    // the app to that instrument, like picking it from the selector.
    static_cast<void>(
        connect(m_positions, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
            if ((col == PosColMark) || (col >= PosColSl)) {  // mark checkbox / editable SL,TP
                return;
            }
            if ((row < 0) || (row >= m_shownPositions.size())) {
                return;
            }
            const QString sym = m_shownPositions[row].symbol;
            if (sym.isEmpty()) {
                return;
            }
            m_autoInstrumentDone = true;  // a manual pick ends the startup auto-load
            selectInstrument(sym);
        }));
    posLayout->addWidget(m_positions);

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
         QStringLiteral("Net P/L (%1)").arg(m_ccy), QStringLiteral("Fees")});
    m_pnlTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_pnlTable->verticalHeader()->setVisible(false);
    m_pnlTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_pnlTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pnlTable->setFocusPolicy(Qt::NoFocus);
    m_pnlTable->setMaximumHeight(150);
    m_pnlTable->setToolTip(QStringLiteral(
        "Net P/L of trades closed in the last 7 weeks, only for instruments in the "
        "selector. Whole-account totals are shown above for context."));
    pnlLayout->addWidget(m_pnlTable);

    auto *pnlButtons = new QHBoxLayout;
    m_pnlRefresh = new QPushButton(QStringLiteral("Refresh closed-trade P/L"), m_pnlBox);
    static_cast<void>(
        connect(m_pnlRefresh, &QPushButton::clicked, m_client, &EtoroClient::fetchMonthlyPnl));
    m_pnlDetails = new QPushButton(QStringLiteral("All trades…"), m_pnlBox);
    m_pnlDetails->setToolTip(QStringLiteral(
        "Every closed trade of the last 7–13 weeks (lookback selectable), with net "
        "P/L, rollover fees and estimated opening/closing spread costs per trade."));
    static_cast<void>(
        connect(m_pnlDetails, &QPushButton::clicked, this, &MainWindow::openClosedTrades));
    pnlButtons->addWidget(m_pnlRefresh);
    pnlButtons->addWidget(m_pnlDetails);
    pnlLayout->addLayout(pnlButtons);

    // Right column: open trades (stretches) with the closed-trade summary and the AI
    // decision support beneath it, balancing the tall left-hand signals panel.
    auto *rightCol = new QVBoxLayout;
    rightCol->addWidget(posBox, 1);
    rightCol->addWidget(m_pnlBox, 0);
    rightCol->addWidget(m_aiBox, 0);
    controlsRow->addLayout(rightCol, 1);
    lowerLayout->addLayout(controlsRow);

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
                const QString sym =
                    (it != nullptr) ? it->data(Qt::UserRole).toString() : QString();
                if (sym.isEmpty()) {
                    return;
                }
                m_autoInstrumentDone = true;  // a manual pick ends the startup auto-load
                selectInstrument(sym);
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

    // Events + recommendations sit side by side in a draggable splitter: market events
    // gets the larger share by default, and the user can adjust the split at runtime.
    auto *eventsSplitter = new QSplitter(Qt::Horizontal, lower);
    eventsSplitter->addWidget(m_eventsBox);
    eventsSplitter->addWidget(m_recoBox);
    eventsSplitter->setChildrenCollapsible(false);  // neither pane can be dragged to zero
    eventsSplitter->setStretchFactor(0, 3);  // events grows faster than reco on resize
    eventsSplitter->setStretchFactor(1, 1);
    eventsSplitter->setSizes({300, 120});     // default ~5:2 width split
    lowerLayout->addWidget(eventsSplitter);

    // Log panel. Pin the box to its content height (a fixed-height log + margins):
    // without this the group box expands to soak up all spare vertical space on a
    // tall window, leaving a huge empty Activity area. Fixed here means the spare
    // space flows to the open-trades / events panels above instead.
    auto *logBox = new QGroupBox(QStringLiteral("Activity"), lower);
    auto *logLayout = new QVBoxLayout(logBox);
    logLayout->setContentsMargins(6, 4, 6, 4);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setFixedHeight(68);
    logLayout->addWidget(m_log);
    logBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    lowerLayout->addWidget(logBox);

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
    // A short delay lets the listed-instrument id resolution progress first, so the
    // filter to listed instruments is as complete as possible on the first fetch.
    if (!m_pnlAutoFetched) {
        m_pnlAutoFetched = true;
        QTimer::singleShot(1500, m_client, &EtoroClient::fetchMonthlyPnl);
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

    // Seed the auto-order thresholds off the first known price: buy 0.5% below,
    // sell 1% above. Done once so the user can still edit them afterwards.
    if (!m_autoDefaultsSet && (price > 0.0)) {
        m_autoDefaultsSet = true;
        m_buyBelow->setValue(price * 0.995);
        m_sellAbove->setValue(price * 1.01);
    }

    updateOpenTradePnl(price);  // live-refresh open-trade P/L with the new price
    updateSignals();
    checkAutoOrders(price);
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

    m_openCost->setText(
        QStringLiteral("Buy <b>%1%2</b> &nbsp;·&nbsp; Sell <b>%1%3</b> "
                       "<span style='color:#9a9a9a'>(spread %4)</span>")
            .arg(m_ccy)
            .arg(QLocale().toString(costBuy, 'f', 2))
            .arg(QLocale().toString(costSell, 'f', 2))
            .arg(QLocale().toString(spread, 'f', trading::priceDecimals(ask))));

    // Rollover fees scale with the position's unit count. The fees are quoted in
    // USD per unit; multiplying by units derived from the euro amount lands in
    // euro via the same conversion-cancels identity as the spread cost above.
    if (m_feeCost != nullptr) {
        if (!m_fees.isValid()) {
            m_feeCost->setText(QStringLiteral("<span style='color:#9a9a9a'>—</span>"));
        } else {
            const double unitsBuy = (amount * leverage) / ask;
            const double unitsSell = (amount * leverage) / bid;
            m_feeCost->setText(
                QStringLiteral("Buy <b>%1%2</b> &nbsp;·&nbsp; Sell <b>%1%3</b> per night "
                               "<span style='color:#9a9a9a'>(weekend %1%4 / %1%5)</span>")
                    .arg(m_ccy)
                    .arg(QLocale().toString(m_fees.buyOvernight * unitsBuy, 'f', 2))
                    .arg(QLocale().toString(m_fees.sellOvernight * unitsSell, 'f', 2))
                    .arg(QLocale().toString(m_fees.buyWeekend * unitsBuy, 'f', 2))
                    .arg(QLocale().toString(m_fees.sellWeekend * unitsSell, 'f', 2)));
        }
    }
}

void MainWindow::setUiScale(double scale)
{
    scale = std::clamp(scale, 0.6, 2.5);  // 60%–250%: readable, never off-screen
    if (qFuzzyCompare(scale, m_uiScale))
        return;
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

QTableWidgetItem *MainWindow::makePlItem(double profitUsd) const
{
    const QString amountText = QLocale().toString(qAbs(toDisplay(profitUsd)), 'f', 2);
    const QString text =
        ((profitUsd < 0.0) ? QStringLiteral("-") : QString()) + m_ccy + amountText;
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setForeground(profitUsd >= 0.0 ? QColor(0x25, 0xb5, 0x63)
                                         : QColor(0xe3, 0x55, 0x55));
    return item;
}

// Re-price the open-trades P/L column from a fresh live price, in place, so it tracks
// the chart between the ~3s portfolio polls. Each shown-instrument row shows its last
// API P/L plus the account-currency value of the price move since m_pnlAnchorPrice
// (same value-per-point identity the SL/TP maths uses — FX-free). Positions on other
// instruments are left as the poll supplied them (no live price for them here).
void MainWindow::updateOpenTradePnl(double price)
{
    if ((m_positions == nullptr) || (price <= 0.0) || (m_pnlAnchorPrice <= 0.0)
        || m_shownPositions.isEmpty()) {
        return;
    }
    const QString shown = m_client->instrument().symbol;

    // These are non-editable, programmatic fills: don't let itemChanged fire.
    m_updatingPositions = true;
    const QSignalBlocker block(m_positions);
    const qsizetype rows = std::min<qsizetype>(m_shownPositions.size(), m_positions->rowCount());
    for (qsizetype row = 0; row < rows; ++row) {
        const Position &p = m_shownPositions[row];
        if ((p.openRate <= 0.0) || (p.symbol.compare(shown, Qt::CaseInsensitive) != 0)) {
            continue;
        }
        const double perPoint = trading::accountValuePerPoint(p);
        if (perPoint <= 0.0) {
            continue;
        }
        const double delta = (p.isBuy ? 1.0 : -1.0) * perPoint * (price - m_pnlAnchorPrice);
        m_positions->setItem(static_cast<int>(row), PosColPl, makePlItem(p.profit + delta));
    }
    m_updatingPositions = false;
}

void MainWindow::onPortfolio(const QList<Position> &positionsIn)
{
    // Show trades in a fixed order by position number, so the rows never reshuffle
    // just because the API returned them differently between polls (a reshuffle would
    // force a full rebuild, clearing the ticked checkboxes and any open SL/TP editor).
    QList<Position> positions = positionsIn;
    const auto sortBegin = positions.begin();
    const auto sortEnd = positions.end();
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
    for (const Position &p : positions) {
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
    double openTotal = 0.0;
    for (const Position &p : positions) {
        openTotal += p.amount;
    }
    m_openTradesTotal = openTotal;

    // Row-indexed snapshot so the SL/TP editors can map a row back to its trade.
    m_shownPositions = positions;

    // Anchor the just-supplied API P/L to the current live price, so updateOpenTradePnl
    // can re-price these trades against later ticks without a jump at the next poll.
    m_pnlAnchorPrice = m_lastPrice;

    // A full rebuild replaces every cell (destroying any open SL/TP editor) and
    // resets the marked checkboxes, so only do it when the set/order of trades
    // actually changes. Otherwise refresh values in place, leaving an in-progress
    // edit — and the ticked checkboxes — untouched across the periodic poll.
    bool sameRows = m_positions->rowCount() == positions.size();
    for (qsizetype row = 0; sameRows && (row < positions.size()); ++row) {
        QTableWidgetItem *idItem = m_positions->item(static_cast<int>(row), 0);
        if (idItem == nullptr) {
            sameRows = false;
        } else {
            const QString shownId = idItem->data(Qt::UserRole).toString();
            const QString polledId = positions[row].positionId;
            if (shownId != polledId) {
                sameRows = false;
            }
        }
    }
    // An open cell editor is a QLineEdit parented into the table; don't overwrite
    // its cell while the user is typing there.
    QWidget *fw = QApplication::focusWidget();
    const bool editing =
        (qobject_cast<QLineEdit *>(fw) != nullptr) && m_positions->isAncestorOf(fw);

    // itemChanged must not fire for these programmatic fills.
    m_updatingPositions = true;
    const QSignalBlocker block(m_positions);

    // Override a position's SL/TP with the value the user just submitted, until the
    // server snapshot converges to it (or the pin times out) — see m_pendingSlTp.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kPendingTtlMs = 12000;  // give the server up to ~12s to reflect it
    auto ratesClose = [](double a, double b) {
        const double scale = std::max({std::abs(a), std::abs(b), 1.0});
        return std::abs(a - b) <= scale * 1e-4;  // absorbs 2dp/5dp round-trip rounding
    };
    auto withPending = [this, nowMs, kPendingTtlMs, &ratesClose](Position p) -> Position {
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

    auto plItem = [this](const Position &p) { return makePlItem(p.profit); };
    // Expected spread cost to close the position now (account currency → euro).
    auto closeCostItem = [this](const Position &p) {
        const QString text = p.closingCost > 0.0
                                 ? m_ccy + QLocale().toString(toDisplay(p.closingCost), 'f', 2)
                                 : QStringLiteral("—");
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignCenter);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        item->setForeground(QColor(0x9a, 0x9a, 0x9a));
        item->setToolTip(QStringLiteral(
            "Estimated cost to close this trade at the current price — eToro attributes half "
            "the bid/ask spread to the exit, i.e. roughly spread/2 × units, matching its close "
            "dialog. eToro's P/L already reflects this (a long is valued at the bid, a short at "
            "the ask)."));
        return item;
    };
    auto slTpItem = [](const QString &text) {  // editable: Stop-loss / Take-profit
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignCenter);
        item->setFlags((item->flags() | Qt::ItemIsEditable | Qt::ItemIsEnabled)
                       & ~Qt::ItemIsUserCheckable);
        return item;
    };
    // Stop-loss cell, tagged with a "⇅" marker + tooltip when the stop trails the price.
    auto slItem = [&slTpItem, this](const Position &p) {
        QString text = trading::slSignedAmountText(p, m_eurPerUsd);  // signed: −loss / +profit
        const bool trailing = p.trailingStop && (p.stopLossRate > 0.0) && !text.isEmpty();
        if (trailing) {
            text += QStringLiteral(" ⇅");
        }
        QTableWidgetItem *item = slTpItem(text);
        if (trailing) {
            item->setToolTip(
                QStringLiteral("Trailing stop-loss (follows the price in your favour)"));
        }
        return item;
    };

    if (!sameRows) {
        const QStringList markedIds = markedPositionIds();
        const QSet<QString> marked(markedIds.cbegin(), markedIds.cend());

        auto makeItem = [](const QString &text) {
            auto *item = new QTableWidgetItem(text);
            item->setTextAlignment(Qt::AlignCenter);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            return item;
        };
        // Side cell coloured by direction: green BUY / red SELL.
        auto sideItem = [&makeItem](bool isBuy) {
            QTableWidgetItem *item = makeItem(isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
            item->setForeground(isBuy ? QColor(0x25, 0xb5, 0x63) : QColor(0xe3, 0x55, 0x55));
            return item;
        };

        const qint32 posCount = static_cast<qint32>(positions.size());
        m_positions->setRowCount(posCount);
        for (qint32 row = 0; row < posCount; ++row) {
            const Position p = withPending(positions[row]);
            auto *idItem = makeItem(p.positionId);
            idItem->setData(Qt::UserRole, p.positionId);
            idItem->setFlags((idItem->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsSelectable);
            idItem->setCheckState(marked.contains(p.positionId) ? Qt::Checked : Qt::Unchecked);
            m_positions->setItem(row, PosColMark, idItem);
            m_positions->setItem(row, PosColInstrument, makeItem(p.symbol));
            m_positions->setItem(row, PosColSide, sideItem(p.isBuy));
            // Invested amount (account currency → euro), rounded up to the next whole unit.
            m_positions->setItem(row, PosColAmount,
                                 makeItem(QLocale().toString(std::ceil(toDisplay(p.amount)), 'f', 0)));
            m_positions->setItem(row, PosColLev,
                                 makeItem((p.leverage > 0.0)
                                              ? QStringLiteral("x%1").arg(p.leverage, 0, 'f', 0)
                                              : QStringLiteral("—")));
            m_positions->setItem(
                row, PosColOpen,
                makeItem(QLocale().toString(p.openRate, 'f', trading::priceDecimals(p.openRate))));
            m_positions->setItem(row, PosColUnits, makeItem(QLocale().toString(p.units, 'f', 4)));
            m_positions->setItem(row, PosColPl, plItem(p));
            m_positions->setItem(row, PosColCloseCost, closeCostItem(p));
            m_positions->setItem(row, PosColSl, slItem(p));
            m_positions->setItem(row, PosColTp,
                                 slTpItem(trading::slTpAmountText(p, p.takeProfitRate, m_eurPerUsd)));
        }
    } else {
        const qint32 posCount = static_cast<qint32>(positions.size());
        for (qint32 row = 0; row < posCount; ++row) {
            const Position p = withPending(positions[row]);
            m_positions->setItem(row, PosColPl, plItem(p));
            m_positions->setItem(row, PosColCloseCost, closeCostItem(p));
            // Don't overwrite an SL/TP cell the user is currently editing.
            if (!editing) {
                m_positions->setItem(row, PosColSl, slItem(p));
                m_positions->setItem(row, PosColTp,
                                     slTpItem(trading::slTpAmountText(p, p.takeProfitRate, m_eurPerUsd)));
            }
        }
    }

    // Drop pins for positions no longer open (e.g. closed), so the map can't grow.
    if (!m_pendingSlTp.isEmpty()) {
        QSet<QString> openIds;
        openIds.reserve(positions.size());
        for (const Position &p : positions) {
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

    m_updatingPositions = false;

    // On the first portfolio after startup, adopt the instrument shown at the top of
    // the open-trades list. One-shot: a later-opened trade — or the portfolio re-fetch
    // triggered by the switch itself — must never override the current view.
    if (!m_autoInstrumentDone) {
        m_autoInstrumentDone = true;
        if (!positions.isEmpty()) {
            const QString top = positions.first().symbol;
            if (!top.isEmpty()
                && (top.compare(m_client->config().symbol, Qt::CaseInsensitive) != 0)) {
                selectInstrument(top);
            }
        }
    }
}

void MainWindow::onPositionCellChanged(QTableWidgetItem *item)
{
    if (m_updatingPositions || (item == nullptr)) {
        return;
    }
    const qint32 col = item->column();
    if ((col != PosColSl) && (col != PosColTp)) {  // only Stop-loss / Take-profit are editable
        return;
    }
    const qint32 row = item->row();
    if ((row < 0) || (row >= m_shownPositions.size())) {
        return;
    }
    const Position p = m_shownPositions[row];  // copy: the snapshot may change under us
    if ((p.units <= 0.0) || (p.openRate <= 0.0)) {
        appendLog(QStringLiteral("Can't set SL/TP yet — trade has no open rate/units."), true);
        return;
    }

    // Read a cell as a signed account-currency amount (blank / 0 / unparsable = clear).
    auto cellValue = [this](qint32 r, qint32 c) -> double {
        QTableWidgetItem *it = m_positions->item(r, c);
        if (it == nullptr) {
            return 0.0;
        }
        QString t = it->text().trimmed();
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

    // Normalise the edited cell now (guarded so it doesn't recurse); safe because no
    // table rebuild happens here. SL shows its sign; TP is a plain positive amount.
    m_updatingPositions = true;
    if (col == PosColSl) {
        item->setText((slPnlDisp != 0.0)
                          ? (((slPnlDisp < 0.0) ? QStringLiteral("-") : QStringLiteral("+"))
                             + QLocale().toString(std::abs(slPnlDisp), 'f', 2))
                          : QString());
    } else {
        item->setText((tpAmtDisp > 0.0) ? QLocale().toString(tpAmtDisp, 'f', 2) : QString());
    }
    m_updatingPositions = false;

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

void MainWindow::updateSignals()
{
    // Keep the estimated opening (spread) cost in step with the live quote and the
    // current amount/leverage — this slot is wired to all three. Done before the
    // early return below so the cost still shows while the signal series fills.
    updateOpenCost();

    // Compute over the HOURLY close series (one bar = one hour), with the last
    // (forming-hour) bar pinned to the live price so the signals still track the
    // market. Hourly — not the minute tail — so a symbol reads the same here as in
    // the leverage screener. Horizon maths below is therefore in hourly bars.
    QList<double> series = m_hourlyCloses;
    if (m_lastPrice > 0.0) {
        if (series.isEmpty()) {
            series.append(m_lastPrice);
        } else {
            series.last() = m_lastPrice;
        }
    }

    // VIX / calendar regime and news sentiment — the same two sources the Decision
    // window uses, surfaced here as signal rows. Independent of the price series, so
    // set them before any early return.
    {
        const QString green = QStringLiteral("#25b563");
        const QString red = QStringLiteral("#e35555");
        const QString amber = QStringLiteral("#e0b000");
        const QString grey = QStringLiteral("#9a9a9a");

        bool eventRisk = false;
        const double regime = trading::marketRegime(marketSnapshot(), eventRisk);
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

        qint32 newsCount = 0;
        const QList<NewsHeadline> news = m_newsBySymbol.value(m_client->config().symbol);
        if (news.isEmpty()) {
            m_sigNews->setText(QStringLiteral("<span style='color:%1'>n/a</span>").arg(grey));
        } else {
            const double s = trading::newsSentimentScore(news, newsCount);
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

    constexpr qsizetype kFast = 10;
    constexpr qsizetype kSlow = 30;
    constexpr qsizetype kRsi = 14;

    if (series.size() < (kSlow + 1)) {
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
        return;
    }

    const double fast = trading::sma(series, kFast);
    const double slow = trading::sma(series, kSlow);
    const double r = trading::rsi(series, kRsi);
    const double hist = trading::macdHistogram(series);
    const double pctB = trading::bollingerPercentB(series, 20);
    const double vol = trading::volatilityPct(series, 20);
    const double momentum = trading::roc(series, 10);
    const double first = series.first();
    const double changePct =
        (first > 0.0) ? (((series.last() - first) / first) * 100.0) : 0.0;

    const QString green = QStringLiteral("#25b563");
    const QString red = QStringLiteral("#e35555");
    const QString amber = QStringLiteral("#e0b000");

    // --- Individual indicators ---------------------------------------------
    const bool bull = fast > slow;
    m_sigTrend->setText(colored(bull ? QStringLiteral("Bullish ▲") : QStringLiteral("Bearish ▼"),
                                bull ? green : red));

    QString rsiState = QStringLiteral("Neutral");
    QString rsiColor = amber;
    if (r >= 70.0) {
        rsiState = QStringLiteral("Overbought");
        rsiColor = red;
    } else if (r <= 30.0) {
        rsiState = QStringLiteral("Oversold");
        rsiColor = green;
    }
    m_sigMomentum->setText(
        colored(QStringLiteral("%1 (%2)").arg(r, 0, 'f', 1).arg(rsiState), rsiColor));

    m_sigMacd->setText(colored(hist >= 0.0 ? QStringLiteral("Bullish ▲") : QStringLiteral("Bearish ▼"),
                               hist >= 0.0 ? green : red));

    QString bollState = QStringLiteral("mid-band");
    QString bollColor = amber;
    if (pctB >= 0.9) {
        bollState = QStringLiteral("upper — stretched");
        bollColor = red;
    } else if (pctB <= 0.1) {
        bollState = QStringLiteral("lower — stretched");
        bollColor = green;
    }
    m_sigBoll->setText(colored(QStringLiteral("%1 (%2)").arg(pctB, 0, 'f', 2).arg(bollState), bollColor));

    m_sigVol->setText(colored(QStringLiteral("±%1%/bar").arg(vol, 0, 'f', 3), amber));

    // Least-squares regression trend over the last 30 closes (slope + R² fit).
    const trading::Regression reg = trading::linRegForecast(series, 30);
    const QString regDir = (reg.slopePct > 0.0)
                               ? QStringLiteral("↑")
                               : ((reg.slopePct < 0.0) ? QStringLiteral("↓")
                                                       : QStringLiteral("→"));
    const QString slopeText = QString::number(reg.slopePct, 'f', 3);
    const QString r2Text = QString::number(reg.r2, 'f', 2);
    m_sigRegression->setText(colored(
        regDir + QLatin1Char(' ') + slopeText + QStringLiteral("%/bar  R² ") + r2Text,
        (reg.slopePct > 0.0) ? green : ((reg.slopePct < 0.0) ? red : amber)));

    // k-Nearest-Neighbors analog forecast: match the current 10-bar pattern to
    // history and average what followed the 5 closest analogs.
    const trading::Knn kn = trading::knnForecast(series, 10, 5);
    const QString knnDir = (kn.retPct > 0.0)
                               ? QStringLiteral("↑")
                               : ((kn.retPct < 0.0) ? QStringLiteral("↓")
                                                    : QStringLiteral("→"));
    const QString knnRetText = QString::number(kn.retPct, 'f', 3);
    const QString knnAgreeText = QString::number(kn.agree * 100.0, 'f', 0);
    m_sigKnn->setText(colored(
        knnDir + QLatin1Char(' ') + ((kn.retPct >= 0.0) ? QStringLiteral("+") : QString())
            + knnRetText + QStringLiteral("%  (") + knnAgreeText + QStringLiteral("% agree)"),
        (kn.retPct > 0.0) ? green : ((kn.retPct < 0.0) ? red : amber)));

    // Stochastic %K — entry timing (oversold in an uptrend is a good long entry).
    const double stochK = trading::stochasticK(series, 14);
    QString stochState = QStringLiteral("mid");
    QString stochColor = amber;
    if (stochK >= 80.0) {
        stochState = QStringLiteral("overbought");
        stochColor = red;
    } else if (stochK <= 20.0) {
        stochState = QStringLiteral("oversold");
        stochColor = green;
    }
    m_sigStoch->setText(
        colored(QStringLiteral("%1 (%2)").arg(stochK, 0, 'f', 1).arg(stochState), stochColor));

    // Trend filter: price above the 50-bar SMA = long-friendly regime.
    const double sma50 = trading::sma(series, 50);
    const bool aboveTrend = (sma50 > 0.0) && (series.last() > sma50);
    m_sigTrend50->setText(
        (sma50 <= 0.0) ? colored(QStringLiteral("n/a"), amber)
                       : colored(aboveTrend ? QStringLiteral("above ▲ (uptrend)")
                                            : QStringLiteral("below ▼ (downtrend)"),
                                 aboveTrend ? green : red));

    // Risk gauge for the selected leverage: expected ~1h move × leverage = the
    // swing in your margin. High leverage makes small moves large P/L. On the hourly
    // series one bar IS one hour, so the per-hour move is just the per-bar σ.
    const double lev = m_leverage->currentText().toDouble();
    const double hourMovePct = vol;   // per-bar σ = per-hour σ (1 hourly bar = 1h)
    const double marginSwing = hourMovePct * lev;
    const QString riskColor =
        (marginSwing >= 30.0) ? red : ((marginSwing >= 15.0) ? amber : green);
    m_sigRisk->setText(colored(QStringLiteral("±%1%/h → ±%2% margin (x%3)")
                                   .arg(hourMovePct, 0, 'f', 2)
                                   .arg(marginSwing, 0, 'f', 0)
                                   .arg(lev, 0, 'f', 0),
                               riskColor));

    m_sigChange->setText(colored(QStringLiteral("%1%2%")
                                     .arg((changePct >= 0.0) ? QStringLiteral("+") : QString())
                                     .arg(changePct, 0, 'f', 2),
                                 (changePct >= 0.0) ? green : red));

    // --- Ensemble prediction ("model" vote across the indicators) ----------
    // The directional vote across the indicators is computed by the shared
    // trading::computeEnsemble() (the leverage screener uses the same call, so a symbol
    // ranks identically here and there). It returns the net score, the vote count
    // and the raw confidence; the VIX-level and event-risk confidence haircuts are
    // applied just below. Volatility sets the expected move.
    const trading::Ensemble ens = trading::computeEnsemble(series, m_vixValid, m_vixChangePct);
    const qint32 score = ens.score;
    const qint32 votes = ens.votes;
    double confidence = ens.confidence;

    // High absolute VIX = a fearful, choppy tape: trim confidence (instrument-agnostic).
    QString vixNote;
    if (m_vixValid && (m_vix >= 25.0)) {
        confidence *= ((m_vix >= 35.0) ? 0.6 : 0.8);
        vixNote = QStringLiteral(" · VIX %1").arg(m_vix, 0, 'f', 0);
    }

    // Event risk: an imminent calendar event lowers confidence and flags volatility.
    QString eventNote;
    if (m_nextEventTime.isValid()) {
        const qint64 mins = QDateTime::currentDateTime().secsTo(m_nextEventTime) / 60;
        if ((mins >= 0) && (mins <= 60)) {
            confidence *= 0.5;
            eventNote = QStringLiteral(" ⚠ %1 in %2m").arg(m_nextEventTitle).arg(mins);
        }
    }
    eventNote += vixNote;

    const QString arrow = (score > 0) ? QStringLiteral("↑")
                                      : ((score < 0) ? QStringLiteral("↓")
                                                     : QStringLiteral("→"));
    const QString dirWord = (score > 0) ? QStringLiteral("up")
                                        : ((score < 0) ? QStringLiteral("down")
                                                       : QStringLiteral("flat"));
    const QString predColor = (score > 0) ? green : ((score < 0) ? red : amber);
    const QString confText = QString::number(confidence, 'f', 0);
    const QString volText = QString::number(vol, 'f', 2);
    const QString predText = arrow + QLatin1Char(' ') + dirWord + QStringLiteral("  ~")
                             + confText + QStringLiteral("% conf, ±")
                             + volText + QLatin1Char('%') + eventNote;
    m_sigPrediction->setText(colored(predText, predColor));
    // NB: the chart arrow is now driven by the overall Signal + AI Recommendation
    // (set at the end of this function), not by the raw ensemble score.

    // --- 3-hour forecast ---------------------------------------------------
    // Extrapolate the recent drift over a 3-hourly-bar horizon, with a ±1σ range
    // that scales with √horizon (random-walk diffusion). One bar = one hour now.
    constexpr qint32 kHorizonBars = 3;  // 3 hours = 3 hourly bars
    const qsizetype driftWin = qMin<qsizetype>(series.size() - 1, 48);  // ~2 days of hours
    const double drift = trading::meanReturn(series, driftWin);
    const double projPct = (std::pow(1.0 + drift, kHorizonBars) - 1.0) * 100.0;
    const double bandPct = vol * std::sqrt(static_cast<double>(kHorizonBars));
    const double target = m_lastPrice * std::pow(1.0 + drift, kHorizonBars);
    const QString f3Arrow = (projPct > 0.0)
                                ? QStringLiteral("↑")
                                : ((projPct < 0.0) ? QStringLiteral("↓") : QStringLiteral("→"));
    const QString f3Color = (projPct > 0.0) ? green : ((projPct < 0.0) ? red : amber);
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

    // --- AI forecast: next 3 trading days ----------------------------------
    // Ensemble-driven, bounded projection. The indicator vote (score/votes) sets
    // both direction and how far within the expected ±1σ range over the horizon
    // (√-of-time diffusion) price is likely to travel — so it never runs away the
    // way naive per-minute drift compounded over thousands of bars would.
    constexpr qint32 kHoursPerDay = 24;  // most of these are 24/7 CFDs; hourly bars now
    constexpr qint32 kForecastDays = 3;
    constexpr qint32 kDaysHorizon = kForecastDays * kHoursPerDay;  // 72 hourly bars
    const double dayRangePct = vol * std::sqrt(static_cast<double>(kDaysHorizon));
    const double biasFrac =
        (votes > 0) ? (score / static_cast<double>(votes)) : 0.0;  // [-1, 1]
    const double dayProjPct = biasFrac * dayRangePct;
    const double dayTarget = m_lastPrice * (1.0 + (dayProjPct / 100.0));
    const QString fdArrow = (dayProjPct > 0.0)
                                ? QStringLiteral("↑")
                                : ((dayProjPct < 0.0) ? QStringLiteral("↓")
                                                      : QStringLiteral("→"));
    const QString fdColor = (dayProjPct > 0.0) ? green : ((dayProjPct < 0.0) ? red : amber);
    const QString fdChange = QString::number(dayProjPct, 'f', 2);
    const QString fdTarget = QString::number(dayTarget, 'f', trading::priceDecimals(dayTarget));
    const QString fdBand = QString::number(dayRangePct, 'f', 1);
    const QString fdConf = QString::number(confidence, 'f', 0);
    const QString fdText = fdArrow + QLatin1Char(' ')
                           + ((dayProjPct >= 0.0) ? QStringLiteral("+") : QString())
                           + fdChange + QStringLiteral("% → ~")
                           + fdTarget + QStringLiteral("  (±")
                           + fdBand + QStringLiteral("%, ")
                           + fdConf + QStringLiteral("% conf)");
    m_sig3d->setText(colored(fdText, fdColor));

    // Overall signal from the same ensemble (needs a clear majority).
    QString signal = QStringLiteral("NEUTRAL");
    QString sigColor = amber;
    qint32 signalDir = 0;  // +1 BUY / -1 SELL / 0 NEUTRAL — feeds the chart arrow
    if (score >= 2) {
        signal = QStringLiteral("BUY");
        sigColor = green;
        signalDir = 1;
    } else if (score <= -2) {
        signal = QStringLiteral("SELL");
        sigColor = red;
        signalDir = -1;
    } else {
        // no clear majority — keep the NEUTRAL defaults
    }
    m_sigOverall->setText(colored(signal, sigColor));
    // Remember the call for the close watchdog (a confident flip against an open
    // position is one of its triggers).
    m_lastSignalDir = signalDir;
    m_lastSignalConf = confidence;

    // Keep the SL/TP defaults tracking volatility while the user hasn't taken
    // over — done before the edge estimate below so it prices the same values.
    proposeSlTpDefaults(vol);

    // --- AI decision support ------------------------------------------------
    // 1) Logistic up-probability: a hand-weighted logistic model over the same
    //    features the indicators expose, squashed through a sigmoid.
    double z = (1.0 * (bull ? 1.0 : -1.0)) + (0.8 * ((hist >= 0.0) ? 1.0 : -1.0))
             + (0.05 * (r - 50.0)) + (0.02 * (50.0 - stochK));
    if (vol > 0.0) {
        z += 0.5 * (momentum / vol);
        if (reg.valid) {
            z += 0.5 * (reg.slopePct / vol);
        }
        if (kn.k > 0) {
            z += 0.4 * (kn.retPct / vol);
        }
    }
    if (sma50 > 0.0) {
        z += 0.6 * (aboveTrend ? 1.0 : -1.0);
    }
    const double pUp = trading::sigmoid(0.5 * z);
    const QString upArrow = (pUp >= 0.55) ? QStringLiteral("▲")
                                          : ((pUp <= 0.45) ? QStringLiteral("▼")
                                                           : QStringLiteral("→"));
    const QString upColor = (pUp >= 0.55) ? green : ((pUp <= 0.45) ? red : amber);
    m_aiUpProb->setText(colored(
        QStringLiteral("%1 %2% up").arg(upArrow).arg(std::lround(pUp * 100.0)), upColor));

    // 2)+3) Bootstrap Monte-Carlo over the 3h horizon, reused for the price
    //       outlook and the take-profit-before-stop-loss edge of the user's setup.
    const double amount = m_amount->value();
    const double leverage = m_leverage->currentText().toDouble();
    const double exposure = amount * leverage;
    const double tp = m_takeProfit->value();
    const double sl = m_stopLoss->value();
    const double tpFrac = (exposure > 0.0) ? (tp / exposure) : 0.0;
    const double slFrac = (exposure > 0.0) ? (sl / exposure) : 0.0;
    const trading::McOutlook mc = trading::monteCarlo(series, m_lastPrice, kHorizonBars, tpFrac, slFrac, 1200);

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

    const bool preferLong = score >= 0;  // evaluate the ensemble's favoured side
    const double pWin = preferLong ? mc.pWinLong : mc.pWinShort;
    // Win-rate needed to break even for this reward:risk.
    const double breakeven = ((tp + sl) > 0.0) ? (sl / (tp + sl)) : 0.0;
    const double edge = pWin - breakeven;
    const bool edgeValid = mc.valid && (tp > 0.0) && (sl > 0.0) && (exposure > 0.0);
    if (edgeValid) {
        const QString side = preferLong ? QStringLiteral("BUY") : QStringLiteral("SELL");
        const QString eColor = (edge > 0.02) ? green : ((edge < -0.02) ? red : amber);
        const QString verdict = (edge > 0.02)
                                    ? QStringLiteral("favourable")
                                    : ((edge < -0.02) ? QStringLiteral("unfavourable")
                                                      : QStringLiteral("marginal"));
        const long edgePct = std::lround(edge * 100.0);
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

    // 4) Market regime from the Hurst exponent.
    const double hurst = trading::hurstExponent(series);
    QString regime = QStringLiteral("Random walk");
    QString regimeColor = amber;
    if (hurst >= 0.55) {
        regime = QStringLiteral("Trending");
        regimeColor = green;
    } else if (hurst <= 0.45) {
        regime = QStringLiteral("Mean-reverting");
        regimeColor = amber;
    } else {
        // in between — keep the "Random walk" default
    }
    m_aiRegime->setText(colored(
        QStringLiteral("%1 (H %2)").arg(regime, QString::number(hurst, 'f', 3)), regimeColor));

    // 5) Explicit BUY / SELL / HOLD call, with a one-line rationale.
    const bool bullishLean = (score >= 2) && (pUp >= 0.50);
    const bool bearishLean = (score <= -2) && (pUp <= 0.50);
    QString advice;
    QString adviceColor = amber;
    qint32 adviceDir = 0;  // +1 BUY / -1 SELL / 0 HOLD — feeds the chart arrow
    if (bullishLean) {
        advice = QStringLiteral("BUY");
        adviceColor = green;
        adviceDir = 1;
    } else if (bearishLean) {
        advice = QStringLiteral("SELL");
        adviceColor = red;
        adviceDir = -1;
    } else {
        advice = QStringLiteral("HOLD — no clear edge, stay flat");
    }
    if (bullishLean || bearishLean) {
        if (hurst >= 0.55) {
            advice += QStringLiteral(" — trend-following favoured");
        } else if (hurst <= 0.45) {
            advice += QStringLiteral(" — choppy, size down / fade extremes");
        } else {
            // random-walk regime — nothing extra to add
        }
        if (edgeValid && (edge < 0.0)) {
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

    // Chart arrow = agreement of the overall Signal and the AI Recommendation:
    // both bullish → ▲, both bearish → ▼, a lean either way → the leaning arrow,
    // and a conflict (BUY vs SELL) or both flat → no arrow.
    m_chart->setPredictionDirection(signalDir + adviceDir);
}

void MainWindow::checkAutoOrders(double price)
{
    if (price <= 0.0) {
        return;
    }

    const double buyBelow = m_buyBelow->value();
    const double sellAbove = m_sellAbove->value();

    if (m_armBuy->isChecked() && (buyBelow > 0.0) && (price < buyBelow)) {
        const QString priceText = QLocale().toString(price, 'f', 2);
        const QString limitText = QLocale().toString(buyBelow, 'f', 2);
        appendLog(QStringLiteral("Auto-order: price %1 < %2 → BUY").arg(priceText, limitText));
        m_armBuy->setChecked(false);  // one-shot: disarm before firing
        placeOrder(true);
    }
    if (m_armSell->isChecked() && (sellAbove > 0.0) && (price > sellAbove)) {
        const QString priceText = QLocale().toString(price, 'f', 2);
        const QString limitText = QLocale().toString(sellAbove, 'f', 2);
        appendLog(QStringLiteral("Auto-order: price %1 > %2 → SELL").arg(priceText, limitText));
        m_armSell->setChecked(false);
        placeOrder(false);
    }
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
    constexpr qint64 kKeepPastSecs = 10 * 60;
    const QDateTime now = QDateTime::currentDateTime();

    // Recompute the soonest still-upcoming event (event-risk term of the prediction)
    // and count what should currently be visible, in one pass.
    m_nextEventTime = QDateTime();
    m_nextEventTitle.clear();
    qint32 visibleCount = 0;
    for (const EconomicEvent &e : m_eventList) {
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
    for (const EconomicEvent &e : m_eventList) {
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

        auto *item = new QListWidgetItem(line, m_events);
        item->setForeground(impactColor(guess.dir));  // colour by predicted direction
        item->setToolTip(eventTooltip(e, guess, sym));
    }
}

void MainWindow::refreshChartEventMarker()
{
    // Mark the chart's event line only while an event is imminent: from 10 minutes
    // before to 5 minutes after it. If several qualify, mark the nearest in time.
    const QDateTime now = QDateTime::currentDateTime();
    const EconomicEvent *active = nullptr;
    qint64 bestAbs = -1;
    for (const EconomicEvent &e : m_eventList) {
        if (!e.when.isValid()) {
            continue;
        }
        const qint64 secs = now.secsTo(e.when);  // >0 upcoming, <0 already passed
        if ((secs <= (10 * 60)) && (secs >= (-5 * 60))) {
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
        QMessageBox::warning(this, QStringLiteral("Order"), message);
    }
}

void MainWindow::onPositionClosed(bool ok, const QString &message)
{
    appendLog(message, !ok);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Close position"), message);
        return;
    }
    // (Re)arm the delayed closed-trade P/L refresh so the just-closed trade shows up
    // in the summary; restarting collapses a burst of closes into a single fetch.
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
                          .arg(mood)                     // %3
                          .arg(arrow)                    // %4
                          .arg((changePct >= 0.0) ? QStringLiteral("+") : QString())  // %5
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

void MainWindow::onMonthlyPnl(const MonthlyPnl &s)
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
    const qint32 pnlRows = static_cast<qint32>(s.perInstrument.size());
    m_pnlTable->setRowCount(pnlRows);
    for (qint32 i = 0; i < pnlRows; ++i) {
        const InstrumentPnl &r = s.perInstrument[i];
        auto *name = new QTableWidgetItem(r.symbol);
        auto *trades = new QTableWidgetItem(QString::number(r.trades));
        trades->setTextAlignment(Qt::AlignCenter);
        auto *net = new QTableWidgetItem(signed2(r.netProfit));
        net->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        net->setForeground(colorFor(r.netProfit));
        auto *fees = new QTableWidgetItem(QLocale().toString(toDisplay(r.fees), 'f', 2));
        fees->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_pnlTable->setItem(i, 0, name);
        m_pnlTable->setItem(i, 1, trades);
        m_pnlTable->setItem(i, 2, net);
        m_pnlTable->setItem(i, 3, fees);
    }

    // Headline summary above the table; the box title names the actual window
    // (the detail dialog can re-fetch with a 7–13 week lookback).
    const QString fromText = s.fromDate.toString(Qt::ISODate);
    const QString toText = s.toDate.toString(Qt::ISODate);
    const qint32 weeks = qRound(static_cast<double>(s.fromDate.daysTo(s.toDate)) / 7.0);
    m_pnlBox->setTitle(QStringLiteral("Closed trades — last %1 weeks (listed instruments)")
                           .arg(weeks));
    if (s.accountTrades == 0) {
        m_pnlSummary->setText(QStringLiteral("<b>No closed trades in this window "
                                             "(%1 → %2).</b>")
                                  .arg(fromText, toText));
        return;
    }
    const QString netColor = colorFor(s.netProfit).name();
    const QString netText = signed2(s.netProfit);
    QString html = QStringLiteral(
                       "<b>Net P/L (listed): <span style='color:%1'>%2 %3</span></b>"
                       " &nbsp;·&nbsp; %4 closed trades &nbsp;·&nbsp; %5 → %6")
                       .arg(netColor, netText, ccy)
                       .arg(s.trades)
                       .arg(fromText, toText);
    if (s.perInstrument.isEmpty()) {
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
    const QString grey = QStringLiteral("#9a9a9a");
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
    constexpr qint64 kLogCooldownMs = 5 * 60 * 1000;  // one log line per trade per 5 min
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QStringList lines;
    for (const Position &p : m_shownPositions) {
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
            if (p.isBuy && (price < lower)) {
                reason = QStringLiteral("price %1 fell out of the prediction corridor (≥ %2)")
                             .arg(QLocale().toString(price, 'f', trading::priceDecimals(price)),
                                  QLocale().toString(lower, 'f', trading::priceDecimals(lower)));
            } else if (!p.isBuy && (price > upper)) {
                reason = QStringLiteral("price %1 rose out of the prediction corridor (≤ %2)")
                             .arg(QLocale().toString(price, 'f', trading::priceDecimals(price)),
                                  QLocale().toString(upper, 'f', trading::priceDecimals(upper)));
            } else {
                // inside the corridor — check the signal flip below
            }
        }
        // Trigger 2 — the ensemble flipped hard against the position.
        if (reason.isEmpty() && (m_lastSignalDir != 0) && (m_lastSignalConf >= 60.0)
            && ((p.isBuy && (m_lastSignalDir < 0)) || (!p.isBuy && (m_lastSignalDir > 0)))) {
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
            m_closeAdviceMs.insert(p.positionId, now);
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
    m_closeAdvice->setText(
        QStringLiteral("<span style='color:#e0b000'><b>⚠ Close proposal — %1:</b></span><br/>%2")
            .arg(sym.toHtmlEscaped(), lines.join(QStringLiteral("<br/>"))));
    m_closeAdvice->setVisible(true);
}

// ---------------------------------------------------------------------------
// Closed-trades detail window
// ---------------------------------------------------------------------------

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
            m_closedWeeks->addItem(QStringLiteral("%1 weeks").arg(w), w);
        }
        m_closedWeeks->setCurrentIndex(m_closedWeeks->count() - 1);  // default: 13 weeks
        top->addWidget(m_closedWeeks);
        m_closedRefresh = new QPushButton(QStringLiteral("Refresh"), m_closedDialog);
        top->addWidget(m_closedRefresh);
        top->addStretch();
        lay->addLayout(top);

        m_closedSummary = new QLabel(QStringLiteral("Loading closed trades…"), m_closedDialog);
        m_closedSummary->setTextFormat(Qt::RichText);
        m_closedSummary->setWordWrap(true);
        lay->addWidget(m_closedSummary);

        m_closedTable = new QTableWidget(0, 11, m_closedDialog);
        m_closedTable->setHorizontalHeaderLabels(
            {QStringLiteral("Closed"), QStringLiteral("Instrument"), QStringLiteral("Side"),
             QStringLiteral("Lev"), QStringLiteral("Invest (%1)").arg(m_ccy),
             QStringLiteral("Open"), QStringLiteral("Close"),
             QStringLiteral("Net P/L (%1)").arg(m_ccy), QStringLiteral("Fees"),
             QStringLiteral("Open cost*"), QStringLiteral("Close cost*")});
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
            "the trade's notional — eToro does not report historical spreads."));
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

void MainWindow::rebuildClosedTradesTable()
{
    if (m_closedTable == nullptr) {
        return;
    }
    const QColor green(0x25, 0xb5, 0x63);
    const QColor red(0xe3, 0x55, 0x55);
    const QColor grey(0x9a, 0x9a, 0x9a);

    double sumNet = 0.0;
    double sumFees = 0.0;
    double sumCosts = 0.0;
    qint32 costRows = 0;

    const qint32 rows = static_cast<qint32>(m_closedTrades.size());
    m_closedTable->setRowCount(rows);
    for (qint32 i = 0; i < rows; ++i) {
        const ClosedTrade &t = m_closedTrades[i];
        auto make = [&t, &grey](const QString &text) {
            auto *it = new QTableWidgetItem(text);
            if (!t.listed) {
                it->setForeground(grey);  // not in the app's selector — context only
            }
            return it;
        };
        auto money = [this](double usd) { return QLocale().toString(toDisplay(usd), 'f', 2); };

        m_closedTable->setItem(i, 0,
            make(t.closeTime.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        m_closedTable->setItem(i, 1, make(t.symbol));
        auto *side = make(t.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
        side->setForeground(t.isBuy ? green : red);
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
        net->setForeground((t.netProfit > 0.0) ? green : ((t.netProfit < 0.0) ? red : grey));
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
        m_closedSummary->setText(
            QStringLiteral("<b>No closed trades in the selected window.</b> "
                           "<span style='color:#9a9a9a'>(simulation mode keeps no "
                           "per-trade history)</span>"));
        return;
    }
    const QString netColor = (sumNet >= 0.0) ? QStringLiteral("#25b563")
                                             : QStringLiteral("#e35555");
    const QString netText = QStringLiteral("%1%2 %3")
                                .arg((sumNet >= 0.0) ? QStringLiteral("+") : QString(),
                                     QLocale().toString(toDisplay(sumNet), 'f', 2), m_ccy);
    m_closedSummary->setText(
        QStringLiteral("<b>%1 trades · net <span style='color:%2'>%3</span> · "
                       "rollover fees %4%5 · est. spread costs %4%6 (%7 trades priced)</b>")
            .arg(rows)
            .arg(netColor, netText, m_ccy,
                 QLocale().toString(toDisplay(sumFees), 'f', 2),
                 QLocale().toString(toDisplay(sumCosts), 'f', 2))
            .arg(costRows));
}

void MainWindow::onLog(const QString &message, bool isError)
{
    appendLog(message, isError);
}

// ---------------------------------------------------------------------------
// Leverage screener
// ---------------------------------------------------------------------------

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
    bool replaced = false;
    for (qsizetype i = 0; i < m_screenerRows.size(); ++i) {
        if (m_screenerRows[i].symbol == row.symbol) {
            m_screenerRows[i] = row;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
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
}

void MainWindow::onInstrumentRatings(const QHash<QString, WebRating> &ratingBySymbol)
{
    m_ratingBySymbol = ratingBySymbol;
    rebuildRecommendations();
    rebuildDecision();
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

    const QColor green(0x25, 0xb5, 0x63);
    const QColor red(0xe3, 0x55, 0x55);

    auto ratingWord = [](double s) -> QString {
        if (s >= 0.5) {
            return QStringLiteral("Strong Buy");
        }
        if (s >= 0.1) {
            return QStringLiteral("Buy");
        }
        if (s > -0.1) {
            return QStringLiteral("Neutral");
        }
        if (s > -0.5) {
            return QStringLiteral("Sell");
        }
        return QStringLiteral("Strong Sell");
    };
    auto ago = [](const QDateTime &t) -> QString {
        if (!t.isValid()) {
            return QString();
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

    for (const ScreenerRow &r : m_screenerRows) {
        if (!r.ok || r.closes.isEmpty()) {
            continue;
        }
        // "Buy / sell now" is actionable advice, so drop instruments whose market is
        // closed right now — you couldn't trade them anyway. (Unknown state = show all,
        // so the panel isn't emptied before the first tradeability check lands.)
        if (m_tradeabilityKnown && !m_tradeableNow.contains(r.symbol)) {
            continue;
        }
        const trading::Ensemble e = trading::computeEnsemble(r.closes, m_vixValid, m_vixChangePct);
        const bool haveRating = m_ratingBySymbol.contains(r.symbol);
        const double rating = haveRating ? m_ratingBySymbol.value(r.symbol).consensus() : 0.0;

        // The call comes from the technical ensemble; a strong web rating can stand in
        // when the ensemble is neutral, and otherwise confirms or tempers it.
        qint32 dir = 0;
        double confidence = 0.0;
        if (e.valid && (e.signalDir != 0)) {
            dir = e.signalDir;
            // Same instrument-agnostic VIX-level haircut the live panel applies.
            confidence = trading::applyVixHaircut(e.confidence, m_vixValid, m_vix);
            if (haveRating) {
                const bool agrees = (rating > 0) == (dir > 0);
                if (agrees && (std::abs(rating) >= 0.1)) {
                    confidence = std::min(100.0, confidence + 8.0);
                } else if (!agrees && (std::abs(rating) >= 0.3)) {
                    confidence *= 0.8;  // the market rating disagrees — temper it
                } else {
                    // weak rating — neither confirms nor tempers
                }
            }
        } else if (haveRating && (std::abs(rating) >= 0.5)) {
            dir = (rating > 0) ? 1 : -1;
            confidence = std::abs(rating) * 100.0;
        } else {
            // no actionable signal from either source
        }
        if (dir == 0) {
            continue;  // nothing actionable for this instrument right now
        }

        const QString side = (dir > 0) ? QStringLiteral("BUY") : QStringLiteral("SELL");
        Reco reco;
        reco.symbol = r.symbol;
        reco.dir = dir;
        reco.confidence = confidence;
        // Side is conveyed by the column and colour, so the row is just symbol + confidence.
        reco.row = QStringLiteral("%1   ·   %2%").arg(r.symbol).arg(qRound(confidence));

        QStringList tip;
        tip << QStringLiteral("%1 — %2 (confidence %3%)").arg(r.symbol, side).arg(qRound(confidence));
        tip << QString();
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
        if (haveRating) {
            tip << QStringLiteral("TradingView 1h rating: %1 (%2%3) — %4")
                       .arg(ratingWord(rating),
                            (rating >= 0.0) ? QStringLiteral("+") : QString())
                       .arg(rating, 0, 'f', 2)
                       .arg(((rating > 0) == (dir > 0)) ? QStringLiteral("confirms")
                                                        : QStringLiteral("disagrees"));
        } else {
            tip << QStringLiteral("TradingView rating: n/a for this instrument");
        }
        const QList<NewsHeadline> news = m_newsBySymbol.value(r.symbol);
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

    // Strongest signal first within each column.
    const auto sortBegin = recos.begin();
    const auto sortEnd = recos.end();
    std::sort(sortBegin, sortEnd,
              [](const Reco &a, const Reco &b) { return a.confidence > b.confidence; });

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
// TradingView rating bucket for a score in [-1, 1].
QString decisionRatingWord(double s)
{
    if (s >= 0.5) {
        return QStringLiteral("Strong Buy");
    }
    if (s >= 0.1) {
        return QStringLiteral("Buy");
    }
    if (s > -0.1) {
        return QStringLiteral("Neutral");
    }
    if (s > -0.5) {
        return QStringLiteral("Sell");
    }
    return QStringLiteral("Strong Sell");
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
    m.vixValid = m_vixValid;
    m.vix = m_vix;
    m.vixChangePct = m_vixChangePct;
    m.events = m_eventList;
    m.fgValid = m_fgValid;
    m.fearGreed = m_fg;
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
        m_decisionSources->setMaximumHeight(210);  // six source rows
        lay->addWidget(m_decisionSources);

        lay->addWidget(new QLabel(QStringLiteral("All instruments, ranked by composite:"),
                                  m_decisionDialog));
        m_decisionRanked = new QTableWidget(0, 4, m_decisionDialog);
        m_decisionRanked->setHorizontalHeaderLabels(
            {QStringLiteral("Instrument"), QStringLiteral("Call"), QStringLiteral("Composite"),
             QStringLiteral("Confidence")});
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
                const QString sym = m_decisionRanked->item(sel.first()->row(), 0)
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
                QTableWidgetItem *it = m_decisionRanked->item(row, 0);
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

    const QColor green(0x25, 0xb5, 0x63);
    const QColor red(0xe3, 0x55, 0x55);
    const QColor grey(0x9a, 0x9a, 0x9a);
    auto callColour = [&green, &red, &grey](qint32 dir) {
        return (dir > 0) ? green : ((dir < 0) ? red : grey);
    };
    auto callWord = [](qint32 dir) {
        return (dir > 0) ? QStringLiteral("BUY")
                         : ((dir < 0) ? QStringLiteral("SELL") : QStringLiteral("—"));
    };

    const QList<trading::DecisionRow> rows = trading::computeDecisionRows(marketSnapshot());

    // Fill the ranked table (all instruments), guarded so the programmatic refill isn't
    // mistaken for a user selection.
    m_decisionUpdatingRanked = true;
    const qint32 rowCount = static_cast<qint32>(rows.size());
    m_decisionRanked->setRowCount(rowCount);
    for (qint32 i = 0; i < rowCount; ++i) {
        const trading::DecisionRow &d = rows[i];
        auto *sym = new QTableWidgetItem(d.symbol);
        sym->setData(Qt::UserRole, d.symbol);
        auto *call = new QTableWidgetItem(callWord(d.dir));
        call->setForeground(callColour(d.dir));
        call->setTextAlignment(Qt::AlignCenter);
        auto *comp = new QTableWidgetItem(QStringLiteral("%1").arg(d.composite, 0, 'f', 2));
        comp->setTextAlignment(Qt::AlignCenter);
        auto *conf = new QTableWidgetItem(QStringLiteral("%1%").arg(qRound(d.confidence)));
        conf->setTextAlignment(Qt::AlignCenter);
        m_decisionRanked->setItem(i, 0, sym);
        m_decisionRanked->setItem(i, 1, call);
        m_decisionRanked->setItem(i, 2, comp);
        m_decisionRanked->setItem(i, 3, conf);
    }

    // Resolve the focus instrument: the user's manual pick if it's still listed, else
    // the recommendation (Claude's actionable pick, else the top composite).
    auto rowFor = [&rows](const QString &s) -> const trading::DecisionRow * {
        for (const trading::DecisionRow &d : rows) {
            if (d.symbol == s) {
                return &d;
            }
        }
        return nullptr;
    };
    const bool aiActionable = m_aiDecision.ok
                              && (m_aiDecision.action.compare(QStringLiteral("HOLD"),
                                                              Qt::CaseInsensitive) != 0);
    const trading::DecisionRow *topRow = nullptr;
    for (const trading::DecisionRow &d : rows) {
        if (d.dir != 0) {
            topRow = &d;
            break;
        }
    }
    QString focus;
    if (!m_decisionSelected.isEmpty() && (rowFor(m_decisionSelected) != nullptr)) {
        focus = m_decisionSelected;
    } else if (aiActionable && (rowFor(m_aiDecision.symbol) != nullptr)) {
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

    const QColor green(0x25, 0xb5, 0x63);
    const QColor red(0xe3, 0x55, 0x55);
    const QColor grey(0x9a, 0x9a, 0x9a);
    auto callColour = [&green, &red, &grey](qint32 dir) {
        return (dir > 0) ? green : ((dir < 0) ? red : grey);
    };
    auto callWord = [](qint32 dir) {
        return (dir > 0) ? QStringLiteral("BUY")
                         : ((dir < 0) ? QStringLiteral("SELL") : QStringLiteral("—"));
    };

    const trading::DecisionRow *focus = nullptr;
    for (const trading::DecisionRow &d : rows) {
        if (d.symbol == focusSymbol) {
            focus = &d;
            break;
        }
    }
    const bool manual = !m_decisionSelected.isEmpty() && (focusSymbol == m_decisionSelected);

    if (m_decisionSourcesLabel != nullptr) {
        m_decisionSourcesLabel->setText(
            (focus != nullptr)
                ? QStringLiteral("Sources for %1%2:")
                      .arg(focusSymbol, manual ? QStringLiteral(" (selected)") : QString())
                : QStringLiteral("Sources:"));
    }

    auto setSrc = [this](qint32 row, const QString &name, const QString &read, const QColor &c,
                         const QString &conf, const QString &note) {
        auto make = [](const QString &t) {
            auto *it = new QTableWidgetItem(t);
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            return it;
        };
        auto *r = make(read);
        if (c.isValid()) {
            r->setForeground(c);
        }
        m_decisionSources->setItem(row, 0, make(name));
        m_decisionSources->setItem(row, 1, r);
        m_decisionSources->setItem(row, 2, make(conf));
        m_decisionSources->setItem(row, 3, make(note));
    };

    m_decisionSources->setRowCount(6);
    if (focus != nullptr) {
        const QColor techColor = callColour(focus->techDir);
        const QString techConf =
            focus->haveTech ? QStringLiteral("%1%").arg(qRound(focus->techConf)) : QString();
        setSrc(0, QStringLiteral("Technical ensemble"),
               focus->haveTech ? focus->techLabel : QStringLiteral("n/a"), techColor, techConf,
               QStringLiteral("indicator blend"));
        const qint32 ratingDir =
            (focus->rating > 0) ? 1 : ((focus->rating < 0) ? -1 : 0);
        const QString ratingRead =
            focus->haveRating ? decisionRatingWord(focus->rating) : QStringLiteral("n/a");
        const QColor ratingColor = focus->haveRating ? callColour(ratingDir) : grey;
        const QString ratingConf =
            focus->haveRating ? QStringLiteral("%1").arg(focus->rating, 0, 'f', 2) : QString();
        setSrc(1, QStringLiteral("TradingView rating"), ratingRead, ratingColor, ratingConf,
               QStringLiteral("15m / 1h / 1D consensus"));
        const qint32 newsDir =
            (focus->newsScore > 0.1) ? 1 : ((focus->newsScore < -0.1) ? -1 : 0);
        const QColor newsColor = focus->haveNews ? callColour(newsDir) : grey;
        const QString newsConf =
            focus->haveNews ? QStringLiteral("%1").arg(focus->newsScore, 0, 'f', 2) : QString();
        const QString newsNote = QStringLiteral("%1 headlines").arg(focus->newsCount);
        setSrc(2, QStringLiteral("News sentiment"),
               focus->haveNews ? ((newsDir > 0)
                                      ? QStringLiteral("positive")
                                      : ((newsDir < 0) ? QStringLiteral("negative")
                                                       : QStringLiteral("neutral")))
                               : QStringLiteral("n/a"),
               newsColor, newsConf, newsNote);
        const qint32 regimeDir =
            (focus->regime > 0.05) ? 1 : ((focus->regime < -0.05) ? -1 : 0);
        const QColor regimeColor = callColour(regimeDir);
        const QString regimeConf =
            m_vixValid ? QStringLiteral("VIX %1").arg(m_vix, 0, 'f', 1) : QString();
        setSrc(3, QStringLiteral("VIX / calendar regime"),
               m_vixValid ? ((m_vix >= 25.0)
                                 ? QStringLiteral("risk-off")
                                 : ((m_vix < 16.0) ? QStringLiteral("risk-on")
                                                   : QStringLiteral("neutral")))
                          : QStringLiteral("n/a"),
               regimeColor, regimeConf,
               focus->eventRisk ? QStringLiteral("⚠ high-impact event <6h") : QStringLiteral("—"));
        // Crowd sentiment: what the trading crowd is doing right now (CNN F&G).
        const qint32 crowdDir =
            (focus->crowd > 0.1) ? 1 : ((focus->crowd < -0.1) ? -1 : 0);
        setSrc(4, QStringLiteral("Crowd (Fear & Greed)"),
               focus->haveCrowd
                   ? QStringLiteral("%1/100 %2").arg(qRound(m_fg)).arg(m_fgRating)
                   : QStringLiteral("n/a"),
               focus->haveCrowd ? callColour(crowdDir) : grey,
               focus->haveCrowd ? QStringLiteral("%1").arg(focus->crowd, 0, 'f', 2)
                                : QString(),
               QStringLiteral("extremes read contrarian"));
    } else {
        for (qint32 r = 0; r < 5; ++r) {
            setSrc(r, (r == 0) ? QStringLiteral("Technical ensemble")
                      : ((r == 1) ? QStringLiteral("TradingView rating")
                         : ((r == 2) ? QStringLiteral("News sentiment")
                            : ((r == 3) ? QStringLiteral("VIX / calendar regime")
                                        : QStringLiteral("Crowd (Fear & Greed)")))),
                   QStringLiteral("—"), grey, QString(),
                   m_screenerRows.isEmpty() ? QStringLiteral("scanning…") : QString());
        }
    }
    // Claude (AI) row — always its own pick, for reference.
    const bool aiActionable = m_aiDecision.ok
                              && (m_aiDecision.action.compare(QStringLiteral("HOLD"),
                                                              Qt::CaseInsensitive) != 0);
    const bool aiBuy =
        m_aiDecision.action.compare(QStringLiteral("BUY"), Qt::CaseInsensitive) == 0;
    const QString aiRead =
        m_aiDecision.ok ? QStringLiteral("%1 %2").arg(m_aiDecision.action, m_aiDecision.symbol)
                        : QStringLiteral("n/a");
    const QColor aiColor = aiActionable ? callColour(aiBuy ? 1 : -1) : grey;
    const QString aiConf =
        m_aiDecision.ok ? QStringLiteral("%1%").arg(qRound(m_aiDecision.confidence)) : QString();
    const QString aiNote =
        m_aiAdvisor->isConfigured() ? (m_aiDecision.ok ? QStringLiteral("synthesised")
                                                       : QStringLiteral("pending / n/a"))
                                    : QStringLiteral("set anthropicApiKey to enable");
    setSrc(5, QStringLiteral("Claude (AI)"), aiRead, aiColor, aiConf, aiNote);

    // Build the costed plan first: the headline's suggested leverage below quotes
    // the plan's volatility-targeted recommendation, so the two never disagree.
    renderTradePlan(focus, focusSymbol);

    // --- headline conclusion (follows the focus instrument) ---
    QString html;
    if (focus == nullptr) {
        html = m_screenerRows.isEmpty()
                   ? QStringLiteral("<b>Scanning instruments…</b>")
                   : QStringLiteral("<b>No actionable trade right now — HOLD.</b>");
    } else {
        const qint32 dir = focus->dir;
        const qint32 maxLev = focus->maxLev;
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
        // Overall recommendation basis, for context.
        const trading::DecisionRow *topRow = nullptr;
        for (const trading::DecisionRow &d : rows) {
            if (d.dir != 0) {
                topRow = &d;
                break;
            }
        }
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
        if (!basis.isEmpty()) {
            html += QStringLiteral("<div style='color:#777'>%1</div>")
                        .arg(basis.join(QStringLiteral(" · ")));
        }
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
    const ScreenerRow *row = nullptr;
    for (const ScreenerRow &r : m_screenerRows) {
        if (r.symbol == focusSymbol) {
            row = &r;
            break;
        }
    }
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
    in.horizonHours = 24;
    in.now = QDateTime::currentDateTime();
    in.vixValid = m_vixValid;
    in.vix = m_vix;
    in.vixChangePct = m_vixChangePct;
    in.eventRisk = focus->eventRisk;
    in.fgValid = m_fgValid;
    in.fearGreed = m_fg;

    const trading::TradePlan plan = trading::buildTradePlan(in);
    if (!plan.valid) {
        m_decisionPlanLabel->setText(QString());
        return;
    }
    m_lastPlan = plan;
    m_lastPlanSymbol = focusSymbol;

    const QString green = QStringLiteral("#25b563");
    const QString red = QStringLiteral("#e35555");
    const QString amber = QStringLiteral("#e0b000");
    const QString grey = QStringLiteral("#9a9a9a");
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
    html += QStringLiteral(
                "<div>Stake %1 · recommended leverage <b>x%2</b> "
                "<span style='color:%3'>(±%4% of stake per hour)</span> · "
                "SL <b>%5</b> @ %6 · TP <b>%7</b> @ %8</div>")
                .arg(eur(m_amount->value()))
                .arg(plan.leverage)
                .arg(grey)
                .arg(plan.marginSwingPct, 0, 'f', 1)
                .arg(eur(plan.slAmount), rate(plan.slRate), eur(plan.tpAmount),
                     rate(plan.tpRate));

    // Cost bill: spread both ways, overnight, weekend — netted against the edge.
    QString costLine = QStringLiteral("open %1 + close %2")
                           .arg(eur(plan.openCost), eur(plan.closeCost));
    if (plan.feePerNight != 0.0) {
        costLine += QStringLiteral(" + %1/night").arg(eur(plan.feePerNight));
    }
    if (plan.crossesWeekend) {
        costLine += QStringLiteral(" + weekend %1").arg(eur(plan.weekendFee));
    }
    const QString netColor = (plan.expectedNet > 0.0) ? green : red;
    html += QStringLiteral(
                "<div>Costs (%1 night%2): %3 = <b>%4</b>%5 · expected edge after costs: "
                "<span style='color:%6'><b>%7%8</b></span></div>")
                .arg(plan.nights)
                .arg((plan.nights == 1) ? QString() : QStringLiteral("s"))
                .arg(costLine, eur(plan.expectedCosts))
                .arg(plan.costsComplete
                         ? QString()
                         : QStringLiteral(" <span style='color:%1'>(partial — select the "
                                          "instrument for live spread/fees)</span>")
                               .arg(amber))
                .arg(netColor,
                     (plan.expectedNet >= 0.0) ? QStringLiteral("+") : QString(),
                     QLocale().toString(plan.expectedNet, 'f', 2));
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
    m_autoDefaultsSet = false;  // reseed auto-order thresholds for the new instrument
    m_pnlAnchorPrice = 0.0;     // drop the old instrument's P/L anchor until the next poll
    m_fees = InstrumentFees{};  // the old instrument's rollover fees no longer apply
    m_slTpAuto = true;          // volatility-proposed SL/TP resume for the new instrument
    m_forecastTarget = 0.0;     // old corridor no longer applies (watchdog stands down)
    m_lastSignalDir = 0;
    m_webQuotePrice = 0.0;      // old instrument's reference quote no longer applies
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

void MainWindow::handleOrderButton(bool isBuy)
{
    // Require a deliberate double-press: a single click only arms the button and
    // prompts; the order fires only if the SAME button is pressed again in time.
    constexpr qint64 kDoublePressMs = 650;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if ((m_orderClickMs != 0) && (isBuy == m_orderClickBuy)
        && ((now - m_orderClickMs) <= kDoublePressMs)) {
        m_orderClickMs = 0;  // consumed — the next order needs two fresh presses
        placeOrder(isBuy);
    } else {
        m_orderClickMs = now;
        m_orderClickBuy = isBuy;
        const double amountEur = m_amount->value();
        // Show the euro order and the USD amount eToro will actually receive, so the
        // conversion is visible before the confirming second press.
        const QString sizes = (m_eurPerUsd > 0.0)
                                  ? QStringLiteral("%1%2 (≈ $%3)")
                                        .arg(m_ccy)
                                        .arg(amountEur, 0, 'f', 2)
                                        .arg(fromDisplay(amountEur), 0, 'f', 2)
                                  : QStringLiteral("%1%2").arg(m_ccy).arg(amountEur, 0, 'f', 2);
        appendLog(QStringLiteral("Press %1 again within 650 ms to place %2.")
                      .arg(isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"), sizes));
    }
}

void MainWindow::placeOrder(bool isBuy)
{
    const double leverage = m_leverage->currentText().toDouble();
    const bool trailingStop = m_trailingStop->isChecked();
    const QString side = isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");

    // Guard 0 — market closed: the buttons are already disabled then, but the keyboard
    // double-tap and armed auto-orders reach placeOrder() directly, so block here too.
    const QString sym = m_client->config().symbol;
    if (m_tradeabilityKnown && !m_tradeableNow.contains(sym)) {
        appendLog(side + QStringLiteral(" blocked — %1's market is closed; eToro isn't "
                                        "accepting opening orders right now.").arg(sym),
                  true);
        return;
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
    if ((m_openTradesTotal + amount) > (kMaxOpenExposure + 1e-6)) {
        const QString msg =
            QStringLiteral("%1 blocked — open trades would reach %2%3, over the %2%4 limit "
                           "(%2%5 currently open).")
                .arg(side, m_ccy)
                .arg(toDisplay(m_openTradesTotal + amount), 0, 'f', 2)
                .arg(toDisplay(kMaxOpenExposure), 0, 'f', 2)
                .arg(toDisplay(m_openTradesTotal), 0, 'f', 2);
        appendLog(msg, true);
        QMessageBox::warning(this, QStringLiteral("Open-trades limit"), msg);
        return;
    }

    // No modal confirmation (the buttons' double-press is the gate) — but log the
    // euro order AND the exact USD amount eToro will receive, so the conversion is
    // visible before it goes out.
    appendLog(QStringLiteral("Submitting %1 order: %2%3 (≈ $%4) (x%5), SL %2%6 / TP %2%7…")
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
    // the real-mode case moments later.
    m_openTradesTotal += amount;
    m_buyButton->setEnabled(false);
    m_sellButton->setEnabled(false);
    m_orderCooldownTimer->start();

    m_client->openPosition(isBuy, amount, leverage, stopLoss, takeProfit, trailingStop);
}

void MainWindow::onCloseClicked()
{
    const QStringList ids = markedPositionIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Close trades"),
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
    QStringList ids;
    for (qint32 row = 0; row < m_positions->rowCount(); ++row) {
        QTableWidgetItem *item = m_positions->item(row, 0);
        if ((item != nullptr) && (item->checkState() == Qt::Checked)) {
            ids << item->data(Qt::UserRole).toString();
        }
    }
    return ids;
}

void MainWindow::appendLog(const QString &message, bool isError)
{
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    const QString prefix = isError ? QStringLiteral("[!]") : QStringLiteral("   ");
    m_log->appendPlainText(QStringLiteral("%1 %2 %3").arg(ts, prefix, message));
}
