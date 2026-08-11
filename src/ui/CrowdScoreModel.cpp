// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/CrowdScoreModel.h"

#include <QVariantMap>

namespace trading::ui {

CrowdScoreModel::CrowdScoreModel(QObject *parent) : QObject(parent) {}

void CrowdScoreModel::setResult(const trading::crowd::CrowdScoreResult &result)
{
    m_result = result;
    Q_EMIT changed();
}

QVariantList CrowdScoreModel::components() const
{
    // One map per factor, so a view can show WHAT moved the score and by how much — a missing
    // factor is present with measured=false rather than dropped, so "no options data" is visible
    // rather than an unexplained gap.
    QVariantList out;
    out.reserve(m_result.components.size());
    for (const trading::crowd::ScoreComponent &component : m_result.components) {
        QVariantMap map;
        map.insert(QStringLiteral("label"), component.label);
        map.insert(QStringLiteral("measured"), component.measured);
        map.insert(QStringLiteral("contrarian"), component.contrarian);
        map.insert(QStringLiteral("weight"), component.weight);
        map.insert(QStringLiteral("zscore"), component.zscore);
        map.insert(QStringLiteral("contribution"), component.contribution);
        map.insert(QStringLiteral("freshness"), trading::crowd::freshnessWord(component.freshness));
        out.append(map);
    }
    return out;
}

} // namespace trading::ui
