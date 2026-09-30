// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/LiveBotExecutor.h"

#include "services/EtoroClient.h"
#include "ui/BotSimRunner.h"

#include <algorithm>
#include <utility>

using trading::LiveMirrorInputs;
using trading::LiveOrderPlan;
using trading::Money;
using trading::PaperClosedTrade;
using trading::PaperTrade;

namespace {

// The daily STAKE cap the armed session carries is 20 × the per-order cap. The binding
// daily rule of REQ-F-076 is the realised LOSS cap (the kill switch), not committed
// stake: with one live position per instrument and two instruments, twenty orders a day
// is far more than the mirror can send, so this bound only ever fires on a defect — and
// LiveArm needs SOME per-day figure or its ledger stays invalid.
constexpr qint64 kDayStakeMultiple = 20;

QString sideWord(bool isBuy)
{
    return isBuy ? QStringLiteral("BUY") : QStringLiteral("SELL");
}

}   // namespace

LiveBotExecutor::LiveBotExecutor(EtoroClient *client, BotSimRunner *runner,
                                 trading::IOrderGateway *gateway, LiveBotSetup setup,
                                 QObject *parent)
    : QObject(parent), m_client(client), m_runner(runner), m_cfg(std::move(setup.config)),
      m_isClientLive(std::move(setup.isClientLive)), m_audit(setup.auditPath),
      m_sender(gateway, &m_arm, &m_audit)
{
    if (!m_isClientLive) {
        // The composition root leaves it here: the client's own answer, which is false
        // for a demo account, for missing keys and under TRADINGAPP_FORCE_SIMULATION.
        m_isClientLive = [client] { return (client != nullptr) && client->config().isLive(); };
    }
    if (m_runner != nullptr) {
        static_cast<void>(connect(m_runner, &BotSimRunner::tradeOpenedDetail, this,
                                  &LiveBotExecutor::onPaperOpened));
        static_cast<void>(
            connect(m_runner, &BotSimRunner::tradeClosed, this, &LiveBotExecutor::onPaperClosed));
        // A disarmed paper bot opens nothing, so nothing is mirrored — the live arming
        // stays as it is (it follows paper events, it does not drive them), but that is
        // worth one line, or an armed mirror that sends nothing looks broken.
        static_cast<void>(connect(m_runner, &BotSimRunner::changed, this, [this] {
            const bool runnerArmed = m_runner->armed();
            if (m_runnerArmedSeen && !runnerArmed && m_arm.isArmed()) {
                emit log(QStringLiteral("LIVE: the paper bot was disarmed — real-money execution "
                                        "stays armed but no new live order can come from a "
                                        "disarmed paper bot"),
                         false);
            }
            m_runnerArmedSeen = runnerArmed;
        }));
        m_runnerArmedSeen = m_runner->armed();
    }
    if (m_client != nullptr) {
        static_cast<void>(connect(m_client, &EtoroClient::positionOpened, this,
                                  &LiveBotExecutor::onPositionOpened));
        static_cast<void>(connect(m_client, &EtoroClient::positionClosed, this,
                                  &LiveBotExecutor::onPositionClosed));
        static_cast<void>(
            connect(m_client, &EtoroClient::portfolioUpdated, this, &LiveBotExecutor::onPortfolio));
        static_cast<void>(
            connect(m_client, &EtoroClient::fxRateUpdated, this, &LiveBotExecutor::onFx));
    }
    static_cast<void>(
        connect(&m_arm, &trading::LiveArm::stateChanged, this, &LiveBotExecutor::changed));
}

QString LiveBotExecutor::armRefusal() const
{
    // Most fundamental reason first, like LiveArm::check: a tripped kill switch is not
    // "not ready", and neither is a book another process is trading.
    if (m_arm.isTripped()) {
        return QStringLiteral("the kill switch is tripped (%1) — clear it explicitly first")
            .arg(m_arm.tripReason());
    }
    if ((m_runner == nullptr) || !m_runner->ownsBook()) {
        return QStringLiteral("this process does not own the paper bot's book, so it is not "
                              "the one making the decisions");
    }
    if (!m_isClientLive()) {
        return QStringLiteral("the broker client is not live — real keys and mode \"real\" "
                              "are required");
    }
    if (!m_cfg.maxPerOrder.isPositive() || !m_cfg.maxDailyLoss.isPositive()) {
        return QStringLiteral("no positive per-order and daily-loss caps are configured — a "
                              "cap of zero sends nothing");
    }
    if (m_eurPerUsd <= 0.0) {
        return QStringLiteral("the EUR/USD rate is not known yet, so the EUR caps cannot be "
                              "priced in the account's currency");
    }
    return {};
}

bool LiveBotExecutor::arm(const QDateTime &now)
{
    const QString refusal = armRefusal();
    if (!refusal.isEmpty()) {
        emit log(QStringLiteral("LIVE ARM refused: %1").arg(refusal), true);
        emit changed();
        return false;
    }
    // The caps travel into the armed state in the ORDER currency at this moment's rate:
    // LiveArm compares them with the stakes the guarded send sees, which are in that
    // currency, and a cap in another currency would compare as unordered — passing the
    // per-order check and failing the daily one, both for the wrong reason.
    const trading::Currency ccy = trading::currencyFromCode(
        m_client != nullptr ? m_client->config().orderCurrency : QStringLiteral("usd"));
    const auto inOrderCurrency = [this, ccy](const Money &capEur) {
        return (ccy == trading::Currency::Eur)
                   ? capEur
                   : Money::fromDouble(capEur.toDouble() / m_eurPerUsd, ccy);
    };
    const Money perOrder = inOrderCurrency(m_cfg.maxPerOrder);
    if (!m_arm.arm(m_cfg.armMinutes, perOrder, perOrder.timesInt(kDayStakeMultiple), now)) {
        emit log(QStringLiteral("LIVE ARM refused by the armed state itself"), true);
        return false;
    }
    m_dayLossCap = inOrderCurrency(m_cfg.maxDailyLoss);
    // The REQ-F-031 verdict is evidence, not a lock (the owner's decision of 2026-09-30,
    // recorded in REQ-F-031 and REQ-F-076): an arming over an unmet record goes through,
    // and the log says exactly what the record has not shown yet, so the person who
    // armed did it in the face of the figures rather than in ignorance of them.
    const trading::LiveReadiness readiness = m_runner->liveReadiness();
    if (!readiness.ready) {
        emit log(QStringLiteral("LIVE ARMED although the paper record is NOT ready for real "
                                "money (REQ-F-031): %1")
                     .arg(readiness.blockers.join(QStringLiteral("; "))),
                 true);
    }
    emit log(QStringLiteral("LIVE ARMED for %1 min: %2 — %3 per order (%4 at %5 EUR/USD), "
                            "%6 (%7) realised daily loss trips the kill switch. Audit: %8")
                 .arg(m_cfg.armMinutes)
                 .arg(m_cfg.symbols.join(QStringLiteral("+")), m_cfg.maxPerOrder.toString(),
                      perOrder.toString())
                 .arg(m_eurPerUsd, 0, 'f', 4)
                 .arg(m_cfg.maxDailyLoss.toString(), m_dayLossCap.toString(), m_audit.path()),
             false);
    return true;
}

void LiveBotExecutor::disarm()
{
    m_arm.disarm();
    emit log(QStringLiteral("LIVE DISARMED — no further real order will be sent; the %1 "
                            "mirrored position(s) stay open and are still closed when their "
                            "paper trades close")
                 .arg(openLiveCountFor(QString())),
             false);
}

void LiveBotExecutor::trip(const QString &reason)
{
    m_arm.trip(reason);
    emit log(QStringLiteral("LIVE KILL SWITCH tripped: %1 — nothing more is sent until it is "
                            "cleared by hand")
                 .arg(reason),
             true);
}

void LiveBotExecutor::clearTrip()
{
    m_arm.clearTrip();
    emit log(QStringLiteral("LIVE kill switch cleared — still NOT armed; arming is its own "
                            "double press"),
             false);
}

bool LiveBotExecutor::isArmed(const QDateTime &now) const
{
    return m_arm.isArmed(now);
}

QString LiveBotExecutor::stateLine(const QDateTime &now) const
{
    const qint32 open = openLiveCountFor(QString());
    const QString realised = (m_day.realized.isValid() && (m_day.date == now.toUTC().date()))
                                 ? m_day.realized.toString()
                                 : QStringLiteral("nothing realised today");
    return QStringLiteral("%1 · day: %2 of %3 loss cap · %4 mirrored (%5 open)")
        .arg(m_arm.stateLine(now), realised,
             m_dayLossCap.isValid() ? m_dayLossCap.toString()
                                    : (m_cfg.maxDailyLoss.isValid() ? m_cfg.maxDailyLoss.toString()
                                                                    : QStringLiteral("no")))
        .arg(m_mirrored.size())
        .arg(open);
}

qint32 LiveBotExecutor::openLiveCountFor(const QString &symbol) const
{
    return static_cast<qint32>(
        std::ranges::count_if(m_mirrored, [&symbol](const LiveMirroredPosition &pos) {
            return !pos.closed && (symbol.isEmpty() || (pos.symbol == symbol));
        }));
}

trading::OrderContext LiveBotExecutor::contextFor(const PaperTrade &trade,
                                                  const LiveOrderPlan &plan) const
{
    trading::OrderContext ctx;
    ctx.accountCurrency = plan.amounts.stake.currency();
    ctx.orderCurrency = ctx.accountCurrency;
    ctx.instrument.instrumentId = trade.instrumentId;
    ctx.instrument.symbol = trade.symbol;
    ctx.leverageLadder = {trade.leverage};
    // The client's rate for the id, else the paper fill's own mid — which came from this
    // client's quote book seconds earlier (sidesFor). The validator needs a positive rate
    // to accept a market order at all; the client's order path re-prices the instrument
    // itself and refuses on its own if it has no live rate by then.
    double rate = (m_client != nullptr) ? m_client->lastRateFor(trade.instrumentId) : 0.0;
    if (rate <= 0.0) {
        rate = trade.effectiveRate();
    }
    ctx.marketRate = rate;
    return ctx;
}

void LiveBotExecutor::onPaperOpened(const PaperTrade &trade)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool armed = m_arm.isArmed(now);
    // While not armed, the twenty-odd out-of-scope paper opens a day are not worth a
    // line each; an IN-scope one still goes to the guarded send, which refuses it as
    // not-armed and RECORDS that — the audit is the record of every attempt.
    if (!armed && !m_cfg.symbols.contains(trade.symbol)) {
        return;
    }
    LiveMirrorInputs in;
    in.trade = trade;
    in.eurPerUsd = m_eurPerUsd;
    in.accountCurrency = trading::currencyFromCode(
        m_client != nullptr ? m_client->config().orderCurrency : QStringLiteral("usd"));
    in.openLivePositionsForSymbol = openLiveCountFor(trade.symbol);
    in.orderCurrencyCap = m_arm.maxPerOrder();
    const LiveOrderPlan plan = trading::liveOrderFor(in, m_cfg);
    if (!plan.ok) {
        if (armed) {
            emit log(QStringLiteral("LIVE REFUSED %1: %2").arg(plan.code, plan.why), false);
        }
        return;
    }
    sendMirror(trade, plan, now);
}

void LiveBotExecutor::sendMirror(const PaperTrade &trade, const LiveOrderPlan &plan,
                                 const QDateTime &now)
{
    const trading::GuardedSendOutcome out =
        m_sender.send(plan.request, plan.amounts, contextFor(trade, plan), now, now);
    if (!out.sent) {
        const QString code = !out.validation.ok() ? out.validation.codes().join(QStringLiteral(","))
                                                  : ((out.armRefusal != trading::ArmRefusal::None)
                                                         ? trading::armRefusalCode(out.armRefusal)
                                                         : QStringLiteral("rejected"));
        emit log(QStringLiteral("LIVE REFUSED %1: %2 — %3").arg(code, out.refusalText(), plan.why),
                 out.armRefusal == trading::ArmRefusal::None);
        return;
    }
    LiveMirroredPosition pos;
    pos.paperId = trade.id;
    pos.symbol = trade.symbol;
    pos.instrumentId = trade.instrumentId;
    pos.isBuy = trade.isBuy;
    pos.stake = plan.amounts.stake;
    pos.requestId = out.result.requestId;
    pos.state = QStringLiteral("confirming…");
    m_mirrored.append(pos);
    emit log(QStringLiteral("LIVE SENT %1 (request %2, paper trade #%3) — %4")
                 .arg(plan.why, out.result.requestId)
                 .arg(trade.id)
                 .arg(out.result.detail),
             false);
    emit changed();
}

void LiveBotExecutor::onPositionOpened(const QString &positionId, qint64 instrumentId, bool isBuy)
{
    // First unmatched mirrored position on this instrument and side, oldest first: the
    // list is append-only in send order, and at most one live position per instrument
    // makes the pairing unambiguous in practice.
    for (LiveMirroredPosition &pos : m_mirrored) {
        if (pos.closed || !pos.positionId.isEmpty() || (pos.instrumentId != instrumentId)
            || (pos.isBuy != isBuy)) {
            continue;
        }
        pos.positionId = positionId;
        pos.state = QStringLiteral("open");
        emit log(QStringLiteral("LIVE OPEN %1 %2 — position %3 (paper trade #%4)")
                     .arg(sideWord(isBuy), pos.symbol, positionId)
                     .arg(pos.paperId),
                 false);
        emit changed();
        return;
    }
}

void LiveBotExecutor::onPaperClosed(const PaperClosedTrade &done)
{
    for (LiveMirroredPosition &pos : m_mirrored) {
        if (pos.closed || (pos.paperId != done.id)) {
            continue;
        }
        if (done.partial) {
            emit log(QStringLiteral("LIVE: paper trade #%1 closed PARTIALLY — the live position "
                                    "%2 is kept whole until the paper trade closes")
                         .arg(done.id)
                         .arg(pos.positionId),
                     false);
            return;
        }
        if (pos.positionId.isEmpty()) {
            // Real money with no handle to close it by: flagged, kept, and shouted —
            // never dropped, because dropping it says "flat" while the risk is open.
            pos.state = QStringLiteral("close by hand");
            emit log(QStringLiteral("LIVE CLOSE needed BY HAND: paper trade #%1 (%2 %3) closed "
                                    "but the broker never named the live position it opened — "
                                    "close it in eToro")
                         .arg(done.id)
                         .arg(sideWord(pos.isBuy), pos.symbol),
                     true);
            emit changed();
            return;
        }
        pos.state = QStringLiteral("closing…");
        emit log(QStringLiteral("LIVE CLOSE %1 %2 position %3 — the paper trade closed (%4)")
                     .arg(sideWord(pos.isBuy), pos.symbol, pos.positionId,
                          trading::closeReasonWord(done.reason)),
                 false);
        emit changed();
        if (m_client != nullptr) {
            m_client->closePosition(pos.positionId);
        }
        return;
    }
}

void LiveBotExecutor::onPositionClosed(bool ok, const QString &message, const QString &positionId)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (LiveMirroredPosition &pos : m_mirrored) {
        if (pos.closed || pos.positionId.isEmpty() || (pos.positionId != positionId)) {
            continue;
        }
        if (!ok) {
            pos.state = QStringLiteral("close by hand");
            emit log(QStringLiteral("LIVE CLOSE FAILED for position %1: %2 — close it in eToro")
                         .arg(positionId, message),
                     true);
            emit changed();
            return;
        }
        realise(pos, positionId, now);
        return;
    }
}

void LiveBotExecutor::realise(LiveMirroredPosition &pos, const QString &positionId,
                              const QDateTime &now)
{
    // The close reply carries no figure; the last polled P/L is the best this process
    // knows. Unknown books nothing — and says so, because a silent zero would let a
    // losing day never reach the cap.
    const auto it = m_profitById.constFind(positionId);
    const Money pnl = (it != m_profitById.constEnd())
                          ? Money::fromDouble(it.value(), pos.stake.currency())
                          : Money();
    m_day = trading::liveDayAfterClose(m_day, now, pnl);
    pos.closed = true;
    pos.state = pnl.isValid() ? QStringLiteral("closed %1").arg(pnl.toString())
                              : QStringLiteral("closed (result unknown)");
    emit log(
        QStringLiteral("LIVE CLOSED %1 %2 position %3: %4 — day realised %5")
            .arg(sideWord(pos.isBuy), pos.symbol, positionId,
                 pnl.isValid() ? pnl.toString()
                               : QStringLiteral("result UNKNOWN (no P/L polled for it)"),
                 m_day.realized.isValid() ? m_day.realized.toString() : QStringLiteral("unknown")),
        false);
    emit changed();
    // Judged on a BOOKED result only: a close whose result is unknown changed nothing, and
    // a kill switch a person cleared by hand must not re-trip on nothing — the next booked
    // loss on a day already at the cap trips it again.
    if (pnl.isValid() && trading::liveDailyLossReached(m_day, now, m_dayLossCap)) {
        trip(QStringLiteral("daily loss cap reached: %1 realised against %2 (%3)")
                 .arg(m_day.realized.toString(), m_dayLossCap.toString(),
                      m_cfg.maxDailyLoss.toString()));
    }
}

void LiveBotExecutor::onPortfolio(const QList<Position> &positions)
{
    for (const Position &p : positions) {
        for (const LiveMirroredPosition &pos : m_mirrored) {
            if (!pos.closed && (pos.positionId == p.positionId)) {
                m_profitById.insert(p.positionId, p.profit);
            }
        }
    }
}

void LiveBotExecutor::onFx(double eurPerUsd)
{
    if (eurPerUsd > 0.0) {
        m_eurPerUsd = eurPerUsd;
    }
}
