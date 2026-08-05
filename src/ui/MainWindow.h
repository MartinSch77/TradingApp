#ifndef TRADINGAPP_MAINWINDOW_H
#define TRADINGAPP_MAINWINDOW_H

#include "domain/DecisionEngine.h"
#include "domain/Forecasting.h"
#include "domain/Models.h"
#include "domain/IndexConfluence.h"
#include "domain/PaperTrader.h"
#include "domain/TradePlan.h"
#include <QDateTime>
#include <QFont>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QSize>
#include <QStringList>

class AiAdvisor;
class BotSimDialog;
class BotSimRunner;
class EconomicCalendar;
class EtoroClient;
class MarketFeeds;
class OllamaAdvisor;
class PositionsModel;
class PriceChart;
class ScreenerDialog;
class TradeGaugeDialog;
class TradeScriptDialog;
class TradeScriptRunner;
QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QCloseEvent)
QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QDialog)
QT_FORWARD_DECLARE_CLASS(QEvent)
QT_FORWARD_DECLARE_CLASS(QDoubleSpinBox)
QT_FORWARD_DECLARE_CLASS(QFormLayout)
QT_FORWARD_DECLARE_CLASS(QGroupBox)
QT_FORWARD_DECLARE_CLASS(QHBoxLayout)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QMessageBox)
QT_FORWARD_DECLARE_CLASS(QPlainTextEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTableWidget)
QT_FORWARD_DECLARE_CLASS(QTableView)
QT_FORWARD_DECLARE_CLASS(QTableWidgetItem)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    // The window depends on narrow service interfaces, injected by the
    // composition root (main.cpp): the broker client, the public web feeds, the
    // AI advisor and the economic calendar are separate objects with separate
    // lifecycles — the UI consumes their signals and owns none of them.
    explicit MainWindow(EtoroClient *client, MarketFeeds *feeds, AiAdvisor *aiAdvisor,
                        EconomicCalendar *calendar, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    // Double-tap 's' = Sell, 'b' = Buy (application-wide, ignored while typing).
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onReady(const Instrument &instrument);
    void onHistory(const QList<Candle> &candles);
    void onPrice(const QDateTime &time, double price);
    void onPortfolio(const QList<Position> &positions);
    void onCash(double available, const QString &currency);
    void onFxRate(double eurPerUsd);  // EUR-per-USD update → switch displays to euro
    void onEvents(const QList<EconomicEvent> &events);
    void onOrderResult(bool ok, const QString &message);
    // The limit orders currently resting at eToro (REQ-F-027) — the panel's table and
    // the exposure check are pure views of this.
    void onPendingOrders(const QList<PendingOrder> &orders);
    void onPositionClosed(bool ok, const QString &message);
    void onVix(double level, double changePct);  // CBOE VIX ("fear index") update
    void onExternalSignal(bool available, double score, const QString &rating);  // web rating
    void onFearGreed(double score, const QString &rating);  // crowd sentiment (CNN F&G)
    // Independent Yahoo reference quote for the current instrument — freshness and
    // level cross-check on the eToro rate.
    void onWebQuote(const QString &symbol, double price, const QDateTime &asOf);
    void onMonthlyPnl(const MonthlyPnl &summary);
    void onMonthlyPnlFailed(const QString &error);
    void onClosedTrades(const QList<ClosedTrade> &trades);  // detail list behind the summary
    void openClosedTrades();   // the closed-trades detail window (lazily built)
    void onLog(const QString &message, bool isError);
    void onLeverageOptions(const QList<int> &values);  // repopulate + select the max
    // The set of app symbols whose market is currently open; drives BUY/SELL enablement.
    void onTradeability(const QSet<QString> &tradeableSymbols);

    // Leverage screener (all instruments with a buy/sell signal from the same
    // ensemble as the live panel, ranked BUY-first then by confidence).
    void openScreener();
    // Trade-script window (REQ-F-028): load / dry-run / arm a script of
    // conditional orders. The RUNNER lives on regardless of the window.
    void openScript();
    // Trading-bot simulation window (REQ-F-029): the paper account, its open and
    // closed simulated trades and the bot's decision log. The runner keeps its
    // books with the window closed, so an experiment can run for days.
    void openBotSim();
    // "Trading-Bot opened a trade" — raised from the runner's signal, so it shows
    // whether or not the bot window is open.
    void onBotTradeOpened(const QString &symbol);
    // The local model's picks, shown as a SOURCE in the signals panel and the
    // decision window (REQ-F-034) — separately from what the bot does with them.
    void onLocalModelProposals(const QList<trading::AiProposal> &picks);
    void updateLocalModelSignal();
    // The local model's hold/close read, pushed into the REAL open-trades table as
    // advice (REQ-F-034).
    void pushAiOpinionsToPositions();
    [[nodiscard]] trading::AiProposal localPickFor(const QString &symbol) const;

private:
    // Construct + gate the two autonomous runners — the trade script (REQ-F-028)
    // and the paper-trading bot simulation (REQ-F-029). Kept out of the
    // constructor so the ctor stays within its metrics baseline, like
    // connectWorkerResults.
    void setupRunners();

private slots:
    void onScreenerRow(const ScreenerRow &row);
    void onScreenerProgress(int done, int total);
    void onScreenerFinished();

    // "Buy / sell now" panel: web rating per instrument, and recent news headlines
    // that feed the recommendation reasoning shown on hover.
    void onInstrumentRatings(const QHash<QString, WebRating> &ratingBySymbol);
    void onInstrumentNews(const QString &symbol, const QList<NewsHeadline> &headlines);
    // One reference ticker's session series (^VIX, ^VXN, ^TNX, or a heavyweight of
    // either index).
    void connectInstrumentFeeds();
    void buildDecisionSourcesTable(QVBoxLayout *lay);
    void onReferenceSeries(const QString &ticker, const QList<double> &closes);
    void updateConfluenceSignal();

    // Decision window: a separate window listing each source's independent read and a
    // final AI+algorithmic conclusion on which instrument to trade, and how, now.
    void openDecision();
    void onAiDecision(const AiDecision &decision);

    void onBuyClicked();
    void onSellClicked();
    void onCloseClicked();
    // A stop-loss / take-profit cell editor was committed (PositionsModel::
    // slTpEdited): convert the currency amount to a rate and push the change.
    void onPositionSlTpEdited(qint32 row, qint32 column, const QString &text);

private:
    // Account figures come from the API in USD (the account currency); the UI shows
    // euro. toDisplay converts a USD amount to the shown euro value; fromDisplay maps
    // a user-entered euro amount back to USD for the API. Both are identity until the
    // first EURUSD rate arrives (m_eurPerUsd == 0), so nothing shows a bogus number.
    [[nodiscard]] double toDisplay(double usd) const
    {
        return (m_eurPerUsd > 0.0) ? (usd * m_eurPerUsd) : usd;
    }
    [[nodiscard]] double fromDisplay(double disp) const
    {
        return (m_eurPerUsd > 0.0) ? (disp / m_eurPerUsd) : disp;
    }
    void buildUi();
    // Switch the traded instrument: sync the selector, reset per-instrument UI state
    // and ask the client to load it. Used by the selector and the startup auto-load.
    void selectInstrument(const QString &sym);
    // triggerRate = 0 sends a market order (executes now); triggerRate > 0 sends a
    // limit order that eToro holds until the rate is reached (REQ-F-027).
    void placeOrder(bool isBuy, double triggerRate = 0.0);
    // Buy/Sell button gate: an order is placed only on a second press of the SAME
    // button within 650 ms; a single press just arms and prompts for confirmation.
    // `limit` distinguishes the market buttons from the limit-order ones, so a press
    // on one never confirms the other.
    void handleOrderButton(bool isBuy, bool limit = false);
    // Submit the limit order the panel describes (rate from the buy/sell rate field).
    void placeLimitOrder(bool isBuy);
    void cancelSelectedPendingOrder();  // cancel the limit order marked in the table
    // Modal editor for the resting order in `row`: trigger rate + SL/TP amounts. On OK
    // the client cancels and re-places the order (eToro has no update-order endpoint),
    // which the dialog says out loud. row < 0 = nothing selected.
    void openPendingOrderEditor(qint32 row);
    void rebuildPendingOrdersTable();   // (re)fill the resting-orders view from m_pendingShown
    // Make the resting order in `row` the traded instrument (clicking its Instr. cell, as
    // in the open-trades table). This is the route to adjusting an order on another
    // instrument, since the re-placement is priced from the selected one.
    void switchToPendingOrderInstrument(qint32 row);
    // Refresh only the "Now" column of the resting-order table (each order's own
    // instrument rate, plus a mouse-over with the distance still to travel). Runs on every
    // price tick, so it edits the cells in place instead of rebuilding the table.
    void updatePendingOrderRates();
    // Row of the marked resting order, or -1: the mark is held as an ORDER ID, because the
    // 4 s refresh rebuilds the rows and an order that resolved shifts the ones below it.
    [[nodiscard]] qint32 selectedPendingRow() const;
    // Account-currency total the resting limit orders will tie up once they trigger;
    // counted against the open-exposure cap alongside the already-open trades.
    [[nodiscard]] double pendingExposureTotal() const;
    void appendLog(const QString &message, bool isError = false);
    [[nodiscard]] QStringList markedPositionIds() const;  // position ids of ticked open-trade rows
    // A trade missing from the newest portfolio snapshot has been closed (by this
    // app, by eToro's UI, by an SL/TP hit or by a liquidation): log it and refresh
    // the closed-trades list at once. Compares against m_shownPositions, so call
    // it BEFORE that is replaced.
    void refreshClosedTradesForVanished(const QList<Position> &current);
    // eventFilter()'s Ctrl+wheel UI-zoom leg; true = the wheel event was consumed.
    [[nodiscard]] bool handleZoomWheel(QObject *watched, QEvent *event);
    // eventFilter()'s double-tap s/b leg; true = the key press was consumed.
    [[nodiscard]] bool handleQuickKeyEvent(QEvent *event);
    void handleQuickKey(qint32 key);        // double-tap detection for the s/b shortcut
    void updateSignals();               // recompute the SPX500 technical signals panel
    // Shared values of one updateSignals() pass, threaded by reference through the
    // render helpers below — TradePlan.cpp's PlanContext pattern. The struct is
    // file-local: defined only in MainWindow.cpp, right above updateSignals().
    struct SignalsContext;
    void renderRegimeAndNewsRows();     // VIX/calendar regime + news-sentiment rows
    void renderGatheringDataRows();     // reset every signal row while history fills
    void renderIndicatorRows(SignalsContext &ctx);      // trend/RSI/MACD/Bollinger/volatility rows
    void renderForecastModelRows(SignalsContext &ctx);  // regression + pattern-kNN rows
    void renderTimingAndRiskRows(SignalsContext &ctx);  // stochastic, SMA-50, risk, change rows
    void renderPredictionRow(SignalsContext &ctx);      // ensemble vote + confidence haircuts
    void render3hForecastRow(const SignalsContext &ctx);  // drift extrapolation + watchdog corridor
    void render3dForecastRow(SignalsContext &ctx);      // bounded 3-trading-day projection
    void renderOverallSignalRow(SignalsContext &ctx);   // BUY/SELL/NEUTRAL majority call
    void renderUpProbabilityRow(SignalsContext &ctx);   // logistic up-probability row
    void dispatchMonteCarlo(const SignalsContext &ctx); // coalesced off-GUI-thread MC run
    void renderAiRegimeRow(SignalsContext &ctx);        // Hurst trending/mean-reverting row
    void renderAdviceRow(SignalsContext &ctx);          // synthesized BUY/SELL/HOLD line
    void updateOpenCost();              // refresh the estimated opening (spread) cost
    // Enable/disable BUY & SELL (and the "market closed" hint) from whether the selected
    // instrument's market is currently open. Respects the order cooldown and treats an
    // unknown tradeability state (before the first eligibility check) as "open".
    void updateTradeButtonsEnabled();
    // True when the selected instrument's market reads closed but the user armed the
    // "Trade anyway" override — orders then go through and are logged as overridden.
    [[nodiscard]] bool marketClosedOverridden() const;
    // The "market closed" warning and its "Trade anyway" override as one row (REQ-F-026),
    // built here rather than inline in buildUi() to keep that function off the metrics
    // ratchet. updateTradeButtonsEnabled() shows and hides the row as a unit.
    [[nodiscard]] QWidget *buildMarketClosedRow(QWidget *parent);
    // The limit-order panel (rate per side, submit buttons, resting-order table with
    // its cancel button) — also built out of line to keep buildUi() off that ratchet.
    [[nodiscard]] QGroupBox *buildLimitOrderBox(QWidget *parent);
    // The resting-order table plus its Adjust/Cancel buttons, appended to `form`.
    void buildPendingOrdersView(QWidget *parent, QFormLayout *form);
    // buildUi() is an orchestrator; the per-panel builders below (same ratchet
    // reasoning as buildMarketClosedRow) run in its exact original order.
    // Header row: title, instrument selector, header buttons, price/cash column.
    [[nodiscard]] QHBoxLayout *buildHeaderRow(QWidget *central, const QString &sym);
    // The header's window/dialog buttons (Graph … Closed trades…).
    void buildHeaderButtons(QWidget *central);
    // The parentless, stay-on-top price/time chart window (shown by the constructor).
    void buildChartWindow(const QString &sym, qint32 chartW, qint32 chartH);
    // "Trade <symbol>" panel: hours, amount/leverage/SL/TP fields, BUY/SELL buttons.
    [[nodiscard]] QGroupBox *buildTradePanel(QWidget *lower, const QString &sym);
    // The opening-cost and overnight-fee rows at the bottom of the trade panel.
    void buildTradeCostRows(QGroupBox *tradeBox, QFormLayout *tradeForm);
    // The floating stay-on-top window holding the signals AND the AI panel.
    void buildSignalsWindow(const QString &sym);
    // The labelled rows of the signals panel, each with its explanatory mouse-over.
    void buildSignalRows(QGroupBox *sigBox, QFormLayout *sigForm);
    void buildStatisticalSignalRows(QGroupBox *sigBox, QFormLayout *sigForm);
    // The AI decision-support panel, sharing the floating signals window.
    void buildAiPanel(QVBoxLayout *signalsWinLayout);
    // "Open trades" panel: positions view, totals, close-proposal banner, Close button.
    [[nodiscard]] QGroupBox *buildPositionsPanel(QWidget *lower);
    // Closed-trade monthly P/L summary panel (m_pnlBox and its buttons).
    void buildMonthlyPnlPanel(QWidget *lower);
    // Bottom splitter row: market events | Activity log | Buy / sell now.
    void buildBottomRow(QWidget *lower, QVBoxLayout *lowerLayout, const QString &sym);
    // "Buy / sell now" panel: BUY and SELL recommendation columns plus Refresh.
    void buildRecommendationsPanel(QWidget *lower);
    void onMarketClosedOverrideToggled(bool on);  // log the change, re-gate the buttons
    // Wire the results of everything computed off the GUI thread (AI advisor + the three
    // QtConcurrent watchers). Called from the constructor; separate so its connection
    // list stays within the metrics budget.
    void connectWorkerResults();
    // Live-refresh the open-trades P/L cells from the client's quote book, without
    // waiting for the next portfolio poll. EVERY row moves, each marked from its own
    // instrument's current quote — the way eToro marks it — so a trade on an instrument
    // other than the one on screen is not left behind.
    void updateOpenTradePnl();
    // The invested / P-L totals under the open-trades table. Called from the same places
    // as the re-price, so the summary can never state a different figure than the rows.
    void updateOpenTradesSummary();
    // Ctrl + mouse-wheel UI zoom: scale both windows' size and every font from a
    // captured baseline. setUiScale clamps and stores the factor; applyUiScale does
    // the work. Over the chart, Ctrl+wheel keeps zooming its time axis instead.
    void setUiScale(double scale);
    void applyUiScale();
    void startScreenerScan();           // clear the screener and kick off a fresh scan
    // "Buy / sell now": trigger a fresh scan + web rating + news fetch, and (re)build
    // the actionable-instrument list with its per-row reasoning tooltips.
    void startRecommendationScan();
    void rebuildRecommendations();

    // Decision window helpers. The weighted multi-source blend itself lives in the
    // domain layer (trading::computeDecisionRows and friends); the UI only captures
    // its latest feeds as the engine's input snapshot and renders the result.
    [[nodiscard]] trading::MarketSnapshot marketSnapshot() const;
    void startDecisionScan();     // (re)scan + rating + news, then ask Claude if configured
    void rebuildDecision();       // (re)fill the decision window from the latest data
    // Render the decision window's sources table + conclusion for one "focus" instrument
    // (the row the user selected in the ranked list, or the top pick by default).
    void renderDecisionFocus(const QList<trading::DecisionRow> &rows, const QString &focusSymbol);
    // The headline conclusion under the sources table: the call + confidence, suggested
    // leverage/amount, recommendation basis, Claude's rationale and any event warning.
    void renderDecisionConclusion(const QList<trading::DecisionRow> &rows,
                                  const trading::DecisionRow *focus, const QString &focusSymbol,
                                  bool manual);
    // The conclusion's suggested leverage: the costed plan's figure, else Claude's (only
    // for its own pick), else the legacy cap — always clamped to the instrument max.
    [[nodiscard]] qint32 suggestedFocusLeverage(const trading::DecisionRow &focus,
                                                const QString &focusSymbol,
                                                bool aiActionable) const;
    // "algorithmic top / Claude" context line under the headline ("" when neither exists).
    [[nodiscard]] QString decisionBasisHtml(const QList<trading::DecisionRow> &rows) const;
    // Build + render the decision window's costed trade plan for the focus instrument
    // (verdict, P(win), risk factor, leverage, SL/TP, the full cost bill); the result
    // is kept in m_lastPlan for the "Apply to trade panel" button.
    void renderTradePlan(const trading::DecisionRow *focus, const QString &focusSymbol);
    // Batch-cost a plan per ranked instrument (off the GUI thread) so the ranked
    // table's "Trade plan" column mirrors the panel's verdict for every row.
    void dispatchRowPlans(const QList<trading::DecisionRow> &rows);
    void applyRowPlanVerdicts();       // batch finished → refresh the column cells
    void updateDecisionOpenColumn();   // market opened/closed → repaint "Open" cells
    // Applies an asynchronously-built plan (renderTradePlan dispatches the
    // Monte-Carlo heavy buildTradePlan to the thread pool via m_planWatcher).
    void renderTradePlanResult(const trading::TradePlan &plan, const QString &focusSymbol,
                               bool isCurrent);
    // Applies the signals-panel Monte-Carlo outlook computed off the GUI thread.
    void renderMonteCarlo(const trading::McOutlook &mc);
    void applyDecisionPlan();     // push m_lastPlan into the trade panel (no order placed)
    // Auto-fill the trade panel's SL/TP from recent volatility while the user hasn't
    // edited them by hand (m_slTpAuto); volPct is the per-hour σ from updateSignals.
    void proposeSlTpDefaults(double volPct);
    // Close-proposal watchdog: when the live price breaks out of the forecast
    // corridor against an open position (or the signal flips against it with high
    // confidence), propose closing that position — banner + log, never auto-close.
    void checkCloseProposals(double price);
    // Rows for the closed-trades window: filtered to the app's own instruments while
    // its "Only this app's instruments" box is ticked (the default).
    [[nodiscard]] QList<ClosedTrade> shownClosedTrades() const;
    void rebuildClosedTradesTable();  // (re)fill the closed-trades detail window
    // Lookback for closed-trade refreshes: every fetch feeds the summary panel AND
    // the details dialog, so while the dialog is open its selector wins — an auto
    // refresh (startup, post-close) must not shrink the dialog's window to 7 weeks.
    [[nodiscard]] qint32 closedLookbackWeeks() const;
    void updateTradeHours(const QString &symbol);  // approx. trading-hours label
    // Show the chart's event line while an event is within 10 min before / 5 min after.
    void refreshChartEventMarker();
    // Recompute the soonest upcoming event and (re)build the events list, dropping any
    // event that passed more than 10 minutes ago. force=false skips the rebuild when
    // nothing has aged out since the last render (used by the periodic prune timer).
    void rebuildEventsView(bool force = false);

    EtoroClient *m_client = nullptr;
    MarketFeeds *m_feeds = nullptr;      // VIX / TradingView rating / news
    AiAdvisor *m_aiAdvisor = nullptr;    // Claude decision synthesis

    PriceChart *m_chart = nullptr;
    QLabel *m_titleLabel = nullptr;
    QComboBox *m_instrumentBox = nullptr;  // instrument selector next to the title
    QPushButton *m_chartToggle = nullptr;  // small show/hide toggle for the chart window
    QPushButton *m_screenerButton = nullptr;  // opens the leverage screener dialog
    // Floating stay-on-top window holding the signals AND the AI panel
    // (parentless, like the chart — a parented Qt::Window would steal the main
    // window's focus). Shown at startup; always visible while trading.
    QWidget *m_signalsWindow = nullptr;      // holds m_sigBox + m_aiBox
    QPushButton *m_signalsToggle = nullptr;  // header show/hide ("Signals & AI")

    // Leverage screener window (its table/ranking live in ScreenerDialog).
    ScreenerDialog *m_screenerDialog = nullptr;
    // Scripted trading (REQ-F-028): the runner exists from construction so an
    // armed script keeps executing with its window closed; the dialog is lazy.
    TradeScriptRunner *m_scriptRunner = nullptr;
    TradeScriptDialog *m_scriptDialog = nullptr;
    QPushButton *m_scriptButton = nullptr;
    // Paper-trading bot simulation (REQ-F-029): like the script runner, the RUNNER
    // exists from construction (it keeps marking its simulated positions and
    // persists its books) while the window is created on demand.
    // Local-model advisor for the bot simulation (REQ-F-030); unconfigured is the
    // normal case and costs nothing.
    OllamaAdvisor *m_ollama = nullptr;
    BotSimRunner *m_botRunner = nullptr;
    // The one open "Trading-Bot opened a trade" notice, if any. At most one at a
    // time: a single scan can open a dozen trades. A plain pointer cleared from the
    // box's own destroyed() signal — QPointer would do the same, but its
    // QWeakPointer internals defeat the static analyzer, which then reports a
    // use-after-free inside Qt's header on every TU that includes this one.
    QMessageBox *m_botTradeNotice = nullptr;
    QList<trading::AiProposal> m_localPicks;   // the local model's latest answer
    QStringList m_localAsked;                  // …and what it was shown to answer about
    BotSimDialog *m_botDialog = nullptr;
    QPushButton *m_botButton = nullptr;
    QList<ScreenerRow> m_screenerRows;  // latest scan results, unsorted (arrival order)
    QGroupBox *m_tradeBox = nullptr;       // "Trade <symbol>" panel
    QLabel *m_tradeHours = nullptr;        // approx. trading hours (local) under the title
    QGroupBox *m_sigBox = nullptr;         // "Trading signals — <symbol>" panel
    QLabel *m_priceLabel = nullptr;
    QLabel *m_cashLabel = nullptr;
    QLabel *m_modeLabel = nullptr;
    QDoubleSpinBox *m_amount = nullptr;
    QComboBox *m_leverage = nullptr;
    QDoubleSpinBox *m_stopLoss = nullptr;
    QCheckBox *m_trailingStop = nullptr;   // make the stop-loss trail the price
    QDoubleSpinBox *m_takeProfit = nullptr;
    QLabel *m_openCost = nullptr;  // estimated spread cost to open a buy / a sell
    QLabel *m_feeCost = nullptr;   // overnight/weekend rollover fees for this order size
    InstrumentFees m_fees;         // per-unit fees for the current instrument (real mode)
    QLabel *m_marketClosedLabel = nullptr;  // shown under BUY/SELL when the market is closed
    QWidget *m_marketClosedRow = nullptr;   // holds the label + its override, shown as a unit
    // "Trade anyway": re-enables BUY/SELL against a closed verdict (REQ-F-026). The
    // inference reads a broker feed we do not control, so a wrong "closed" must never
    // be the last word; every use is logged and selectInstrument() clears it.
    QCheckBox *m_marketClosedOverride = nullptr;
    QPushButton *m_buyButton = nullptr;
    QPushButton *m_sellButton = nullptr;
    // App symbols whose market is currently open (inferred from the quote timestamp
    // advancing between polls); m_tradeabilityKnown stays false until the first update,
    // so trading isn't blocked before the state is known.
    QSet<QString> m_tradeableNow;
    bool m_tradeabilityKnown = false;

    // Limit orders (REQ-F-027): the rate to enter at per side, the submit buttons, and
    // the table of orders currently resting AT ETORO with its cancel button. eToro (not
    // this app) watches the price, so a limit order fires with the app closed and off
    // the broker's own feed rather than the app's minutes-delayed poll.
    QDoubleSpinBox *m_limitBuyRate = nullptr;
    QDoubleSpinBox *m_limitSellRate = nullptr;
    QPushButton *m_limitBuyButton = nullptr;
    QPushButton *m_limitSellButton = nullptr;
    QTableWidget *m_pendingTable = nullptr;
    QPushButton *m_editPendingButton = nullptr;    // opens the editor for the marked order
    QPushButton *m_cancelPendingButton = nullptr;
    QList<PendingOrder> m_pendingShown;  // row-indexed snapshot behind m_pendingTable
    QString m_selectedPendingId;         // order id the user marked (survives a refresh)
    QTableView *m_positions = nullptr;         // view over m_positionsModel
    PositionsModel *m_positionsModel = nullptr;
    TradeGaugeDialog *m_tradeGauge = nullptr;  // per-trade gauge (click on a row)
    QPushButton *m_closeButton = nullptr;
    // Row-indexed snapshot of the open trades currently shown, so the SL/TP editors
    // can convert a currency amount back into a rate using each trade's open/units.
    QList<Position> m_shownPositions;

    // SL/TP rates the user just submitted for a position, pinned in the table until
    // the server's portfolio snapshot reflects them (or a short timeout). Without
    // this the ~3s portfolio poll can momentarily revert a just-edited cell to the
    // pre-edit value, so a change looks like it "didn't take".
    struct PendingSlTp {
        double slRate = 0.0;
        double tpRate = 0.0;
        qint64 sinceMs = 0;  // when submitted (ms since epoch), for the timeout
    };
    QHash<QString, PendingSlTp> m_pendingSlTp;  // keyed by positionId
    QPlainTextEdit *m_log = nullptr;

    // Closed-trade monthly P/L summary (listed instruments only).
    QGroupBox *m_pnlBox = nullptr;
    QLabel *m_pnlSummary = nullptr;
    QTableWidget *m_pnlTable = nullptr;
    QPushButton *m_pnlRefresh = nullptr;
    QPushButton *m_pnlDetails = nullptr;     // opens the closed-trades detail window
    QPushButton *m_closedButton = nullptr;   // …and so does this one, from the header
    bool m_pnlAutoFetched = false;  // auto-fetch the summary once, on first ready
    QTimer *m_pnlAfterCloseTimer = nullptr;  // refreshes the summary ~10s after a close,
                                             // coalescing a burst of closes into one fetch
    // Throttle for the immediate refresh that a vanished open trade triggers:
    // the trade-history endpoint sits in the small 60-requests/60s rate pool, so
    // a flurry of closes must not turn into a flurry of history walks.
    qint64 m_lastClosedFetchMs = 0;

    // Closed-trades detail window: every trade of the last N weeks (N selectable
    // 7–13), with the API's net P/L + fees and the app's open/close cost estimates.
    QDialog *m_closedDialog = nullptr;
    QComboBox *m_closedWeeks = nullptr;      // lookback selector, 7..13 weeks
    QLabel *m_closedSummary = nullptr;
    QTableWidget *m_closedTable = nullptr;
    QPushButton *m_closedRefresh = nullptr;
    QCheckBox *m_closedListedOnly = nullptr;  // restrict the list to the app's own
                                             // instruments (on by default)
    QList<ClosedTrade> m_closedTrades;       // latest fetched detail list

    // Trading-signals panel
    QLabel *m_sigTrend = nullptr;
    QLabel *m_sigMomentum = nullptr;
    QLabel *m_sigMacd = nullptr;
    QLabel *m_sigBoll = nullptr;
    QLabel *m_sigVol = nullptr;
    QLabel *m_sigVix = nullptr;
    QLabel *m_sigRegime = nullptr;  // VIX + calendar market regime (risk-on/off)
    QLabel *m_sigNews = nullptr;    // news sentiment for the current instrument
    QLabel *m_sigWeb = nullptr;  // real-time TradingView technical rating
    QLabel *m_sigCrowd = nullptr;
    QLabel *m_sigLocalAi = nullptr;   // the local model's read on this instrument
    QLabel *m_sigConfluence = nullptr;  // how many independent reads agree
    QLabel *m_sigWebQuote = nullptr;  // Yahoo reference quote vs the eToro rate
    QLabel *m_sigRegression = nullptr;
    QLabel *m_sigKnn = nullptr;
    QLabel *m_sigStoch = nullptr;
    QLabel *m_sigTrend50 = nullptr;
    QLabel *m_sigRisk = nullptr;
    QLabel *m_sigChange = nullptr;
    QLabel *m_sigPrediction = nullptr;
    QLabel *m_sig3h = nullptr;
    QLabel *m_sig3d = nullptr;  // AI ensemble forecast for the next 3 trading days
    QLabel *m_sigOverall = nullptr;

    // AI decision-support panel
    QGroupBox *m_aiBox = nullptr;
    QLabel *m_aiUpProb = nullptr;      // logistic up-probability from the feature set
    QLabel *m_aiMonteCarlo = nullptr;  // bootstrap Monte-Carlo 3h outlook
    QLabel *m_aiEdge = nullptr;        // P(take-profit before stop-loss) vs breakeven
    QLabel *m_aiRegime = nullptr;      // trending vs mean-reverting (Hurst)
    QLabel *m_aiAdvice = nullptr;      // synthesized plain-language suggestion

    // Soonest upcoming calendar event, folded into the prediction as event risk.
    QDateTime m_nextEventTime;
    QString m_nextEventTitle;
    QList<EconomicEvent> m_eventList;  // upcoming events, for the chart event marker

    // Economic-calendar panel (service injected by the composition root)
    EconomicCalendar *m_calendar = nullptr;
    QGroupBox *m_eventsBox = nullptr;  // "Market events … (with <symbol> impact)"
    QListWidget *m_events = nullptr;
    QTimer *m_eventTimer = nullptr;    // periodically ages passed events out of the list
    qint32 m_shownEventCount = -1;     // events currently rendered (-1 = nothing built yet)

    // "Buy / sell now" panel: actionable instruments (from the screener ensemble +
    // the web rating), sorted by confidence, each with a reasoning tooltip that also
    // cites recent news. Fed by the same scan as the leverage screener.
    QGroupBox *m_recoBox = nullptr;
    QListWidget *m_recoBuyList = nullptr;   // left column: BUY calls, strongest first
    QListWidget *m_recoSellList = nullptr;  // right column: SELL calls, strongest first
    QPushButton *m_recoRefresh = nullptr;
    QTimer *m_recoTimer = nullptr;                      // periodic background refresh
    bool m_recoKickoff = false;                         // first scan fired once the app is live
    QHash<QString, WebRating> m_ratingBySymbol;         // latest web rating per instrument
    QHash<QString, QList<NewsHeadline>> m_newsBySymbol; // latest headlines per instrument
    QHash<QString, QList<double>> m_intradayBySymbol;   // Yahoo 1-min session closes
    QHash<QString, QList<double>> m_referenceSeries;    // ^VIX / ^VXN / ^TNX / heavyweights (both indices)

    // Decision window (separate, lazily built like the screener dialog).
    QPushButton *m_decisionButton = nullptr;   // opens the window, in the header
    QDialog *m_decisionDialog = nullptr;
    QLabel *m_decisionConclusion = nullptr;    // the prominent final call
    QLabel *m_decisionPlanLabel = nullptr;     // the costed trade plan for the focus
    QPushButton *m_decisionApply = nullptr;    // pushes the plan into the trade panel
    trading::TradePlan m_lastPlan;             // plan behind the Apply button
    // Off-GUI-thread computation of the Monte-Carlo heavy pieces (QtConcurrent):
    // the signals-panel outlook (coalesced: a tick is skipped while one runs)
    // and the decision-window trade plan (the watcher always tracks the newest
    // request, so stale results are dropped automatically).
    QFutureWatcher<trading::McOutlook> m_mcWatcher;
    bool m_mcBusy = false;
    qint32 m_mcScore = 0;                      // ensemble score behind the running MC
    double m_mcTp = 0.0;                       // take-profit amount the MC evaluated
    double m_mcSl = 0.0;                       // stop-loss amount the MC evaluated
    double m_mcExposure = 0.0;                 // amount × leverage behind tp/sl fracs
    double m_lastMcEdge = 0.0;                 // edge of the last COMPLETED MC run —
    bool m_lastMcEdgeValid = false;            // the advice line reads these (≤1 tick old)
    QFutureWatcher<trading::TradePlan> m_planWatcher;
    QString m_planPendingSymbol;               // focus symbol of the running plan build
    bool m_planPendingIsCurrent = false;
    QFutureWatcher<QHash<QString, trading::TradePlan>> m_rowPlanWatcher;
    QHash<QString, trading::TradePlan> m_rowPlans;  // ranked-table plan verdicts by symbol
    QString m_lastPlanSymbol;                  // instrument that plan was built for
    QLabel *m_decisionSourcesLabel = nullptr;  // "Sources for <focus symbol>:" caption
    QTableWidget *m_decisionSources = nullptr; // one row per source for the focus instrument
    QTableWidget *m_decisionRanked = nullptr;  // all instruments by composite
    QString m_decisionSelected;                // symbol picked in the ranked list ("" = top)
    bool m_decisionUpdatingRanked = false;     // guard: ignore selection signals during refill
    QLabel *m_decisionAiStatus = nullptr;
    QPushButton *m_decisionRefresh = nullptr;
    AiDecision m_aiDecision;                    // latest Claude synthesis
    bool m_decisionAiPending = false;           // ask Claude once the in-flight scan ends
    double m_availableCash = 0.0;               // latest free cash, for the suggested size

    // Ctrl+wheel UI zoom state. m_baseFonts records each widget's font the first
    // time it is scaled, so repeated zooms always scale from the original size
    // (never compound); the base window sizes are what applyUiScale multiplies.
    double m_uiScale = 1.0;
    QSize m_baseMainSize;
    QSize m_baseChartSize;
    QHash<QWidget *, QFont> m_baseFonts;

    // Double-tap buy/sell shortcut state.
    qint32 m_quickKey = 0;
    qint64 m_quickKeyMs = 0;

    // Buy/Sell button double-press state (see handleOrderButton).
    qint64 m_orderClickMs = 0;   // time of the first (arming) press, 0 = none pending
    bool m_orderClickBuy = false;  // which button the pending press was on
    bool m_orderClickLimit = false;  // …and whether it was a limit or a market button

    // CBOE VIX ("fear index"), folded into the buy/sell signal ensemble.
    double m_vix = 0.0;          // latest VIX level
    double m_vixChangePct = 0.0;  // change vs. previous close (%)
    bool m_vixValid = false;     // a VIX reading has been received

    // Crowd sentiment: CNN Fear & Greed index (0 fear .. 100 greed).
    bool m_fgValid = false;
    double m_fg = 50.0;
    QString m_fgRating;

    // Independent Yahoo reference quote for the current instrument.
    QString m_webQuoteSymbol;
    double m_webQuotePrice = 0.0;
    QDateTime m_webQuoteTime;     // exchange timestamp of that price

    // Trade-panel SL/TP auto-proposal state: proposals track volatility until the
    // user edits either field by hand; a new instrument re-enables the automatics.
    bool m_slTpAuto = true;
    bool m_settingSlTp = false;   // guards valueChanged during programmatic fills

    // Close-proposal watchdog state: the latest 3h forecast corridor and ensemble
    // read (from updateSignals), plus a per-position cooldown so one breach doesn't
    // spam the log on every price tick.
    double m_forecastTarget = 0.0;   // 0 = no forecast yet
    double m_forecastBandPct = 0.0;  // ±1σ band around the target, in percent
    qint32 m_lastSignalDir = 0;      // ensemble call: +1 BUY / -1 SELL / 0
    double m_lastSignalConf = 0.0;   // its confidence after the haircuts
    QLabel *m_closeAdvice = nullptr; // banner above "Close marked trades"
    QLabel *m_openTradesSummary = nullptr;  // invested + P/L totals under the table
    QHash<QString, qint64> m_closeAdviceMs;  // positionId -> last proposal (ms epoch)


    QString m_ccy;           // display currency symbol (euro)
    double m_eurPerUsd = 0.0;  // EUR per 1 USD (0 = unknown → show raw USD at parity)
    // Hourly candle closes (history resampled to 1-hour bars), used to compute the
    // buy/sell signals. Hourly — not the minute-scale tail — so the live panel reads
    // the same multi-week swing as the leverage screener (which fetches hourly too).
    QList<double> m_hourlyCloses;
    double m_lastPrice = 0.0;
    bool m_limitRateDefaultsSet = false;  // seed the limit-order rates off the first price
    // True from instrument selection until EtoroClient::ready confirms the switch;
    // BUY/SELL stay locked so an order can never target the previous instrument.
    bool m_instrumentResolving = false;
    // At startup, load the instrument of the top open trade — once. Set as soon as
    // the first portfolio arrives (or the user picks an instrument) so it never fires again.
    bool m_autoInstrumentDone = false;

    // Order guards (see kOrderCooldownMs / kMaxOpenExposure in MainWindow.cpp).
    QTimer *m_orderCooldownTimer = nullptr;  // blocks new buy/sell for 2s after an order
    double m_openTradesTotal = 0.0;          // sum of open-position amounts, for the exposure cap
};

#endif // TRADINGAPP_MAINWINDOW_H
