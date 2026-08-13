// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/OrderGateway.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

#include <utility>

namespace trading {

namespace {

QString isoOrEmpty(const QDateTime &t)
{
    return t.isValid() ? t.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime fromIso(const QString &s)
{
    return s.isEmpty() ? QDateTime() : QDateTime::fromString(s, Qt::ISODateWithMs);
}

// Money in the record is minor units plus its currency code — the record must not
// re-introduce the floating-point ambiguity the type removed (REQ-N-008).
QJsonObject moneyToJson(const Money &m)
{
    QJsonObject o;
    o.insert(QStringLiteral("minorUnits"), static_cast<double>(m.minorUnits()));
    o.insert(QStringLiteral("currency"), currencyCode(m.currency()));
    return o;
}

Money moneyFromJson(const QJsonObject &o)
{
    const Currency currency = currencyFromCode(o.value(QStringLiteral("currency")).toString());
    if (currency == Currency::Invalid) {
        return {};
    }
    return Money::fromMinorUnits(static_cast<qint64>(o.value(QStringLiteral("minorUnits"))
                                                         .toDouble()),
                                 currency);
}

QString defaultAuditPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty()) {
        return QStringLiteral("order-audit.jsonl");
    }
    static_cast<void>(QDir().mkpath(dir));
    return QDir(dir).filePath(QStringLiteral("order-audit.jsonl"));
}

} // namespace

// ---------------------------------------------------------------------------
// LiveArm
// ---------------------------------------------------------------------------

QString armRefusalCode(ArmRefusal refusal)
{
    switch (refusal) {
    case ArmRefusal::None:
        return QStringLiteral("armed");
    case ArmRefusal::NotArmed:
        return QStringLiteral("not-armed");
    case ArmRefusal::Expired:
        return QStringLiteral("arm-expired");
    case ArmRefusal::Tripped:
        return QStringLiteral("kill-switch");
    case ArmRefusal::OverOrderCap:
        return QStringLiteral("arm-order-cap");
    case ArmRefusal::OverDayCap:
        return QStringLiteral("arm-day-cap");
    }
    std::unreachable();  // every ArmRefusal is handled above
}

LiveArm::LiveArm(QObject *parent)
    : QObject(parent)
{
}

bool LiveArm::arm(qint32 minutes, const Money &maxPerOrder, const Money &maxPerDay,
                  const QDateTime &now)
{
    if (m_tripped || (minutes <= 0)) {
        return false;
    }
    m_armedUntil = now.toUTC().addSecs(static_cast<qint64>(minutes) * 60);
    m_maxPerOrder = maxPerOrder;
    m_maxPerDay = maxPerDay;
    if (!m_committedDate.isValid()) {
        m_committedDate = now.toUTC().date();
        m_committedToday = maxPerDay.isValid() ? Money::zero(maxPerDay.currency()) : Money();
    }
    emit stateChanged();
    return true;
}

void LiveArm::disarm()
{
    m_armedUntil = QDateTime();
    emit stateChanged();
}

void LiveArm::trip(const QString &reason)
{
    m_tripped = true;
    m_tripReason = reason;
    m_armedUntil = QDateTime();
    emit trippedChanged(true, reason);
    emit stateChanged();
}

void LiveArm::clearTrip()
{
    if (!m_tripped) {
        return;
    }
    m_tripped = false;
    m_tripReason.clear();
    emit trippedChanged(false, QString());
    emit stateChanged();
}

bool LiveArm::isArmed(const QDateTime &now) const
{
    return !m_tripped && m_armedUntil.isValid() && (now.toUTC() < m_armedUntil);
}

ArmRefusal LiveArm::check(const Money &stake, const QDateTime &now) const
{
    // Most fundamental reason first: a tripped kill switch is not "expired", and an
    // expired window is not "unarmed" — the three mean different things to whoever
    // reads the refusal.
    if (m_tripped) {
        return ArmRefusal::Tripped;
    }
    if (!m_armedUntil.isValid()) {
        return ArmRefusal::NotArmed;
    }
    if (now.toUTC() >= m_armedUntil) {
        return ArmRefusal::Expired;
    }
    if (m_maxPerOrder.isValid() && (stake > m_maxPerOrder)) {
        return ArmRefusal::OverOrderCap;
    }
    if (m_maxPerDay.isValid()) {
        const bool sameDay = (m_committedDate == now.toUTC().date());
        const Money committed = (sameDay && m_committedToday.isValid())
                                    ? m_committedToday
                                    : Money::zero(m_maxPerDay.currency());
        const Money after = committed + stake;
        if (!after.isValid() || (after > m_maxPerDay)) {
            return ArmRefusal::OverDayCap;
        }
    }
    return ArmRefusal::None;
}

void LiveArm::recordCommitted(const Money &stake, const QDateTime &now)
{
    const QDate today = now.toUTC().date();
    if (m_committedDate != today) {
        m_committedDate = today;
        m_committedToday = stake.isValid() ? Money::zero(stake.currency()) : Money();
    }
    if (!m_committedToday.isValid()) {
        m_committedToday = stake.isValid() ? Money::zero(stake.currency()) : Money();
    }
    m_committedToday += stake;
    emit stateChanged();
}

void LiveArm::applyCapsTo(OrderContext &context) const
{
    context.maxStakePerOrder = m_maxPerOrder;
    context.maxStakePerDay = m_maxPerDay;
    context.committedToday = m_committedToday;
}

QString LiveArm::stateLine(const QDateTime &now) const
{
    if (m_tripped) {
        return QStringLiteral("KILL SWITCH tripped — %1 (clear it explicitly to arm again)")
            .arg(m_tripReason.isEmpty() ? QStringLiteral("no reason given") : m_tripReason);
    }
    if (!isArmed(now)) {
        return QStringLiteral("not armed — live orders are refused");
    }
    const qint64 secs = now.toUTC().secsTo(m_armedUntil);
    return QStringLiteral("ARMED for %1 min · max %2 per order · max %3 per day · %4 committed "
                          "today")
        .arg((secs + 59) / 60)
        .arg(m_maxPerOrder.isValid() ? m_maxPerOrder.toString() : QStringLiteral("no cap"),
             m_maxPerDay.isValid() ? m_maxPerDay.toString() : QStringLiteral("no cap"),
             m_committedToday.isValid() ? m_committedToday.toString() : QStringLiteral("nothing"));
}

// ---------------------------------------------------------------------------
// OrderAuditEntry / OrderAudit
// ---------------------------------------------------------------------------

QJsonObject OrderAuditEntry::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("decidedAt"), isoOrEmpty(decidedAt));
    o.insert(QStringLiteral("sentAt"), isoOrEmpty(sentAt));
    o.insert(QStringLiteral("requestId"), requestId);
    o.insert(QStringLiteral("outcome"), outcome);
    o.insert(QStringLiteral("detail"), detail);
    o.insert(QStringLiteral("instrumentId"), static_cast<double>(instrumentId));
    o.insert(QStringLiteral("symbol"), symbol);
    o.insert(QStringLiteral("isBuy"), isBuy);
    o.insert(QStringLiteral("stake"), moneyToJson(stake));
    o.insert(QStringLiteral("leverage"), leverage);
    o.insert(QStringLiteral("stopLoss"), moneyToJson(stopLoss));
    o.insert(QStringLiteral("takeProfit"), moneyToJson(takeProfit));
    o.insert(QStringLiteral("triggerRate"), triggerRate);
    o.insert(QStringLiteral("orderCurrency"), currencyCode(orderCurrency));
    o.insert(QStringLiteral("accountCurrency"), currencyCode(accountCurrency));
    o.insert(QStringLiteral("fingerprint"), fingerprint());
    return o;
}

OrderAuditEntry OrderAuditEntry::fromJson(const QJsonObject &o)
{
    OrderAuditEntry e;
    e.decidedAt = fromIso(o.value(QStringLiteral("decidedAt")).toString());
    e.sentAt = fromIso(o.value(QStringLiteral("sentAt")).toString());
    e.requestId = o.value(QStringLiteral("requestId")).toString();
    e.outcome = o.value(QStringLiteral("outcome")).toString();
    e.detail = o.value(QStringLiteral("detail")).toString();
    e.instrumentId = static_cast<qint64>(o.value(QStringLiteral("instrumentId")).toDouble());
    e.symbol = o.value(QStringLiteral("symbol")).toString();
    e.isBuy = o.value(QStringLiteral("isBuy")).toBool(true);
    e.stake = moneyFromJson(o.value(QStringLiteral("stake")).toObject());
    e.leverage = o.value(QStringLiteral("leverage")).toDouble(1.0);
    e.stopLoss = moneyFromJson(o.value(QStringLiteral("stopLoss")).toObject());
    e.takeProfit = moneyFromJson(o.value(QStringLiteral("takeProfit")).toObject());
    e.triggerRate = o.value(QStringLiteral("triggerRate")).toDouble();
    e.orderCurrency = currencyFromCode(o.value(QStringLiteral("orderCurrency")).toString());
    e.accountCurrency = currencyFromCode(o.value(QStringLiteral("accountCurrency")).toString());
    return e;
}

QString OrderAuditEntry::fingerprint() const
{
    // Only the fields that describe the ORDER, never the timestamps: the question this
    // digest answers is "is this the same order", which must stay true for a retry.
    const QString canonical = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                                  .arg(instrumentId)
                                  .arg(isBuy ? QStringLiteral("buy") : QStringLiteral("sell"),
                                       QString::number(stake.minorUnits()),
                                       currencyCode(stake.currency()))
                                  .arg(leverage, 0, 'f', 2)
                                  .arg(stopLoss.minorUnits())
                                  .arg(takeProfit.minorUnits())
                                  .arg(triggerRate, 0, 'f', 6)
                                  .arg(currencyCode(orderCurrency));
    const QByteArray digest =
        QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QString::fromLatin1(digest);
}

OrderAudit::OrderAudit(const QString &path)
    : m_path(path.isEmpty() ? defaultAuditPath() : path)
{
}

bool OrderAudit::append(const OrderAuditEntry &entry)
{
    // Append with an immediate flush. A QSaveFile would be wrong here: it rewrites,
    // and rewriting is how a crash loses the record of the order that caused it.
    QFile file(m_path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    const QByteArray line =
        QJsonDocument(entry.toJson()).toJson(QJsonDocument::Compact) + QByteArrayLiteral("\n");
    const bool written = (file.write(line) == line.size());
    file.flush();
    file.close();
    return written;
}

QList<OrderAuditEntry> OrderAudit::readAll() const
{
    QList<OrderAuditEntry> out;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (doc.isObject()) {
            out.append(OrderAuditEntry::fromJson(doc.object()));
        }
        // A malformed line is SKIPPED rather than aborting the read: a truncated last
        // line (the crash case) must not hide the entries before it.
    }
    return out;
}

QList<OrderAuditEntry> OrderAudit::withFingerprint(const QString &fingerprint) const
{
    QList<OrderAuditEntry> out;
    for (const OrderAuditEntry &entry : readAll()) {
        if (entry.fingerprint() == fingerprint) {
            out.append(entry);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The gateways and the guarded send
// ---------------------------------------------------------------------------

OrderResult FakeOrderGateway::placeOrder(const OrderRequest &request, const Money &stake)
{
    m_sent.append({request, stake});
    return m_next;
}

QString GuardedSendOutcome::refusalText() const
{
    if (sent) {
        return {};
    }
    if (!validation.ok()) {
        return validation.summary();
    }
    if (armRefusal != ArmRefusal::None) {
        switch (armRefusal) {
        case ArmRefusal::Tripped:
            return QStringLiteral("The kill switch is tripped — clear it explicitly before any "
                                  "order can be sent.");
        case ArmRefusal::NotArmed:
            return QStringLiteral("Live trading is not armed.");
        case ArmRefusal::Expired:
            return QStringLiteral("The armed window has expired — arm again deliberately.");
        case ArmRefusal::OverOrderCap:
            return QStringLiteral("This order is larger than the per-order cap the armed "
                                  "session was granted under.");
        case ArmRefusal::OverDayCap:
            return QStringLiteral("This order would exceed the daily cap the armed session was "
                                  "granted under.");
        case ArmRefusal::None:
            break;
        }
    }
    return result.detail.isEmpty() ? QStringLiteral("The broker did not accept the order.")
                                   : result.detail;
}

GuardedOrderSender::GuardedOrderSender(IOrderGateway *gateway, LiveArm *arm, OrderAudit *audit)
    : m_gateway(gateway)
    , m_arm(arm)
    , m_audit(audit)
{
}

bool GuardedOrderSender::isUsable() const
{
    return (m_gateway != nullptr) && (m_arm != nullptr) && (m_audit != nullptr);
}

GuardedSendOutcome GuardedOrderSender::send(const OrderRequest &request,
                                            const OrderAmounts &amounts,
                                            const OrderContext &context,
                                            const QDateTime &decidedAt, const QDateTime &now)
{
    GuardedSendOutcome out;
    if (!isUsable()) {
        // Fail CLOSED: a half-composed sender sends nothing and says so.
        out.validation.problems.append(
            OrderProblem{QStringLiteral("no-gateway"),
                         QStringLiteral("This build has no order gateway composed.")});
        return out;
    }
    // The audit entry is built FIRST, from what was asked for, so that every exit path
    // below has something to record. An entry written only on success is a record of
    // exactly the cases nobody needs to investigate.
    out.audited.decidedAt = decidedAt;
    out.audited.instrumentId = (request.instrumentId != 0) ? request.instrumentId
                                                           : context.instrument.instrumentId;
    out.audited.symbol = context.instrument.symbol;
    out.audited.isBuy = request.isBuy;
    out.audited.stake = amounts.stake;
    out.audited.leverage = request.leverage;
    out.audited.stopLoss = amounts.stopLoss;
    out.audited.takeProfit = amounts.takeProfit;
    out.audited.triggerRate = request.triggerRate;
    out.audited.orderCurrency = context.orderCurrency;
    out.audited.accountCurrency = context.accountCurrency;

    // The armed session's caps and the validator's caps are the SAME caps — a context
    // carrying different numbers would make one of the two decorative.
    OrderContext guarded = context;
    m_arm->applyCapsTo(guarded);

    out.validation = validateOrderRequest(request, amounts, guarded);
    if (!out.validation.ok()) {
        out.audited.outcome = QStringLiteral("refused-validation");
        out.audited.detail = out.validation.codes().join(QStringLiteral(","));
        static_cast<void>(m_audit->append(out.audited));
        return out;
    }

    out.armRefusal = m_arm->check(amounts.stake, now);
    if (out.armRefusal != ArmRefusal::None) {
        out.audited.outcome = QStringLiteral("refused-arm");
        out.audited.detail = armRefusalCode(out.armRefusal);
        static_cast<void>(m_audit->append(out.audited));
        return out;
    }

    out.result = m_gateway->placeOrder(request, amounts.stake);
    out.audited.sentAt = now.toUTC();
    out.audited.requestId = out.result.requestId;
    out.sent = out.result.accepted;
    out.audited.outcome = out.result.accepted ? QStringLiteral("sent")
                                              : QStringLiteral("rejected");
    out.audited.detail = out.result.detail;
    if (out.result.accepted) {
        m_arm->recordCommitted(amounts.stake, now);
    }
    static_cast<void>(m_audit->append(out.audited));
    return out;
}

} // namespace trading
