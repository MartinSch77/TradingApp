// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/CrowdScore.h"

#include <algorithm>

namespace trading::crowd {

namespace {

// The four weighted families, in report order, resolved from the config. Kept in one place so
// the weights, labels and the single contrarian convention cannot drift between the score and
// its explanation.
struct FamilySpec {
    Source family;
    QString label;
    double weight;
    bool contrarian;
};

QList<FamilySpec> familySpecs(const CrowdScoreConfig &cfg)
{
    return {
        {Source::RetailPositioning, QStringLiteral("retail"), cfg.retailWeight, true},
        {Source::Options, QStringLiteral("options"), cfg.optionsWeight, false},
        {Source::InstitutionalPositioning, QStringLiteral("institutional"),
         cfg.institutionalWeight, false},
        {Source::Social, QStringLiteral("social"), cfg.socialWeight, false},
    };
}

double freshnessFactor(Freshness freshness)
{
    switch (freshness) {
    case Freshness::Live:
        return 1.0;
    case Freshness::Stale:
        return 0.5;   // stale data still counts toward the score but halves its confidence
    case Freshness::Absent:
        break;
    }
    return 0.0;
}

} // namespace

QString CrowdScoreResult::headline() const
{
    if (isEmpty()) {
        return QStringLiteral("Crowd score: no data");
    }
    return QStringLiteral("Crowd score %1%2 (%3) · confidence %4% · coverage %5%")
        .arg(score > 0.0 ? QStringLiteral("+") : QString())
        .arg(score, 0, 'f', 2)
        .arg(direction)
        .arg(qRound(confidence * 100.0))
        .arg(qRound(coverage * 100.0));
}

CrowdScoreResult crowdScore(const QList<ComponentReading> &readings, const CrowdScoreConfig &cfg)
{
    CrowdScoreResult out;
    out.version = cfg.version;

    double totalWeight = 0.0;
    double measuredWeight = 0.0;
    double weightedSum = 0.0;
    double freshWeighted = 0.0;

    for (const FamilySpec &spec : familySpecs(cfg)) {
        totalWeight += spec.weight;

        ScoreComponent component;
        component.label = spec.label;
        component.family = spec.family;
        component.weight = spec.weight;
        component.contrarian = spec.contrarian;

        const auto reading = std::find_if(readings.cbegin(), readings.cend(),
                                          [&spec](const ComponentReading &candidate) {
                                              return candidate.family == spec.family;
                                          });
        const bool measured = (reading != readings.cend()) && reading->measured;
        component.measured = measured;
        if (measured) {
            component.zscore = reading->zscore;
            component.freshness = reading->freshness;
            component.ageSec = reading->ageSec;
            // The ONE documented contrarian flip: a crowd that is heavily long is a bearish tell.
            const double signedZ = spec.contrarian ? -reading->zscore : reading->zscore;
            const double scaled = std::clamp(signedZ, -cfg.clampZ, cfg.clampZ) / cfg.clampZ;
            component.contribution = spec.weight * scaled;

            weightedSum += component.contribution;
            measuredWeight += spec.weight;
            freshWeighted += spec.weight * freshnessFactor(reading->freshness);
            if (reading->freshness == Freshness::Stale) {
                out.warnings.append(spec.label + QStringLiteral(" data is stale"));
            }
            if (reading->ageSec >= 0) {
                out.newestAgeSec = (out.newestAgeSec < 0) ? reading->ageSec
                                                          : std::min(out.newestAgeSec, reading->ageSec);
                out.oldestAgeSec = std::max(out.oldestAgeSec, reading->ageSec);
            }
        } else {
            out.warnings.append(QStringLiteral("no ") + spec.label + QStringLiteral(" data"));
        }
        out.components.append(component);
    }

    out.coverage = (totalWeight > 0.0) ? (measuredWeight / totalWeight) : 0.0;
    // Renormalize over the MEASURED weight, so a partial field yields an honest score at a stated
    // coverage rather than one dragged toward zero by the absent components.
    out.score = (measuredWeight > 0.0) ? (weightedSum / measuredWeight) : 0.0;
    const double freshness = (measuredWeight > 0.0) ? (freshWeighted / measuredWeight) : 0.0;
    out.confidence = out.coverage * freshness;
    out.direction = (out.score > cfg.neutralBand) ? QStringLiteral("bullish")
                    : (out.score < -cfg.neutralBand) ? QStringLiteral("bearish")
                                                     : QStringLiteral("neutral");
    return out;
}

} // namespace trading::crowd
