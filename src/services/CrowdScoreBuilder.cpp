// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/CrowdScoreBuilder.h"

#include "domain/RollingZScore.h"
#include "services/CrowdStore.h"

namespace trading::crowd {

namespace {

// The representative series for each weighted family, and the sign that orients its z so a
// POSITIVE value is bullish. Retail is +1 (handed in raw; crowdScore applies the contrarian
// flip), options is -1 (a high put/call ratio is bearish). These match the series ids the
// providers emit.
struct FamilySeries {
    Source family;
    QString seriesId;
    double orient;
};

QList<FamilySeries> familySeries()
{
    return {
        {Source::RetailPositioning, QStringLiteral("IG-PCT-LONG"), 1.0},
        {Source::Options, QStringLiteral("PUT-CALL"), -1.0},
        {Source::InstitutionalPositioning, QStringLiteral("COT-ASSET-MGR-NET"), 1.0},
        {Source::Social, QStringLiteral("NET-SENTIMENT"), 1.0},
    };
}

} // namespace

CrowdScoreResult buildCrowdScore(const CrowdStore &store, const QString &instrument,
                                 const QDateTime &now, const CrowdScoreConfig &cfg)
{
    QList<ComponentReading> readings;
    for (const FamilySeries &spec : familySeries()) {
        ComponentReading reading;
        reading.family = spec.family;

        const Observation latest = store.latest(instrument, spec.family, spec.seriesId);
        if (latest.valid) {
            // Normalize the latest value against the history that PRECEDED it — past-only, so no
            // look-ahead. Too little history leaves the component uncalibrated (unmeasured),
            // which lowers coverage rather than faking a zero.
            const QList<double> history = store.seriesValuesBefore(
                instrument, spec.family, spec.seriesId, latest.receivedTime);
            const std::optional<double> z = zScore(latest.value, history, cfg.minHistory);
            if (z.has_value()) {
                reading.measured = true;
                reading.zscore = spec.orient * *z;
                reading.freshness = latest.freshness(now, cfg.staleAfterSec);
                reading.ageSec = latest.ageSeconds(now);
            }
        }
        readings.append(reading);
    }
    return crowdScore(readings, cfg);
}

} // namespace trading::crowd
