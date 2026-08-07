// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/PositionMath.h"

#include <QLocale>
#include <QSet>

#include <cmath>

namespace trading {

qint32 priceDecimals(double price) noexcept
{
    const double a = std::abs(price);
    if (a >= 100.0) {
        return 2;
    }
    if (a >= 10.0) {
        return 3;
    }
    if (a >= 1.0) {
        return 4;
    }
    return 5;
}

double accountValuePerPoint(const Position &p)
{
    if (p.openRate <= 0.0) {
        return 0.0;
    }
    const double notional = p.amount * p.leverage;
    return (notional > 0.0) ? (notional / p.openRate) : p.units;
}

double positionPnl(const Position &p, const Quote &q)
{
    const double close = q.closeRate(p.isBuy);
    if ((close <= 0.0) || (p.openRate <= 0.0)) {
        return 0.0;
    }
    const double move = (p.isBuy ? 1.0 : -1.0) * (close - p.openRate);
    // Units are counted in the instrument's own currency, so the move has to be
    // converted; the value-per-point fallback is already an account-currency figure.
    return (p.units > 0.0) ? (p.units * move * q.conversion(p.isBuy))
                           : (accountValuePerPoint(p) * move);
}

QString slTpAmountText(const Position &p, double rate, double eurPerUsd)
{
    const double perPoint = accountValuePerPoint(p);
    if ((rate <= 0.0) || (perPoint <= 0.0) || (p.openRate <= 0.0)) {
        return {};
    }
    const double usd = perPoint * std::abs(p.openRate - rate);
    return QLocale().toString(usd * ((eurPerUsd > 0.0) ? eurPerUsd : 1.0), 'f', 2);
}

QString slSignedAmountText(const Position &p, double eurPerUsd)
{
    const double rate = p.stopLossRate;
    const double perPoint = accountValuePerPoint(p);
    if ((rate <= 0.0) || (perPoint <= 0.0) || (p.openRate <= 0.0)) {
        return {};
    }
    const double pnl = (p.isBuy ? (perPoint * (rate - p.openRate))
                                : (perPoint * (p.openRate - rate)))
                       * ((eurPerUsd > 0.0) ? eurPerUsd : 1.0);
    return ((pnl < 0.0) ? QStringLiteral("-") : QStringLiteral("+"))
           + QLocale().toString(std::abs(pnl), 'f', 2);
}

QStringList closedSincePreviousIds(const QList<Position> &previous,
                                   const QList<Position> &current)
{
    if (previous.isEmpty()) {
        return {};  // first snapshot: nothing can have disappeared yet
    }
    QSet<QString> open;
    open.reserve(current.size());
    for (const Position &p : current) {
        static_cast<void>(open.insert(p.positionId));
    }
    QStringList gone;
    for (const Position &p : previous) {
        if (!p.positionId.isEmpty() && !open.contains(p.positionId)) {
            gone.append(p.positionId);
        }
    }
    return gone;
}

CloseSuppression suppressClosedPositions(const QList<Position> &reported,
                                         const QHash<QString, qint64> &closedAtMs,
                                         qint64 nowMs, qint64 windowMs)
{
    CloseSuppression out;
    out.visible.reserve(reported.size());

    // Which remembered ids the broker STILL reports. Anything it has stopped reporting is
    // confirmed gone, and the caller can forget it.
    QSet<QString> stillReported;
    for (const Position &p : reported) {
        if (closedAtMs.contains(p.positionId)) {
            static_cast<void>(stillReported.insert(p.positionId));
        }
    }

    for (const Position &p : reported) {
        const auto closed = closedAtMs.constFind(p.positionId);
        if (closed == closedAtMs.constEnd()) {
            out.visible.append(p);          // never closed by us — show it
            continue;
        }
        // Elapsed is computed rather than compared directly so a clock that moved backwards
        // (NTP step, suspend/resume) expires the entry instead of hiding it indefinitely.
        const qint64 elapsed = nowMs - closed.value();
        if ((elapsed < 0) || (elapsed >= windowMs)) {
            // The close did not take: the broker still has it. Show it again and name it —
            // silence here would leave someone believing they were flat.
            out.visible.append(p);
            out.expired.append(p.positionId);
        }
        // else: inside the window and still reported — hidden, which is the whole point.
    }

    for (auto it = closedAtMs.constBegin(); it != closedAtMs.constEnd(); ++it) {
        if (!stillReported.contains(it.key())) {
            out.confirmed.append(it.key());
        }
    }
    // Deterministic order: a QHash iterates arbitrarily, and a log line whose contents
    // reshuffle between runs is not comparable.
    out.expired.sort();
    out.confirmed.sort();
    return out;
}

} // namespace trading
