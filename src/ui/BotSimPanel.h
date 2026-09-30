// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_BOTSIMPANEL_H
#define TRADINGAPP_UI_BOTSIMPANEL_H

#include "domain/ConfirmGate.h"
#include "ui/BotSimRunner.h"

#include <QDialog>

class QVBoxLayout;
QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPlainTextEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTableWidget)
QT_FORWARD_DECLARE_CLASS(QTimer)
class LiveBotExecutor;

class BotSimDialog : public QDialog
{
    Q_OBJECT
public:
    // `live` is the real-money mirror (REQ-F-076) and may be null: a front end that
    // composes no gateway shows the box as unavailable rather than pretending to have one.
    BotSimDialog(BotSimRunner *runner, LiveBotExecutor *live, QWidget *parent = nullptr);

private:
    void buildUi();
    void buildAccountBox(QVBoxLayout *layout);
    void buildLiveBox(QVBoxLayout *layout);   // "Real money — SPX500 + NSDQ100"
    void buildTables(QVBoxLayout *layout);
    void rebuild();                 // refresh everything from the runner
    void rebuildAccount();          // the header figures
    void rebuildAi();               // the local-model row: status, mode, last proposal
    void rebuildLive();   // the armed state, its caps, the mirrored positions
    void rebuildOpenTable();
    void rebuildClosedTable();
    void appendLog(const QString &message, bool isError);
    void confirmReset();
    // The arming button's press: THE REQ-N-005 double press (trading::confirmPress, the
    // same gate the trade panel and the cockpit use) over the executor's grant action.
    void pressLiveArm();

    BotSimRunner *m_runner = nullptr;
    LiveBotExecutor *m_live = nullptr;
    trading::ConfirmGate m_liveGate;   // the arming button's own gate state
    QPushButton *m_armButton = nullptr;     // checkable: the explicit ARM step
    QPushButton *m_resetButton = nullptr;
    QLabel *m_accountLabel = nullptr;       // start / cash / invested / equity
    QLabel *m_pnlLabel = nullptr;           // open / realised / total P/L
    QLabel *m_statsLabel = nullptr;         // trades, win rate, costs paid
    QLabel *m_dayLabel = nullptr;           // today against the daily target / limit
    QLabel *m_recordLabel = nullptr;        // the measured record (REQ-F-031)
    QLabel *m_liveLabel = nullptr;          // the real-money verdict and its blockers
    QPushButton *m_trainButton = nullptr;   // refit the outcome model on demand
    QLabel *m_reasonLabel = nullptr;        // net per exit rule, worst first
    QLabel *m_modelLabel = nullptr;         // the learned model and the experience log
    QLabel *m_storeLabel = nullptr;         // where the books live
    QComboBox *m_aiModeBox = nullptr;       // off / confirm / lead (REQ-F-030)
    QPushButton *m_aiCheckButton = nullptr; // re-probe the local model service
    QLabel *m_aiStatusLabel = nullptr;      // service state + the latest proposal
    QTableWidget *m_openTable = nullptr;
    QTableWidget *m_closedTable = nullptr;
    QPlainTextEdit *m_log = nullptr;
    // The real-money box (REQ-F-076).
    QLabel *m_liveStateLabel = nullptr;   // armed state, caps, day loss, readiness verdict
    QPushButton *m_liveArmButton = nullptr;
    QPushButton *m_liveDisarmButton = nullptr;
    QPushButton *m_liveKillButton = nullptr;
    QPushButton *m_liveClearButton = nullptr;
    QTableWidget *m_liveTable = nullptr;   // the mirrored positions
};

#endif // TRADINGAPP_UI_BOTSIMPANEL_H
