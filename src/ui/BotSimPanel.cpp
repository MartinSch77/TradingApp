// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/BotSimPanel.h"

#include "domain/PositionMath.h"   // priceDecimals + the quote-freshness bound
#include "services/EtoroClient.h"
#include "domain/Forecasting.h"
#include "domain/IndexConfluence.h"
#include "domain/LeadSignal.h"
#include "services/OllamaAdvisor.h"
#include "ui/LiveBotExecutor.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

using trading::PaperClosedTrade;
using trading::PaperStats;
using trading::PaperTrade;

namespace {

void configureTable(QTableWidget *table, const QStringList &headers)
{
    table->setColumnCount(static_cast<int>(headers.size()));
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
}

QTableWidgetItem *cell(const QString &text, bool numeric = false, double signValue = 0.0,
                       bool colour = false)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (numeric) {
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (colour) {
        item->setForeground((signValue >= 0.0) ? QColor(0x1B, 0x8A, 0x3A)
                                               : QColor(0xC0, 0x39, 0x2B));
    }
    return item;
}

// Index of `id` in a trade list, or -1. The list holds at most maxOpenTrades
} // namespace

BotSimDialog::BotSimDialog(BotSimRunner *runner, LiveBotExecutor *live, QWidget *parent)
    : QDialog(parent), m_runner(runner), m_live(live)
{
    setWindowTitle(QStringLiteral("Trading-bot simulation (paper money)"));
    resize(1180, 860);
    buildUi();
    static_cast<void>(connect(m_runner, &BotSimRunner::changed, this, &BotSimDialog::rebuild));
    static_cast<void>(connect(m_runner, &BotSimRunner::log, this, &BotSimDialog::appendLog));
    if (m_live != nullptr) {
        static_cast<void>(
            connect(m_live, &LiveBotExecutor::changed, this, &BotSimDialog::rebuildLive));
        static_cast<void>(connect(m_live, &LiveBotExecutor::log, this, &BotSimDialog::appendLog));
    }
    rebuild();
}

void BotSimDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    buildAccountBox(layout);
    buildLiveBox(layout);
    buildTables(layout);
}

// The real-money box (REQ-F-076): the ONE surface that can arm the mirror. The arming
// button is the REQ-N-005 double press over the executor's grant action, which names the
// scope, both caps and the duration — so what the two presses confirm is exactly what
// they permit. The other three buttons are single-press on purpose: disarming, the kill
// switch and clearing it move no money, and a panic action must not need two presses.
void BotSimDialog::buildLiveBox(QVBoxLayout *layout)
{
    auto *box = new QGroupBox(QStringLiteral("Real money — SPX500 + NSDQ100"), this);
    auto *boxLayout = new QVBoxLayout(box);
    m_liveStateLabel = new QLabel(box);
    m_liveStateLabel->setObjectName(QStringLiteral("liveStateLabel"));
    m_liveStateLabel->setWordWrap(true);
    m_liveStateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    boxLayout->addWidget(m_liveStateLabel);

    auto *buttons = new QHBoxLayout;
    m_liveArmButton = new QPushButton(QStringLiteral("Arm REAL money (press twice)"), box);
    m_liveArmButton->setObjectName(QStringLiteral("liveArmButton"));
    m_liveArmButton->setToolTip(QStringLiteral(
        "Mirror the paper bot's SPX500 and NSDQ100 decisions to REAL orders for one trading "
        "day, under the caps named in the state line. Two presses within one second, like "
        "every money-moving control. Requires real keys and mode=real, a paper record that "
        "clears the real-money criteria, and a kill switch that is not tripped."));
    static_cast<void>(
        connect(m_liveArmButton, &QPushButton::clicked, this, &BotSimDialog::pressLiveArm));
    m_liveDisarmButton = new QPushButton(QStringLiteral("Disarm"), box);
    m_liveDisarmButton->setObjectName(QStringLiteral("liveDisarmButton"));
    m_liveKillButton = new QPushButton(QStringLiteral("KILL SWITCH"), box);
    m_liveKillButton->setObjectName(QStringLiteral("liveKillButton"));
    m_liveKillButton->setToolTip(QStringLiteral(
        "Disarm at once and STAY disarmed until cleared by hand — a panic action that no "
        "timer tick, re-arm click or restart can undo."));
    m_liveClearButton = new QPushButton(QStringLiteral("Clear kill switch"), box);
    m_liveClearButton->setObjectName(QStringLiteral("liveClearButton"));
    if (m_live != nullptr) {
        static_cast<void>(
            connect(m_liveDisarmButton, &QPushButton::clicked, this, [this] { m_live->disarm(); }));
        static_cast<void>(connect(m_liveKillButton, &QPushButton::clicked, this, [this] {
            m_live->trip(QStringLiteral("kill switch pressed in the bot window"));
        }));
        static_cast<void>(connect(m_liveClearButton, &QPushButton::clicked, this,
                                  [this] { m_live->clearTrip(); }));
    }
    for (QPushButton *button :
         {m_liveArmButton, m_liveDisarmButton, m_liveKillButton, m_liveClearButton}) {
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    boxLayout->addLayout(buttons);

    m_liveTable = new QTableWidget(box);
    m_liveTable->setObjectName(QStringLiteral("liveTable"));
    configureTable(m_liveTable, {QStringLiteral("Instrument"), QStringLiteral("Side"),
                                 QStringLiteral("Stake"), QStringLiteral("Position id"),
                                 QStringLiteral("State"), QStringLiteral("Paper trade")});
    m_liveTable->setMaximumHeight(140);
    boxLayout->addWidget(m_liveTable);
    layout->addWidget(box);
}

void BotSimDialog::pressLiveArm()
{
    if (m_live == nullptr) {
        return;
    }
    const trading::ConfirmDecision decision =
        trading::confirmPress(m_liveGate, m_live->grantAction(),
                              QDateTime::currentMSecsSinceEpoch(), trading::kConfirmWindowMs);
    m_liveGate = decision.next;
    if (!decision.commit) {
        appendLog(decision.prompt, false);
        m_liveArmButton->setText(QStringLiteral("Press again within 1 s to ARM"));
        // The window is the gate's, not the button's: when it has passed the button says
        // what a fresh press would do again, and a stale arm is visibly stale.
        QTimer::singleShot(trading::kConfirmWindowMs, this, [this] { rebuildLive(); });
        return;
    }
    static_cast<void>(m_live->arm());   // refusals are logged by the executor itself
    rebuildLive();
}

void BotSimDialog::rebuildLive()
{
    if (m_live == nullptr) {
        m_liveStateLabel->setText(QStringLiteral(
            "Real-money execution is not available in this front end — it holds no order "
            "gateway. The Widgets app composes one; the console binaries never do."));
        for (QPushButton *button :
             {m_liveArmButton, m_liveDisarmButton, m_liveKillButton, m_liveClearButton}) {
            button->setEnabled(false);
        }
        m_liveTable->setRowCount(0);
        return;
    }
    const trading::LiveReadiness live = m_runner->liveReadiness();
    const bool armed = m_live->isArmed();
    const bool tripped = m_live->isTripped();
    const bool clientLive = m_live->clientLive();
    m_liveStateLabel->setText(
        QStringLiteral("<b>%1</b><br>%2 · record: %3")
            .arg(m_live->stateLine(),
                 clientLive ? QStringLiteral("broker client LIVE (real keys, mode=real)")
                            : QStringLiteral("broker client not live — real keys and "
                                             "mode=real required"),
                 live.ready ? QStringLiteral("<span style='color:#1b8a3a'>real-money "
                                             "criteria met</span>")
                            : QStringLiteral("<span style='color:#c0392b'>not ready</span> — %1")
                                  .arg(live.blockers.join(u"; "))));
    m_liveArmButton->setText(m_liveGate.action.isEmpty()
                                 ? QStringLiteral("Arm REAL money (press twice)")
                                 : QStringLiteral("Press again within 1 s to ARM"));
    m_liveArmButton->setEnabled(clientLive && !armed && !tripped);
    m_liveDisarmButton->setEnabled(armed);
    m_liveKillButton->setEnabled(!tripped);
    m_liveClearButton->setEnabled(tripped);

    const QList<LiveMirroredPosition> mirrored = m_live->mirrored();
    m_liveTable->setRowCount(static_cast<int>(mirrored.size()));
    for (qsizetype i = 0; i < mirrored.size(); ++i) {
        const LiveMirroredPosition &p = mirrored.at(i);
        const int row = static_cast<int>(i);
        m_liveTable->setItem(row, 0, cell(p.symbol));
        m_liveTable->setItem(row, 1,
                             cell(p.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL")));
        m_liveTable->setItem(row, 2, cell(p.stake.toString(), true));
        m_liveTable->setItem(
            row, 3, cell(p.positionId.isEmpty() ? QStringLiteral("confirming…") : p.positionId));
        m_liveTable->setItem(row, 4, cell(p.state));
        m_liveTable->setItem(row, 5, cell(QStringLiteral("#%1").arg(p.paperId), true));
    }
}

// The header: the account, the day, the record, the real-money verdict, the model,
// and the controls that change what the bot does.
void BotSimDialog::buildAccountBox(QVBoxLayout *layout)
{
    auto *account = new QGroupBox(QStringLiteral("Simulated account"), this);
    auto *accountLayout = new QVBoxLayout(account);
    m_accountLabel = new QLabel(account);
    m_accountLabel->setObjectName(QStringLiteral("accountLabel"));
    m_pnlLabel = new QLabel(account);
    m_pnlLabel->setObjectName(QStringLiteral("pnlLabel"));
    m_statsLabel = new QLabel(account);
    m_statsLabel->setObjectName(QStringLiteral("statsLabel"));
    m_dayLabel = new QLabel(account);
    m_dayLabel->setObjectName(QStringLiteral("dayLabel"));
    m_recordLabel = new QLabel(account);
    m_recordLabel->setObjectName(QStringLiteral("recordLabel"));
    m_recordLabel->setWordWrap(true);
    m_liveLabel = new QLabel(account);
    m_liveLabel->setObjectName(QStringLiteral("liveLabel"));
    m_liveLabel->setWordWrap(true);
    m_reasonLabel = new QLabel(account);
    m_reasonLabel->setObjectName(QStringLiteral("reasonLabel"));
    m_reasonLabel->setWordWrap(true);
    m_modelLabel = new QLabel(account);
    m_modelLabel->setObjectName(QStringLiteral("modelLabel"));
    m_modelLabel->setWordWrap(true);
    m_storeLabel = new QLabel(account);
    m_storeLabel->setObjectName(QStringLiteral("storeLabel"));
    m_storeLabel->setStyleSheet(QStringLiteral("color: #666;"));
    for (QLabel *label : {m_accountLabel, m_pnlLabel, m_statsLabel, m_dayLabel, m_recordLabel,
                          m_reasonLabel, m_liveLabel, m_modelLabel, m_storeLabel}) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        accountLayout->addWidget(label);
    }

    auto *buttons = new QHBoxLayout;
    m_armButton = new QPushButton(QStringLiteral("Arm the bot"), account);
    m_armButton->setObjectName(QStringLiteral("armButton"));
    m_armButton->setCheckable(true);
    m_armButton->setToolTip(QStringLiteral(
        "Start / stop the simulation. The bot trades SIMULATED money on live prices — the "
        "simulation itself never places an order at eToro. Real money is the separate, "
        "separately armed box below (REQ-F-076)."));
    static_cast<void>(connect(m_armButton, &QPushButton::toggled, this, [this](bool on) {
        m_runner->setArmed(on);
    }));
    m_resetButton = new QPushButton(QStringLiteral("Reset to start capital"), account);
    m_resetButton->setObjectName(QStringLiteral("resetButton"));
    static_cast<void>(
        connect(m_resetButton, &QPushButton::clicked, this, &BotSimDialog::confirmReset));
    buttons->addWidget(m_armButton);
    buttons->addWidget(m_resetButton);
    buttons->addStretch(1);
    accountLayout->addLayout(buttons);

    // Local-model row (REQ-F-030): what the model service is doing, how its
    // proposal is used, and a re-probe.
    auto *aiRow = new QHBoxLayout;
    aiRow->addWidget(new QLabel(QStringLiteral("Local model (Ollama):"), account));
    m_aiModeBox = new QComboBox(account);
    m_aiModeBox->setObjectName(QStringLiteral("aiModeBox"));
    m_aiModeBox->addItem(QStringLiteral("off — composite decides"),
                         static_cast<int>(trading::BotAiMode::Off));
    m_aiModeBox->addItem(QStringLiteral("confirm — model must agree"),
                         static_cast<int>(trading::BotAiMode::Confirm));
    m_aiModeBox->addItem(QStringLiteral("lead — trade the model's pick"),
                         static_cast<int>(trading::BotAiMode::Lead));
    m_aiModeBox->setToolTip(QStringLiteral(
        "How the local model's proposal is used.\n"
        "off:     the multi-source composite decides; the proposal is only logged.\n"
        "confirm: only the model's pick may be opened, and only while the composite "
        "agrees with its side (the model is a veto).\n"
        "lead:    the model's own pick and side are traded.\n"
        "In every setting the risk rules still apply — the model can never exceed the "
        "stake, exposure, leverage or ruin limits."));
    static_cast<void>(connect(m_aiModeBox, &QComboBox::currentIndexChanged, this, [this](int idx) {
        m_runner->setAiMode(static_cast<trading::BotAiMode>(m_aiModeBox->itemData(idx).toInt()));
    }));
    aiRow->addWidget(m_aiModeBox);
    m_aiCheckButton = new QPushButton(QStringLiteral("Check model"), account);
    m_aiCheckButton->setObjectName(QStringLiteral("aiCheckButton"));
    static_cast<void>(connect(m_aiCheckButton, &QPushButton::clicked, this,
                              [this]() { m_runner->checkAi(); }));
    aiRow->addWidget(m_aiCheckButton);
    m_trainButton = new QPushButton(QStringLiteral("Train from experience"), account);
    m_trainButton->setObjectName(QStringLiteral("trainButton"));
    m_trainButton->setToolTip(QStringLiteral(
        "Refit the outcome model on every trade this bot has closed (REQ-F-033). Runs in "
        "the app itself — no Python needed — and happens automatically every 25 closed "
        "trades as well. The model only gets to refuse trades once it has seen enough of "
        "them and has beaten a coin flip on ones it never saw."));
    static_cast<void>(connect(m_trainButton, &QPushButton::clicked, this,
                              [this]() { m_runner->trainFromExperience(); }));
    aiRow->addWidget(m_trainButton);
    aiRow->addStretch(1);
    accountLayout->addLayout(aiRow);
    m_aiStatusLabel = new QLabel(account);
    m_aiStatusLabel->setObjectName(QStringLiteral("aiStatusLabel"));
    m_aiStatusLabel->setWordWrap(true);
    m_aiStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    accountLayout->addWidget(m_aiStatusLabel);
    layout->addWidget(account);
}

// The three lists: what is open, what closed, and why the bot did what it did.
void BotSimDialog::buildTables(QVBoxLayout *layout)
{
    auto *openBox = new QGroupBox(QStringLiteral("Open simulated trades"), this);
    auto *openLayout = new QVBoxLayout(openBox);
    m_openTable = new QTableWidget(openBox);
    m_openTable->setObjectName(QStringLiteral("openTable"));
    configureTable(m_openTable, {QStringLiteral("Instrument"), QStringLiteral("Side"),
                                 QStringLiteral("Invested"), QStringLiteral("Lev"),
                                 QStringLiteral("Entry"), QStringLiteral("Now"),
                                 QStringLiteral("Stop"), QStringLiteral("Target"),
                                 QStringLiteral("Costs"), QStringLiteral("P/L"),
                                 QStringLiteral("AI"), QStringLiteral("Opened / why")});
    openLayout->addWidget(m_openTable);
    layout->addWidget(openBox, 2);

    auto *closedBox = new QGroupBox(QStringLiteral("Closed simulated trades"), this);
    auto *closedLayout = new QVBoxLayout(closedBox);
    m_closedTable = new QTableWidget(closedBox);
    m_closedTable->setObjectName(QStringLiteral("closedTable"));
    configureTable(m_closedTable, {QStringLiteral("Instrument"), QStringLiteral("Side"),
                                   QStringLiteral("Invested"), QStringLiteral("Lev"),
                                   QStringLiteral("Entry"), QStringLiteral("Exit"),
                                   QStringLiteral("Held (h)"), QStringLiteral("Costs"),
                                   QStringLiteral("Net P/L"), QStringLiteral("Closed when / because")});
    closedLayout->addWidget(m_closedTable);
    layout->addWidget(closedBox, 2);

    auto *logBox = new QGroupBox(QStringLiteral("Bot decisions"), this);
    auto *logLayout = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setObjectName(QStringLiteral("log"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    logLayout->addWidget(m_log);
    layout->addWidget(logBox, 1);
}

void BotSimDialog::rebuild()
{
    if (m_armButton->isChecked() != m_runner->armed()) {
        const QSignalBlocker block(m_armButton);
        m_armButton->setChecked(m_runner->armed());
    }
    m_armButton->setText(m_runner->armed() ? QStringLiteral("Stop the bot")
                                           : QStringLiteral("Arm the bot"));
    rebuildAccount();
    rebuildAi();
    rebuildLive();
    rebuildOpenTable();
    rebuildClosedTable();
}

void BotSimDialog::rebuildAi()
{
    const int wanted = m_aiModeBox->findData(static_cast<int>(m_runner->aiMode()));
    if ((wanted >= 0) && (wanted != m_aiModeBox->currentIndex())) {
        const QSignalBlocker block(m_aiModeBox);
        m_aiModeBox->setCurrentIndex(wanted);
    }
    const QString status = m_runner->aiStatus().isEmpty()
                               ? QStringLiteral("not checked yet")
                               : m_runner->aiStatus();
    // Every pick the model named, in its own order — the window has to show the
    // whole answer, since the bot may act on all of them.
    const QList<trading::AiProposal> &picks = m_runner->lastProposals();
    QStringList lines;
    for (const trading::AiProposal &p : picks) {
        const QString side = (p.dir == 0) ? QStringLiteral("HOLD")
                                          : ((p.dir > 0) ? QStringLiteral("BUY")
                                                         : QStringLiteral("SELL"));
        lines << QStringLiteral("<b>%1 %2</b> (conf %3, lev x%4)")
                     .arg(side,
                          p.resolvedSymbol.isEmpty()
                              ? QStringLiteral("%1 [not tradable here]").arg(p.symbol)
                              : p.resolvedSymbol)
                     .arg(p.confidence, 0, 'f', 0)
                     .arg(p.leverage);
    }
    const QString rationale = picks.isEmpty() || picks.constFirst().rationale.isEmpty()
                                  ? QString()
                                  : QStringLiteral(" — %1").arg(picks.constFirst().rationale);
    m_aiStatusLabel->setText(
        QStringLiteral("Service: %1<br>Last picks (%2): %3%4")
            .arg(status)
            .arg(picks.size())
            .arg(picks.isEmpty() ? QStringLiteral("none yet") : lines.join(u" · "), rationale));
}

void BotSimDialog::rebuildAccount()
{
    const PaperStats s = m_runner->stats();
    m_accountLabel->setText(
        QStringLiteral("<b>Start</b> %1 &nbsp;|&nbsp; <b>Equity</b> %2 &nbsp;|&nbsp; "
                       "<b>Cash</b> %3 &nbsp;|&nbsp; <b>Invested</b> %4")
            .arg(botPlain(s.startEquity), botPlain(s.equity), botPlain(s.cash), botPlain(s.invested)));
    const QString colour = (s.totalPnl >= 0.0) ? QStringLiteral("#1b8a3a") : QStringLiteral("#c0392b");
    m_pnlLabel->setText(
        QStringLiteral("Open P/L %1 &nbsp;|&nbsp; Realised %2 &nbsp;|&nbsp; "
                       "<b style='color:%3'>Total %4 (%5%)</b>")
            .arg(botMoney(s.openPnl), botMoney(s.realized), colour, botMoney(s.totalPnl))
            .arg(s.totalPnlPct, 0, 'f', 2));
    const trading::BotDay day = m_runner->today();
    const trading::PaperPerformance perf = m_runner->performance();
    const trading::LiveReadiness live = m_runner->liveReadiness();
    m_dayLabel->setText(
        QStringLiteral("<b>Today</b> %1 of %2 target · %3 opened, %4 closed · %5")
            .arg(botMoney(day.realized),
                 botPlain(m_runner->book().config().dailyProfitTarget))
            .arg(day.opened)
            .arg(day.closed)
            .arg((day.realized >= m_runner->book().config().dailyProfitTarget)
                     ? QStringLiteral("<b style='color:#1b8a3a'>target reached — done for today</b>")
                     : ((day.realized <= -m_runner->book().config().dailyLossLimit)
                            ? QStringLiteral("<b style='color:#c0392b'>loss limit reached — done "
                                             "for today</b>")
                            : QStringLiteral("trading"))));
    m_recordLabel->setText(
        QStringLiteral("<b>Record</b> %1 closed over %2 day(s) · net %3 (%4/day, last %5 day(s) "
                       "%6) · profit factor %7 · win rate %8% · max drawdown %9 (%10%) · "
                       "shorts %11 (net %12)")
            .arg(perf.closedTrades)
            .arg(perf.tradingDays)
            .arg(botMoney(perf.netTotal), botMoney(perf.netPerDay))
            .arg(perf.rollingDays)
            .arg(botMoney(perf.netLastDays))
            .arg(perf.profitFactor, 0, 'f', 2)
            .arg(perf.winRate, 0, 'f', 0)
            .arg(botPlain(perf.maxDrawdown))
            .arg(perf.maxDrawdownPct, 0, 'f', 1)
            .arg(perf.shortTrades)
            .arg(botMoney(perf.shortNet)));
    m_modelLabel->setText(
        QStringLiteral("%1 · experience log: %2")
            .arg(trading::botNetSummary(m_runner->net(), m_runner->netMode(),
                                        trading::NetGateConfig{}),
                 m_runner->experiencePath()));
    // Which RULE is making or losing the money — the view that answers "why is the
    // record what it is" without anyone reading the JSON.
    QStringList byReason;
    QList<QPair<double, QString>> ranked;
    for (auto it = perf.netByReason.cbegin(); it != perf.netByReason.cend(); ++it) {
        ranked.append({it.value(), it.key()});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;   // worst first: that is the one to look at
    });
    for (const auto &[net, reason] : ranked) {
        byReason << QStringLiteral("%1 %2x %3")
                        .arg(reason)
                        .arg(perf.countByReason.value(reason))
                        .arg(botMoney(net));
    }
    m_reasonLabel->setText(byReason.isEmpty()
                               ? QStringLiteral("<b>By exit rule</b> nothing closed yet")
                               : QStringLiteral("<b>By exit rule</b> (worst first) %1")
                                     .arg(byReason.join(u" · ")));
    // The RECORD's verdict (REQ-F-031). Whether real money follows is the box below's
    // business (REQ-F-076), which shows this same verdict beside its arming button.
    m_liveLabel->setText(
        live.ready
            ? QStringLiteral("<b style='color:#1b8a3a'>Real-money criteria met</b> — the paper "
                             "record clears every threshold.")
            : QStringLiteral("<b style='color:#c0392b'>Not ready for real money</b> — %1.")
                  .arg(live.blockers.join(u"; ")));
    m_statsLabel->setText(
        QStringLiteral("%1 open · %2 closed (%3 won / %4 lost, win rate %5%) · "
                       "costs paid %6 · best %7 · worst %8")
            .arg(s.openTrades)
            .arg(s.closedTrades)
            .arg(s.wins)
            .arg(s.losses)
            .arg(s.winRate, 0, 'f', 0)
            .arg(botPlain(s.costsPaid), botMoney(s.bestTrade), botMoney(s.worstTrade)));
    m_storeLabel->setText(QStringLiteral("The simulated books never reach eToro; the only order "
                                         "path is the real-money box below. Books: %1")
                              .arg(m_runner->storePath()));
}

void BotSimDialog::rebuildOpenTable()
{
    const QList<PaperTrade> &open = m_runner->book().openTrades();
    m_openTable->setRowCount(static_cast<int>(open.size()));
    for (qsizetype i = 0; i < open.size(); ++i) {
        const PaperTrade &t = open.at(i);
        const int row = static_cast<int>(i);
        m_openTable->setItem(row, 0, cell(t.symbol));
        m_openTable->setItem(row, 1, cell(t.isBuy ? QStringLiteral("BUY")
                                                  : QStringLiteral("SELL")));
        m_openTable->setItem(row, 2, cell(botPlain(t.stake), true));
        m_openTable->setItem(row, 3, cell(QStringLiteral("x%1").arg(t.leverage), true));
        m_openTable->setItem(row, 4, cell(botRate(t.openRate), true));
        // A mark that is not live is flagged, exactly as the real open-trades
        // table flags one (a stale quote must never look like a live P/L).
        m_openTable->setItem(row, 5,
                             cell(t.markLive ? botRate(t.effectiveRate())
                                             : QStringLiteral("%1 (not live)")
                                                   .arg(botRate(t.effectiveRate())),
                                  true));
        m_openTable->setItem(row, 6, cell(botRate(t.slRate), true));
        m_openTable->setItem(row, 7, cell(botRate(t.tpRate), true));
        m_openTable->setItem(row, 8,
                             cell(QStringLiteral("%1 (+%2 nights)")
                                      .arg(botPlain(t.costsSoFar()))
                                      .arg(t.nightsCharged),
                                  true));
        m_openTable->setItem(row, 9, cell(botMoney(t.netPnl()), true, t.netPnl(), true));
        // What the model says about KEEPING this one, refreshed every review pass:
        // hold, close, or "—" for a position it has not mentioned (REQ-F-032).
        const trading::HoldVerdict hold = m_runner->holdOpinion(t.id);
        QTableWidgetItem *flag = cell(trading::holdOpinionWord(hold.opinion));
        flag->setTextAlignment(Qt::AlignCenter);
        if (hold.opinion == trading::HoldOpinion::Close) {
            flag->setForeground(QColor(0xC0, 0x39, 0x2B));
        } else if (hold.opinion == trading::HoldOpinion::Hold) {
            flag->setForeground(QColor(0x1B, 0x8A, 0x3A));
        }
        flag->setToolTip(hold.why.isEmpty()
                             ? QStringLiteral("The model has not been asked about this position "
                                              "yet, or did not mention it in its last answer. "
                                              "Silence never closes a trade.")
                             : hold.why);
        m_openTable->setItem(row, 10, flag);
        m_openTable->setItem(row, 11,
                             cell(QStringLiteral("%1 — %2")
                                      .arg(t.openTime.toString(QStringLiteral("MM-dd HH:mm")),
                                           t.entryBasis)));
    }
}

void BotSimDialog::rebuildClosedTable()
{
    const QList<PaperClosedTrade> &closed = m_runner->book().closedTrades();
    m_closedTable->setRowCount(static_cast<int>(closed.size()));
    // Newest first: the interesting end of a multi-day experiment is the recent one.
    for (qsizetype i = 0; i < closed.size(); ++i) {
        const PaperClosedTrade &c = closed.at(closed.size() - 1 - i);
        const int row = static_cast<int>(i);
        m_closedTable->setItem(row, 0, cell(c.symbol));
        m_closedTable->setItem(row, 1, cell(c.isBuy ? QStringLiteral("BUY")
                                                    : QStringLiteral("SELL")));
        m_closedTable->setItem(row, 2, cell(botPlain(c.stake), true));
        m_closedTable->setItem(row, 3, cell(QStringLiteral("x%1").arg(c.leverage), true));
        m_closedTable->setItem(row, 4, cell(botRate(c.openRate), true));
        m_closedTable->setItem(row, 5, cell(botRate(c.closeRate), true));
        m_closedTable->setItem(row, 6, cell(QStringLiteral("%1").arg(c.heldHours(), 0, 'f', 1),
                                            true));
        m_closedTable->setItem(row, 7, cell(botPlain(c.totalCost()), true));
        m_closedTable->setItem(row, 8, cell(botMoney(c.netPnl), true, c.netPnl, true));
        // WHEN it closed belongs next to WHY: reading a record of exits without
        // their times says nothing about whether the rules fired when they should.
        m_closedTable->setItem(
            row, 9,
            cell(c.closeTime.isValid()
                     ? QStringLiteral("%1 — %2")
                           .arg(c.closeTime.toString(QStringLiteral("MM-dd HH:mm")),
                                trading::closeReasonWord(c.reason))
                     : trading::closeReasonWord(c.reason)));
    }
}

void BotSimDialog::appendLog(const QString &message, bool isError)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_log->appendPlainText(QStringLiteral("%1  %2%3")
                               .arg(stamp, isError ? QStringLiteral("ERROR: ") : QString(),
                                    message));
}

void BotSimDialog::confirmReset()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Reset the simulation?"),
        QStringLiteral("Close the open simulated trades at their current mark and start over "
                       "at the configured capital?\n\nThe recorded history is discarded — the "
                       "experiment starts from zero."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes) {
        m_runner->resetBooks();
    }
}
