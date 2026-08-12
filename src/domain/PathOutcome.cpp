// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/PathOutcome.h"

#include <algorithm>
#include <cmath>

namespace trading {

QString pathLabelWord(PathLabel label)
{
    switch (label) {
    case PathLabel::Long:
        return QStringLiteral("long");
    case PathLabel::Short:
        return QStringLiteral("short");
    case PathLabel::NoTrade:
        break;
    }
    return QStringLiteral("no-trade");
}

DirectionalPathResult walkPath(const QList<DailyBar> &bars, double entryPrice, bool isLong,
                               const PathOutcomeConfig &cfg)
{
    DirectionalPathResult out;
    if ((entryPrice <= 0.0) || (cfg.stopFraction <= 0.0)) {
        return out;   // no R defined: nothing safe to walk against
    }
    const double stopDistance = entryPrice * cfg.stopFraction * cfg.stopR;
    const double targetDistance =
        (entryPrice * cfg.stopFraction * cfg.targetR) + (entryPrice * cfg.costBufferFraction);
    const double stopPrice = isLong ? (entryPrice - stopDistance) : (entryPrice + stopDistance);
    const double targetPrice =
        isLong ? (entryPrice + targetDistance) : (entryPrice - targetDistance);

    const qint32 limit = std::min(cfg.maxBars, static_cast<qint32>(bars.size()));
    for (qint32 i = 0; i < limit; ++i) {
        const DailyBar &bar = bars[i];
        const bool stopHit = isLong ? (bar.low <= stopPrice) : (bar.high >= stopPrice);
        const bool targetHit = isLong ? (bar.high >= targetPrice) : (bar.low <= targetPrice);
        // Same-candle ambiguity: the stop is assumed hit FIRST (the design's
        // own conservative rule, shared verbatim with resolveBarAgainstStop).
        if (stopHit) {
            out.barsToResolve = i + 1;
            return out;   // reachedTargetFirst stays false
        }
        if (targetHit) {
            out.reachedTargetFirst = true;
            out.barsToResolve = i + 1;
            return out;
        }
    }
    return out;   // never resolved within maxBars
}

PathLabel resolvePathLabel(const QList<DailyBar> &bars, double entryPrice,
                           const PathOutcomeConfig &cfg)
{
    if (walkPath(bars, entryPrice, true, cfg).reachedTargetFirst) {
        return PathLabel::Long;
    }
    if (walkPath(bars, entryPrice, false, cfg).reachedTargetFirst) {
        return PathLabel::Short;
    }
    return PathLabel::NoTrade;
}

PathLabel resolvePathLabelForPrediction(const Prediction &decision, const QList<Prediction> &later,
                                        const PathOutcomeConfig &cfg)
{
    QList<Prediction> sameSymbol;
    for (const Prediction &row : later) {
        if (row.isValid() && (row.symbol == decision.symbol) && (row.at > decision.at)) {
            sameSymbol.append(row);
        }
    }
    std::sort(sameSymbol.begin(), sameSymbol.end(),
             [](const Prediction &a, const Prediction &b) { return a.at < b.at; });

    QList<DailyBar> bars;
    bars.reserve(sameSymbol.size());
    for (const Prediction &row : sameSymbol) {
        DailyBar bar;
        bar.open = row.price;
        bar.high = row.price;
        bar.low = row.price;
        bar.close = row.price;
        bars.append(bar);
    }
    return resolvePathLabel(bars, decision.price, cfg);
}

} // namespace trading
