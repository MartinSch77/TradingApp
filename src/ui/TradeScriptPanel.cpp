// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/TradeScriptPanel.h"

#include "domain/InstrumentCatalog.h"
#include "domain/Models.h"
#include "services/EtoroClient.h"
#include "ui/Palette.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

// How often the runner re-evaluates the script against the clock and the
// signal state. Entries are edge-triggered state machines, so a coarse tick
// is enough — the TRIGGER itself is watched by the broker, not by this timer.
constexpr qint32 kEvaluateMs = 5000;
// An openPosition() whose order never appeared in the pending registry is
// given this long before the entry re-arms (the client already logged the
// rejection); prevents a rejected placement from wedging the entry forever.
constexpr qint64 kPendingTimeoutMs = 30LL * 1000;

// The 5-dp round openPositionReal applies to the trigger it sends — matching
// it makes "is that resting order OURS" an exact comparison, not a guess.
double roundedTrigger(double trigger)
{
    return std::round(trigger * 1e5) / 1e5;
}

// "script line N (BUY SPX500 @ …)" — every runner log line leads with this,
// so a log reader can find the file line that caused an order (REQ-F-028).
QString describe(const TradeScriptRunner::Tracked &t)
{
    return QStringLiteral("script line %1 (%2 %3 @ %4)")
        .arg(t.entry.lineNumber)
        .arg(t.entry.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
             t.entry.symbol)
        .arg(t.entry.trigger);
}

} // namespace

// ---------------------------------------------------------------------------
// TradeScriptRunner
// ---------------------------------------------------------------------------

TradeScriptRunner::TradeScriptRunner(EtoroClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(kEvaluateMs);
    static_cast<void>(connect(m_timer, &QTimer::timeout, this, &TradeScriptRunner::evaluate));
    static_cast<void>(connect(m_client, &EtoroClient::pendingOrdersUpdated, this,
                              &TradeScriptRunner::onPendingOrders));
}

void TradeScriptRunner::setExposureGate(ExposureGate gate)
{
    m_gate = std::move(gate);
}

bool TradeScriptRunner::load(const QString &text, QStringList *errors)
{
    const trading::ScriptParseResult parsed = trading::parseTradeScript(text);
    if (!parsed.ok) {
        if (errors != nullptr) {
            *errors = parsed.errors;
        }
        return false;
    }
    // Two scripts must never run at once: retire the old one first, loudly.
    setArmed(false);
    m_tracked.clear();
    m_tracked.reserve(parsed.entries.size());
    for (const trading::ScriptEntry &e : parsed.entries) {
        Tracked t;
        t.entry = e;
        t.note = QStringLiteral("waiting");
        m_tracked.append(t);
    }
    m_timer->start();
    emit log(QStringLiteral("Trade script loaded: %1 entries — DISARMED (dry run). "
                            "Arm it to trade.")
                 .arg(m_tracked.size()),
             false);
    emit changed();
    evaluate();
    return true;
}

void TradeScriptRunner::setArmed(bool armed)
{
    if (armed == m_armed) {
        return;
    }
    m_armed = armed;
    if (armed) {
        emit log(QStringLiteral("Trade script ARMED — its entries will be placed as "
                                "broker-side limit orders when their conditions hold."),
                 false);
    } else {
        // Disarming cancels everything still resting (REQ-F-028).
        for (Tracked &t : m_tracked) {
            if (t.state == EntryState::Resting) {
                cancel(t, QStringLiteral("script disarmed"));
            }
            t.dryRunAnnounced = false;
        }
        if (loaded()) {
            emit log(QStringLiteral("Trade script disarmed — scripted orders cancelled; "
                                    "dry-run evaluation continues."),
                     false);
        }
    }
    emit changed();
    evaluate();
}

void TradeScriptRunner::setSignalState(qint32 ensembleDir, qint32 aiDir, bool aiConfigured)
{
    m_signals.ensembleDir = ensembleDir;
    m_signals.aiDir = aiDir;
    m_signals.aiConfigured = aiConfigured;
    if (loaded()) {
        evaluate();  // signal flips place/cancel promptly, not on the next tick
    }
}

// One entry's state step at `now`; true = something observable changed.
bool TradeScriptRunner::evaluateEntry(Tracked &t, const QDateTime &now)
{
    const bool wantsRest = trading::scriptEntryShouldRest(t.entry, now, m_signals);

    // Expiry ends the entry whatever its state (a resting order is pulled).
    if (trading::scriptEntryExpired(t.entry, now)) {
        if (t.state == EntryState::Resting) {
            cancel(t, QStringLiteral("window closed"));
        }
        t.state = EntryState::Done;
        t.note = QStringLiteral("expired — window closed");
        emit log(QStringLiteral("%1 expired without executing.").arg(describe(t)), false);
        return true;
    }

    switch (t.state) {
    case EntryState::Waiting:
        if (!wantsRest) {
            t.dryRunAnnounced = false;
            return false;
        }
        if (!m_armed) {
            // The dry run: say what WOULD happen, once per alignment.
            if (t.dryRunAnnounced) {
                return false;
            }
            t.dryRunAnnounced = true;
            t.note = QStringLiteral("dry run — would place now");
            emit log(QStringLiteral("DRY RUN: %1 would be placed now (script is not armed).")
                         .arg(describe(t)),
                     false);
            return true;
        }
        place(t, m_client->instrumentIdFor(t.entry.symbol));
        return true;
    case EntryState::Pending:
        // The client reported the placement (or its rejection) in the log;
        // if no order materialised, re-arm instead of wedging the entry.
        if ((QDateTime::currentMSecsSinceEpoch() - t.placedMs) > kPendingTimeoutMs) {
            t.state = EntryState::Waiting;
            t.note = QStringLiteral("placement not confirmed — retrying");
            return true;
        }
        return false;
    case EntryState::Resting:
        if (!wantsRest) {
            cancel(t, QStringLiteral("conditions no longer hold"));
            t.state = EntryState::Waiting;  // may re-place inside the window
            t.note = QStringLiteral("cancelled — waiting for conditions");
            return true;
        }
        return false;
    case EntryState::Done:
        return false;
    }
    std::unreachable();  // every EntryState is handled above
}

void TradeScriptRunner::evaluate()
{
    const QDateTime now = QDateTime::currentDateTime();
    bool anyChange = false;
    bool anyLive = false;
    for (Tracked &t : m_tracked) {
        if (t.state == EntryState::Done) {
            continue;
        }
        anyLive = true;
        anyChange = evaluateEntry(t, now) || anyChange;
    }
    if (!anyLive) {
        m_timer->stop();  // every entry is final — nothing left to evaluate
    }
    if (anyChange) {
        emit changed();
    }
}

void TradeScriptRunner::place(Tracked &t, qint64 instrumentId)
{
    if (instrumentId == 0) {
        t.note = QStringLiteral("waiting — instrument not resolved yet");
        return;  // ids resolve shortly after startup; the next tick retries
    }
    QString whyNot;
    if (m_gate && !m_gate(t.entry.amount, &whyNot)) {
        t.note = QStringLiteral("blocked — %1").arg(whyNot);
        emit log(QStringLiteral("%1 blocked: %2").arg(describe(t), whyNot), true);
        return;  // re-evaluated next tick; exposure may free up
    }

    // REQ-F-028: snap a leverage the instrument does not offer to the next
    // lower offered value. The catalog's per-instrument steps are the best
    // static knowledge; the broker's own validation remains the backstop.
    const trading::InstrumentSpec *spec = trading::instrumentSpec(t.entry.symbol);
    const QList<qint32> offered = (spec != nullptr) ? spec->simLeverage : QList<qint32>{};
    const qint32 leverage = trading::snapLeverage(t.entry.leverage, offered);
    if (leverage != t.entry.leverage) {
        emit log(QStringLiteral("%1: leverage x%2 not offered — using x%3.")
                     .arg(describe(t))
                     .arg(t.entry.leverage)
                     .arg(leverage),
                 false);
    }

    OrderRequest req;
    req.isBuy = t.entry.isBuy;
    req.amount = t.entry.amount;
    req.leverage = leverage;
    req.stopLossAmount = t.entry.slAmount;
    req.takeProfitAmount = t.entry.tpAmount;
    req.trailingStop = t.entry.trailing;
    req.triggerRate = t.entry.trigger;   // > 0 = a broker-side limit order
    req.instrumentId = instrumentId;     // any listed instrument (REQ-F-027)
    m_client->openPosition(req);

    t.state = EntryState::Pending;
    t.placedMs = QDateTime::currentMSecsSinceEpoch();
    t.cancelRequested = false;
    t.note = QStringLiteral("placing…");
    emit log(QStringLiteral("%1 placed as a limit order (amount %2, x%3%4%5%6).")
                 .arg(describe(t))
                 .arg(t.entry.amount)
                 .arg(leverage)
                 .arg(t.entry.slAmount > 0.0
                          ? QStringLiteral(", SL %1").arg(t.entry.slAmount)
                          : QString())
                 .arg(t.entry.tpAmount > 0.0
                          ? QStringLiteral(", TP %1").arg(t.entry.tpAmount)
                          : QString())
                 .arg(t.entry.trailing ? QStringLiteral(", trailing") : QString()),
             false);
}

void TradeScriptRunner::cancel(Tracked &t, const QString &why)
{
    if (t.orderId.isEmpty()) {
        return;
    }
    t.cancelRequested = true;
    emit log(QStringLiteral("%1: cancelling resting order (%2).").arg(describe(t), why),
             false);
    m_client->cancelPendingOrder(t.orderId);
    t.orderId.clear();
}

void TradeScriptRunner::onPendingOrders(const QList<PendingOrder> &orders)
{
    bool anyChange = false;
    for (Tracked &t : m_tracked) {
        if (t.state == EntryState::Pending) {
            // Adopt the broker order that matches this entry (same instrument,
            // side and 5-dp trigger — the values openPositionReal sent).
            for (const PendingOrder &o : orders) {
                const bool sameTrigger =
                    std::abs(o.triggerRate - roundedTrigger(t.entry.trigger)) < 1e-9;
                if ((o.instrumentId == m_client->instrumentIdFor(t.entry.symbol))
                    && (o.isBuy == t.entry.isBuy) && sameTrigger && !o.orderId.isEmpty()) {
                    t.orderId = o.orderId;
                    t.state = EntryState::Resting;
                    t.note = QStringLiteral("resting at the broker (order %1)").arg(o.orderId);
                    anyChange = true;
                    break;
                }
            }
        } else if (t.state == EntryState::Resting && !t.orderId.isEmpty()) {
            // Our order left the registry: the broker resolved it (filled,
            // rejected or expired there) — the client's own log says which.
            const bool stillThere =
                std::any_of(orders.cbegin(), orders.cend(), [&t](const PendingOrder &o) {
                    return o.orderId == t.orderId;
                });
            if (!stillThere) {
                t.state = EntryState::Done;
                t.note = QStringLiteral("resolved at the broker — see the activity log");
                emit log(QStringLiteral("%1 resolved at the broker.").arg(describe(t)), false);
                t.orderId.clear();
                anyChange = true;
            }
        } else {
            // Waiting/Done entries have no broker order to reconcile.
        }
    }
    if (anyChange) {
        emit changed();
    }
}

// ---------------------------------------------------------------------------
// TradeScriptDialog
// ---------------------------------------------------------------------------

TradeScriptDialog::TradeScriptDialog(TradeScriptRunner *runner, QWidget *parent)
    : QDialog(parent)
    , m_runner(runner)
    , m_loadButton(new QPushButton(QStringLiteral("Load script…"), this))
    , m_armButton(new QPushButton(QStringLiteral("Arm"), this))
    , m_status(new QLabel(QStringLiteral("No script loaded."), this))
    , m_table(new QTableWidget(0, 6, this))
{
    setWindowTitle(QStringLiteral("Trade script"));
    resize(860, 420);
    auto *layout = new QVBoxLayout(this);

    auto *buttons = new QHBoxLayout;
    m_loadButton->setToolTip(QStringLiteral(
        "One order per line:\n"
        "<instrument>; BUY|SELL @ <trigger>; FROM yyyy-MM-dd HH:mm; TO yyyy-MM-dd HH:mm;\n"
        "SIGNALS; AMOUNT <stake>; SL <amount>; TP <amount>; TRAILING; LEV <n>\n"
        "AMOUNT is required, everything after the trigger is optional; '#' comments.\n"
        "Loading never places anything — arming does."));
    static_cast<void>(connect(m_loadButton, &QPushButton::clicked, this,
                              &TradeScriptDialog::chooseAndLoadFile));
    m_armButton->setCheckable(true);
    m_armButton->setEnabled(false);
    m_armButton->setToolTip(QStringLiteral(
        "The explicit second step (the automation counterpart of the double-press "
        "gate): while armed, entries are placed as broker-side limit orders when "
        "their conditions hold. Disarming cancels every scripted order still "
        "resting. While disarmed, the log shows what WOULD be placed (dry run)."));
    static_cast<void>(connect(m_armButton, &QPushButton::toggled, this, [this](bool on) {
        m_runner->setArmed(on);
        m_armButton->setText(on ? QStringLiteral("ARMED — click to disarm")
                                : QStringLiteral("Arm"));
    }));
    buttons->addWidget(m_loadButton);
    buttons->addWidget(m_armButton);
    buttons->addWidget(m_status, 1);
    layout->addLayout(buttons);

    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("Line"), QStringLiteral("Order"), QStringLiteral("Window"),
         QStringLiteral("Conditions"), QStringLiteral("Size"), QStringLiteral("Status")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_table);

    static_cast<void>(connect(m_runner, &TradeScriptRunner::changed, this,
                              &TradeScriptDialog::rebuild));
    rebuild();
}

void TradeScriptDialog::chooseAndLoadFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load trade script"), QString(),
        QStringLiteral("Trade scripts (*.txt *.trades);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("Cannot read %1").arg(path));
        return;
    }
    QStringList errors;
    if (!m_runner->load(QString::fromUtf8(f.readAll()), &errors)) {
        // All-or-nothing (REQ-F-028): show every bad line, load nothing.
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("Script rejected — nothing was loaded:\n\n%1")
                                 .arg(errors.join(QLatin1Char('\n'))));
        return;
    }
    m_armButton->setEnabled(true);
    m_armButton->setChecked(false);
    m_status->setText(QStringLiteral("%1 — %2 entries, DISARMED (dry run)")
                          .arg(QFileInfo(path).fileName())
                          .arg(m_runner->tracked().size()));
}

void TradeScriptDialog::rebuild()
{
    using Tracked = TradeScriptRunner::Tracked;
    const QList<Tracked> &rows = m_runner->tracked();
    m_table->setRowCount(static_cast<qint32>(rows.size()));
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const Tracked &t = rows.at(i);
        const trading::ScriptEntry &e = t.entry;
        auto cell = [this, i](qint32 col, const QString &text) {
            auto *item = new QTableWidgetItem(text);
            m_table->setItem(static_cast<qint32>(i), col, item);
            return item;
        };
        cell(0, QString::number(e.lineNumber));
        auto *order = cell(1, QStringLiteral("%1 %2 @ %3")
                                  .arg(e.isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                                       e.symbol)
                                  .arg(e.trigger));
        order->setForeground(e.isBuy ? trading::ui::kGreen : trading::ui::kRed);
        const QString fromText = e.from.isValid()
                                     ? e.from.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                     : QStringLiteral("now");
        const QString toText = e.to.isValid()
                                   ? e.to.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                   : QStringLiteral("open end");
        cell(2, QStringLiteral("%1 → %2").arg(fromText, toText));
        cell(3, e.requireSignals ? QStringLiteral("signals + AI must agree")
                                 : QStringLiteral("—"));
        cell(4, QStringLiteral("%1 · SL %2 · TP %3%4 · x%5")
                    .arg(e.amount)
                    .arg((e.slAmount > 0.0) ? QString::number(e.slAmount)
                                            : QStringLiteral("—"))
                    .arg((e.tpAmount > 0.0) ? QString::number(e.tpAmount)
                                            : QStringLiteral("—"))
                    .arg(e.trailing ? QStringLiteral(" (trailing)") : QString())
                    .arg(e.leverage));
        cell(5, t.note);
    }
}
