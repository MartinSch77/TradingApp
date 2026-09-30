// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_LIVEBOTEXECUTOR_H
#define TRADINGAPP_UI_LIVEBOTEXECUTOR_H

#include "domain/LiveMirror.h"
#include "domain/Models.h"
#include "domain/PaperTrader.h"
#include "services/OrderGateway.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class BotSimRunner;
class EtoroClient;

// One position the mirror sent, and what became of it.
struct LiveMirroredPosition {
    qint64 paperId = 0;
    QString symbol;
    qint64 instrumentId = 0;
    bool isBuy = true;
    trading::Money stake;
    QString positionId;   // the broker's id; empty until positionOpened names it
    QString requestId;   // the guarded send's id, for the audit record
    // "confirming…" / "open" / "closing…" / "close by hand" / "closed <net>": what the
    // window shows beside the id. A position whose broker id never arrived is flagged
    // rather than dropped: it is REAL money, and dropping it would tell someone they are
    // flat while the risk is open.
    QString state;
    bool closed = false;
};

// The real-money mirror of the paper bot's SPX500/NSDQ100 decisions (REQ-F-076,
// DES-UI-LIVEBOT). Qt Core only, like BotSimRunner, and composed ONLY in the Widgets
// front end: the console binaries hold no gateway, so they cannot send.
//
// The paper RUNNER still has no route to an order endpoint — it emits trade events. This
// object is the one place that holds a gateway, and every order it sends goes through
// GuardedOrderSender: validated, checked against the armed state and its caps, and
// written to the audit record, or refused with the reason. Nothing here is persisted:
// a restart is disarmed, always.
//
// arm() is the ONLY way in and fails closed three times over, in the order a reader
// wants the reason: the kill switch (tripped beats everything, and only clearTrip()
// releases it), the runner owning its book (a book held by another process is being
// traded THERE), and the client being live (real keys and mode "real"). The REQ-F-031
// readiness verdict is NOT a lock — the owner's decision of 2026-09-30, taken in
// REQ-F-031 itself — but an arming over an unmet record logs every unmet threshold, so
// the verdict is faced, never bypassed unseen. The "is the client live" question is a predicate
// handed in at construction, defaulting to the client's own answer: the headless suite
// has no real account and must still be able to exercise the armed path, and a test
// hook on the class itself (a setter, a flag) would be a way to arm in production.
// What the composition root hands the executor besides its collaborators: the grant's
// shape, where every attempt is recorded, and the "is the client live" question (empty =
// the client's own answer, which is what the composition root leaves it at).
struct LiveBotSetup {
    trading::LiveMirrorConfig config;
    QString auditPath;
    std::function<bool()> isClientLive;
};

class LiveBotExecutor : public QObject
{
    Q_OBJECT
public:
    using LiveCheck = std::function<bool()>;

    // Non-owning pointers, all of them outliving this object (the composition root
    // constructs them there).
    LiveBotExecutor(EtoroClient *client, BotSimRunner *runner, trading::IOrderGateway *gateway,
                    LiveBotSetup setup, QObject *parent = nullptr);

    // Called by the dialog AFTER trading::confirmPress committed — never from anything
    // the app computes. False (and a log line naming why) when refused.
    bool arm(const QDateTime &now = QDateTime::currentDateTimeUtc());
    void disarm();
    // THE KILL SWITCH: sticky, cleared only by clearTrip().
    void trip(const QString &reason);
    void clearTrip();

    [[nodiscard]] bool isArmed(const QDateTime &now = QDateTime::currentDateTimeUtc()) const;
    [[nodiscard]] bool isTripped() const { return m_arm.isTripped(); }
    // The client's own answer to "may real orders go out of this process at all" — what
    // the window greys the arming button on.
    [[nodiscard]] bool clientLive() const { return m_isClientLive(); }
    // LiveArm::stateLine + the day's realised loss against its cap + the mirrored count.
    [[nodiscard]] QString stateLine(const QDateTime &now = QDateTime::currentDateTimeUtc()) const;
    [[nodiscard]] const trading::LiveMirrorConfig &config() const { return m_cfg; }
    [[nodiscard]] QString grantAction() const { return trading::liveGrantAction(m_cfg); }
    [[nodiscard]] QList<LiveMirroredPosition> mirrored() const { return m_mirrored; }
    [[nodiscard]] trading::LiveDay day() const { return m_day; }
    [[nodiscard]] QString auditPath() const { return m_audit.path(); }

signals:
    void log(const QString &message, bool isError);
    void changed();

public slots:
    // The runner's events (connected in the constructor, direct connections — every
    // object here lives on the GUI thread, so no metatype registration is needed).
    void onPaperOpened(const trading::PaperTrade &trade);
    void onPaperClosed(const trading::PaperClosedTrade &done);
    // The client's answers.
    void onPositionOpened(const QString &positionId, qint64 instrumentId, bool isBuy);
    void onPositionClosed(bool ok, const QString &message, const QString &positionId);
    // The last polled live P/L per position id — what a close realises (the close reply
    // itself carries no figure, and the portfolio poll that would is measurably behind).
    void onPortfolio(const QList<Position> &positions);
    void onFx(double eurPerUsd);

private:
    // Why arming is refused right now, or empty when it may proceed.
    [[nodiscard]] QString armRefusal() const;
    [[nodiscard]] qint32 openLiveCountFor(const QString &symbol) const;
    // The validator's context for one plan: the account currency the client learned (USD
    // until it says otherwise), the instrument by id and symbol, the paper trade's own
    // leverage as a one-step ladder (the paper bot already folded to an offered step, and
    // the validator checks the ladder CONTAINS the leverage), and the client's rate for
    // the id (0 = unknown, which the validator refuses for a market order).
    [[nodiscard]] trading::OrderContext contextFor(const trading::PaperTrade &trade,
                                                   const trading::LiveOrderPlan &plan) const;
    void sendMirror(const trading::PaperTrade &trade, const trading::LiveOrderPlan &plan,
                    const QDateTime &now);
    // Book one closed position's realised result and trip on the daily loss.
    void realise(LiveMirroredPosition &pos, const QString &positionId, const QDateTime &now);

    EtoroClient *m_client = nullptr;
    BotSimRunner *m_runner = nullptr;
    trading::LiveMirrorConfig m_cfg;
    LiveCheck m_isClientLive;
    trading::LiveArm m_arm;
    trading::OrderAudit m_audit;
    trading::GuardedOrderSender m_sender;
    QList<LiveMirroredPosition> m_mirrored;
    trading::LiveDay m_day;
    // The daily-loss cap in the ORDER currency, fixed at the rate of the arming like the
    // per-order cap: the ledger it is compared with is what the broker realises, in the
    // account's currency, and a cap in another currency compares as unordered.
    trading::Money m_dayLossCap;
    QHash<QString, double> m_profitById;   // last polled P/L per broker position id
    double m_eurPerUsd = 0.0;   // 0 = unknown -> every plan refused live-fx-unknown
    bool m_runnerArmedSeen = false;   // for the one "paper bot disarmed" line
};

#endif   // TRADINGAPP_UI_LIVEBOTEXECUTOR_H
