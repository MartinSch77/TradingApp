// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_TRADESCRIPTPANEL_H
#define TRADINGAPP_UI_TRADESCRIPTPANEL_H

#include "domain/TradeScript.h"

#include <QDialog>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class EtoroClient;
struct PendingOrder;
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTableWidget)
QT_FORWARD_DECLARE_CLASS(QTimer)

// The trade-script runner (REQ-F-028): drives a loaded script's entries
// through the broker-side limit-order machinery (REQ-F-027). LOADING never
// places anything; execution happens only while the script is explicitly
// ARMED, and disarming cancels every scripted order still resting. While
// disarmed, evaluation still runs and logs what WOULD be placed — the dry run.
// The runner lives independently of its dialog: an armed script keeps
// executing with the window closed.
class TradeScriptRunner : public QObject
{
    Q_OBJECT
public:
    // Lifecycle of one entry per arming. Waiting covers "window not open yet",
    // "signals not aligned" and "instrument not resolved yet"; Pending is the
    // gap between openPosition() and the broker's order appearing in the
    // pending registry; Done is final (filled, rejected, or expired).
    enum class EntryState : qint8 { Waiting, Pending, Resting, Done };

    struct Tracked {
        trading::ScriptEntry entry;
        EntryState state = EntryState::Waiting;
        QString orderId;             // the broker order, while Resting
        QString note;                // status text for the table
        qint64 placedMs = 0;         // when openPosition() went out (Pending watchdog)
        bool cancelRequested = false; // we asked for the cancel (vs. broker resolution)
        bool dryRunAnnounced = false; // one "would place" line per alignment (no spam)
    };

    explicit TradeScriptRunner(EtoroClient *client, QObject *parent = nullptr);

    // The open-exposure cap belongs to the trade panel's guard set; the runner
    // asks this gate before every placement so scripted orders obey the same
    // limit as manual ones (REQ-F-028). Returns false + a reason to refuse.
    using ExposureGate = std::function<bool(double amount, QString *whyNot)>;
    void setExposureGate(ExposureGate gate);

    // Parse + adopt a script (all-or-nothing). A previously loaded script's
    // resting orders are cancelled first — two scripts must never run at once.
    // On failure nothing changes and `errors` carries the per-line reasons.
    [[nodiscard]] bool load(const QString &text, QStringList *errors);

    // The explicit second step of the two-step commitment (REQ-F-028).
    void setArmed(bool armed);
    [[nodiscard]] bool armed() const { return m_armed; }
    [[nodiscard]] bool loaded() const { return !m_tracked.isEmpty(); }

    // Live inputs for SIGNALS-flagged entries (ensemble + AI call, ±1/0).
    void setSignalState(qint32 ensembleDir, qint32 aiDir, bool aiConfigured);

    [[nodiscard]] const QList<Tracked> &tracked() const & { return m_tracked; }

signals:
    void log(const QString &message, bool isError);
    void changed();  // some entry changed state — refresh the view

private:
    void evaluate();                                       // the 5 s tick
    bool evaluateEntry(Tracked &t, const QDateTime &now);  // one entry's state step
    void place(Tracked &t, qint64 instrumentId);
    void cancel(Tracked &t, const QString &why);
    void onPendingOrders(const QList<PendingOrder> &orders);

    EtoroClient *m_client = nullptr;
    QTimer *m_timer = nullptr;         // periodic evaluation while a script is loaded
    QList<Tracked> m_tracked;
    ExposureGate m_gate;
    trading::ScriptSignalState m_signals;
    bool m_armed = false;
};

// The script window: load a file, review the parsed entries with their live
// status, and arm/disarm. A plain view over the runner — no trading logic here.
class TradeScriptDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TradeScriptDialog(TradeScriptRunner *runner, QWidget *parent = nullptr);

private:
    void chooseAndLoadFile();
    void rebuild();  // refill the table from the runner's entries

    TradeScriptRunner *m_runner = nullptr;
    QPushButton *m_loadButton = nullptr;
    QPushButton *m_armButton = nullptr;   // checkable: the explicit ARM step
    QLabel *m_status = nullptr;
    QTableWidget *m_table = nullptr;
};

#endif // TRADINGAPP_UI_TRADESCRIPTPANEL_H
