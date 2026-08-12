// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/YahooChartParser.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>

namespace trading {

QJsonObject yahooChartResult(const QJsonDocument &doc)
{
    return doc.object()
        .value(QStringLiteral("chart"))
        .toObject()
        .value(QStringLiteral("result"))
        .toArray()
        .first()
        .toObject();
}

QList<double> yahooCloses(const QJsonObject &chartResult, bool positiveOnly)
{
    const QJsonArray closeArr = chartResult.value(QStringLiteral("indicators"))
                                    .toObject()
                                    .value(QStringLiteral("quote"))
                                    .toArray()
                                    .first()
                                    .toObject()
                                    .value(QStringLiteral("close"))
                                    .toArray();
    QList<double> closes;
    closes.reserve(closeArr.size());
    for (const auto &v : closeArr) {  // QJsonValueConstRef, no conversion
        if (v.isDouble() && (!positiveOnly || (v.toDouble() > 0.0))) {
            closes.append(v.toDouble());
        }
    }
    return closes;
}

CandleColumns yahooOhlc(const QJsonObject &chartResult)
{
    const QJsonObject quote = chartResult.value(QStringLiteral("indicators"))
                                  .toObject()
                                  .value(QStringLiteral("quote"))
                                  .toArray()
                                  .first()
                                  .toObject();
    const auto column = [&quote](const char *name) {
        const QJsonArray arr = quote.value(QLatin1StringView(name)).toArray();
        QList<double> out;
        out.reserve(arr.size());
        for (const auto &v : arr) {   // QJsonValueConstRef, no conversion
            // A null minute becomes 0.0 and is dropped by candlesFrom's drawable() test.
            // Kept in place rather than skipped, because skipping here would shift this
            // column against the other three — the misalignment yahooBars exists to avoid.
            out.append(v.isDouble() ? v.toDouble() : 0.0);
        }
        return out;
    };
    return CandleColumns{column("open"), column("high"), column("low"), column("close")};
}

QList<double> yahooMetaSessionChange(const QJsonObject &chartResult)
{
    const QJsonObject meta = chartResult.value(QStringLiteral("meta")).toObject();
    const QJsonValue nowV = meta.value(QStringLiteral("regularMarketPrice"));
    const QJsonValue prevV = meta.value(QStringLiteral("previousClose"));
    if (!nowV.isDouble() || !prevV.isDouble()) {
        return {};
    }
    const double now = nowV.toDouble();
    const double prev = prevV.toDouble();
    // A non-positive base would make sessionChangePct divide by it; it already guards, but
    // emitting a series it must reject is worse than emitting nothing.
    if ((now <= 0.0) || (prev <= 0.0)) {
        return {};
    }
    return {prev, now};
}

VolumeSeries yahooBars(const QJsonObject &chartResult)
{
    const QJsonObject quote = chartResult.value(QStringLiteral("indicators"))
                                  .toObject()
                                  .value(QStringLiteral("quote"))
                                  .toArray()
                                  .first()
                                  .toObject();
    const QJsonArray closeArr = quote.value(QStringLiteral("close")).toArray();
    const QJsonArray volumeArr = quote.value(QStringLiteral("volume")).toArray();
    VolumeSeries out;
    const qsizetype bars = std::min(closeArr.size(), volumeArr.size());
    out.closes.reserve(bars);
    out.volumes.reserve(bars);
    for (qsizetype i = 0; i < bars; ++i) {
        const QJsonValue close = closeArr.at(i);
        const QJsonValue volume = volumeArr.at(i);
        if (!close.isDouble() || !volume.isDouble()) {
            continue;   // an empty minute: drop the whole bar, never half of one
        }
        if ((close.toDouble() <= 0.0) || (volume.toDouble() <= 0.0)) {
            continue;
        }
        out.closes.append(close.toDouble());
        out.volumes.append(volume.toDouble());
    }
    return out;
}

} // namespace trading
