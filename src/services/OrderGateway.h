// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_ORDERGATEWAY_H
#define TRADINGAPP_SERVICES_ORDERGATEWAY_H

#include "domain/Money.h"
#include "domain/OrderRequestValidator.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

// The guarded path a real order would take (REQ-N-009), in three pieces that are
// deliberately separable: the ARMED state that must be entered by hand, the SEAM that
// makes the send testable, and the AUDIT record that survives the process.
//
// Nothing here is wired to the live broker. That wiring is a separate, deliberate act
// under REQ-N-005, and this file exists so that when it happens the machinery it
// needs is already built and tested rather than written under time pressure.
namespace trading {

// ---------------------------------------------------------------------------
// The armed state
// ---------------------------------------------------------------------------

// Why an order may not be sent. `None` means it may.
enum class ArmRefusal : quint8 {
    None = 0,
    NotArmed,      // nobody armed this session
    Expired,       // the armed window ran out — attention does not last all day
    Tripped,       // the kill switch was pulled and has not been cleared
    OverOrderCap,  // more than the armed session was granted per order
    OverDayCap     // more than the armed session was granted for the whole day
};

[[nodiscard]] QString armRefusalCode(ArmRefusal refusal);

// An explicit, visible, time-bounded permission to send real orders, with a sticky
// kill switch. Nothing the app COMPUTES may call arm(): the only caller is a UI action
// the user performed, which is what separates this from a configuration flag.
class LiveArm : public QObject
{
    Q_OBJECT;   // ";" so tree-sitter/moc see the first signal's marker

public:
    explicit LiveArm(QObject *parent = nullptr);

    // Arm for `minutes`, under the caps this grant carries. Refused (returns false)
    // when the kill switch is tripped: clearing that is its own deliberate act.
    bool arm(qint32 minutes, const Money &maxPerOrder, const Money &maxPerDay,
             const QDateTime &now = QDateTime::currentDateTimeUtc());
    // Ordinary disarm — the window is simply closed.
    void disarm();
    // THE KILL SWITCH. Sticky by design: a panic action must not be undone by the next
    // timer tick, a re-arm click or a restart of the bot. Only clearTrip() re-enables
    // arming, and that is a separate action a person has to take.
    void trip(const QString &reason);
    void clearTrip();

    [[nodiscard]] bool isTripped() const { return m_tripped; }
    [[nodiscard]] QString tripReason() const { return m_tripReason; }
    [[nodiscard]] bool isArmed(const QDateTime &now = QDateTime::currentDateTimeUtc()) const;
    [[nodiscard]] QDateTime armedUntil() const { return m_armedUntil; }
    [[nodiscard]] Money maxPerOrder() const { return m_maxPerOrder; }
    [[nodiscard]] Money maxPerDay() const { return m_maxPerDay; }
    [[nodiscard]] Money committedToday() const { return m_committedToday; }

    // May an order of this stake go out right now? The order of the checks matters:
    // tripped beats expired beats unarmed beats the caps, so the reason reported is
    // the most fundamental one rather than the first that happens to be computed.
    [[nodiscard]] ArmRefusal check(const Money &stake,
                                   const QDateTime &now = QDateTime::currentDateTimeUtc()) const;
    // Record a stake that actually went out, against the daily cap. Rolls the day over
    // on its own when the date changes — a daily cap that only resets on restart is
    // not a daily cap.
    void recordCommitted(const Money &stake,
                         const QDateTime &now = QDateTime::currentDateTimeUtc());

    // The caps as an OrderContext the validator can use, so the armed session's limits
    // and the validator's limits cannot drift apart.
    void applyCapsTo(OrderContext &context) const;

    // One line for the window: armed until when, under which caps, or why not.
    [[nodiscard]] QString stateLine(const QDateTime &now = QDateTime::currentDateTimeUtc()) const;

signals:
    // Every state change, so a UI cannot show a stale "ARMED" badge.
    void stateChanged();
    void trippedChanged(bool tripped, const QString &reason);

private:
    bool m_tripped = false;
    QString m_tripReason;
    QDateTime m_armedUntil;
    QDate m_committedDate;
    Money m_maxPerOrder;
    Money m_maxPerDay;
    Money m_committedToday;
};

// ---------------------------------------------------------------------------
// The audit record
// ---------------------------------------------------------------------------

// One attempt, whatever became of it. Written for accepted, validation-refused,
// arm-refused and broker-rejected alike: the question this record answers ("what
// exactly did this machine send, and what came back") is asked after something went
// wrong, when the log scrollback is gone.
struct OrderAuditEntry {
    QDateTime decidedAt;      // when the decision was made
    QDateTime sentAt;         // when it left the machine (invalid if it never did)
    QString requestId;        // the x-request-id the broker saw
    QString outcome;          // "sent" / "refused-validation" / "refused-arm" / "rejected"
    QString detail;           // the refusal codes, or the broker's own words
    // The complete fingerprint of what was asked for.
    qint64 instrumentId = 0;
    QString symbol;
    bool isBuy = true;
    Money stake;
    double leverage = 1.0;
    Money stopLoss;
    Money takeProfit;
    double triggerRate = 0.0;
    Currency orderCurrency = Currency::Invalid;
    Currency accountCurrency = Currency::Invalid;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static OrderAuditEntry fromJson(const QJsonObject &o);
    // A short stable digest of the fingerprint — enough to recognise the same order
    // twice in a log without reading every field.
    [[nodiscard]] QString fingerprint() const;
};

// Append-only, one JSON object per line, flushed per entry. JSON Lines rather than a
// JSON array on purpose: an array has to be rewritten to append, and a rewrite is how
// a crash loses the record of the order that caused it.
class OrderAudit
{
public:
    // `path` empty = the default location under the app's config directory.
    explicit OrderAudit(const QString &path = QString());

    [[nodiscard]] QString path() const { return m_path; }
    bool append(const OrderAuditEntry &entry);
    [[nodiscard]] QList<OrderAuditEntry> readAll() const;
    // Entries whose fingerprint matches — "did we already send this?" after a
    // network answer went missing.
    [[nodiscard]] QList<OrderAuditEntry> withFingerprint(const QString &fingerprint) const;

private:
    QString m_path;
};

// ---------------------------------------------------------------------------
// The seam
// ---------------------------------------------------------------------------

// What a gateway answers. `accepted` is about the SEND, not about the fill: a broker
// that took the request still reports execution separately.
struct OrderResult {
    bool accepted = false;
    QString requestId;
    QString detail;      // the broker's own reason, verbatim, when there is one
    QJsonObject raw;     // the answer as it arrived, for the audit record
};

// Order placement as an INTERFACE. Without this seam the order path can only be
// exercised against a live account, which means it is not exercised at all. Both
// implementations are compiled into the shipped binary — the choice is made at
// composition time, never by a build flag that could differ between the tested
// binary and the shipped one.
class IOrderGateway
{
public:
    IOrderGateway() = default;
    virtual ~IOrderGateway() = default;
    IOrderGateway(const IOrderGateway &) = delete;
    IOrderGateway &operator=(const IOrderGateway &) = delete;
    IOrderGateway(IOrderGateway &&) = delete;
    IOrderGateway &operator=(IOrderGateway &&) = delete;

    // Human-readable name of what this gateway talks to, for the audit record and the
    // window ("eToro (real)", "fake (test)").
    [[nodiscard]] virtual QString name() const = 0;
    // Send an already-validated request. A gateway MUST NOT be the place validation
    // happens — placeOrder that also validates cannot be tested for the case where
    // validation was skipped.
    virtual OrderResult placeOrder(const OrderRequest &request, const Money &stake) = 0;
};

// The gateway the automated suite uses: it records exactly what it was asked to do and
// answers with a configured outcome, including the failure modes that are hard to
// provoke on a real account (a rejection, a timeout with no answer at all).
class FakeOrderGateway : public IOrderGateway
{
public:
    [[nodiscard]] QString name() const override { return QStringLiteral("fake (test)"); }
    OrderResult placeOrder(const OrderRequest &request, const Money &stake) override;

    // What the next placeOrder should answer.
    void setNextResult(const OrderResult &result) { m_next = result; }
    // Every request it was given, in order, with the exact stake.
    [[nodiscard]] const QList<QPair<OrderRequest, Money>> &sent() const { return m_sent; }
    void clear() { m_sent.clear(); }

private:
    OrderResult m_next{true, QStringLiteral("fake-request-id"), QString(), {}};
    QList<QPair<OrderRequest, Money>> m_sent;
};

// The composed, guarded send: validate, then check the armed state, then hand to the
// gateway — and write the audit entry whatever the answer was. This is the ONLY way a
// caller should send, because the three guarantees are only guarantees when none of
// them can be skipped individually.
struct GuardedSendOutcome {
    bool sent = false;
    OrderValidation validation;
    ArmRefusal armRefusal = ArmRefusal::None;
    OrderResult result;
    OrderAuditEntry audited;

    // Why it did not go out, in the words a user should see.
    [[nodiscard]] QString refusalText() const;
};

// The three collaborators are composed ONCE — at the composition root, next to where
// the rest of the services are wired — rather than threaded through every call site.
// A caller that holds a sender cannot reach past it to the gateway, which is the
// property that makes "validated, armed and recorded" true of every order rather than
// of every order somebody remembered to route correctly.
class GuardedOrderSender
{
public:
    // Non-owning: all three must outlive the sender, which the composition root
    // guarantees by constructing them there. Pointers rather than references because a
    // reference member makes the class non-assignable and hides that lifetime
    // (cppcoreguidelines-avoid-const-or-ref-data-members).
    GuardedOrderSender(IOrderGateway *gateway, LiveArm *arm, OrderAudit *audit);

    [[nodiscard]] GuardedSendOutcome send(const OrderRequest &request,
                                          const OrderAmounts &amounts,
                                          const OrderContext &context,
                                          const QDateTime &decidedAt,
                                          const QDateTime &now = QDateTime::currentDateTimeUtc());

    [[nodiscard]] IOrderGateway *gateway() const { return m_gateway; }
    [[nodiscard]] LiveArm *arm() const { return m_arm; }
    [[nodiscard]] OrderAudit *audit() const { return m_audit; }
    // A sender missing a collaborator can send nothing — checked rather than assumed,
    // because "no gateway" must fail closed like every other refusal here.
    [[nodiscard]] bool isUsable() const;

private:
    IOrderGateway *m_gateway = nullptr;
    LiveArm *m_arm = nullptr;
    OrderAudit *m_audit = nullptr;
};

} // namespace trading

#endif // TRADINGAPP_SERVICES_ORDERGATEWAY_H
