// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/CrowdInference.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <cmath>
#include <optional>

namespace trading::crowd {

namespace {

// One metadata value parsed as a JSON array, or nothing — the caller names the key in its
// refusal so the reason points at the field, not just at "the metadata".
std::optional<QJsonArray> jsonArrayProp(const QHash<QString, QString> &props, const QString &key)
{
    if (!props.contains(key)) {
        return std::nullopt;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(props.value(key).toUtf8());
    if (!doc.isArray()) {
        return std::nullopt;
    }
    return doc.array();
}

CrowdModelMeta refused(const QString &why)
{
    CrowdModelMeta meta;
    meta.error = why;
    return meta;
}

} // namespace

CrowdModelMeta crowdModelMetaFromProps(const QHash<QString, QString> &props)
{
    const auto features = jsonArrayProp(props, QStringLiteral("feature_names"));
    if (!features.has_value()) {
        return refused(QStringLiteral("model metadata lacks a readable feature_names list — "
                                      "columns cannot be matched by name, so it is not scored"));
    }
    const auto medians = jsonArrayProp(props, QStringLiteral("imputation_medians"));
    if (!medians.has_value()) {
        return refused(QStringLiteral("model metadata lacks readable imputation_medians — a "
                                      "missing input would need an invented fill-in"));
    }
    const auto classes = jsonArrayProp(props, QStringLiteral("classes"));
    if (!classes.has_value()) {
        return refused(QStringLiteral("model metadata lacks a readable classes list — the "
                                      "probability columns would be unlabelled"));
    }

    CrowdModelMeta meta;
    for (const auto &value : *features) {
        if (!value.isString() || value.toString().isEmpty()) {
            return refused(QStringLiteral("feature_names holds a non-name entry"));
        }
        meta.features.append(value.toString());
    }
    for (const auto &value : *medians) {
        if (!value.isDouble()) {
            return refused(QStringLiteral("imputation_medians holds a non-number entry"));
        }
        meta.medians.append(value.toDouble());
    }
    for (const auto &value : *classes) {
        if (!value.isString() || value.toString().isEmpty()) {
            return refused(QStringLiteral("classes holds a non-name entry"));
        }
        meta.classes.append(value.toString());
    }
    if (meta.features.isEmpty()) {
        return refused(QStringLiteral("the model declares no features"));
    }
    if (meta.medians.size() != meta.features.size()) {
        return refused(QStringLiteral("imputation_medians (%1) do not pair with feature_names "
                                      "(%2) — the imputation contract is broken")
                           .arg(meta.medians.size())
                           .arg(meta.features.size()));
    }
    if (meta.classes.isEmpty()) {
        return refused(QStringLiteral("the model declares no classes"));
    }

    meta.manifestVersion = props.value(QStringLiteral("manifest_version")).toInt();
    meta.instrument = props.value(QStringLiteral("instrument"));
    meta.horizonDays = props.value(QStringLiteral("horizon_days")).toInt();
    meta.deadZonePct = props.value(QStringLiteral("dead_zone_pct")).toDouble();
    meta.trainingRows = props.value(QStringLiteral("training_rows")).toLongLong();
    meta.ok = true;
    return meta;
}

CrowdFeatureVector assembleCrowdFeatures(const CrowdModelMeta &meta,
                                         const QHash<QString, double> &byName)
{
    CrowdFeatureVector out;
    if (!meta.ok) {
        out.error = QStringLiteral("no usable model metadata: %1").arg(meta.error);
        return out;
    }
    out.values.reserve(meta.features.size());
    for (qsizetype i = 0; i < meta.features.size(); ++i) {
        const auto it = byName.constFind(meta.features.at(i));
        // A non-finite value is a measurement that never happened — treated exactly like a
        // missing one (the trainer's median), never fed to the graph as NaN.
        if (it != byName.constEnd() && std::isfinite(it.value())) {
            out.values.append(static_cast<float>(it.value()));
        } else {
            out.values.append(static_cast<float>(meta.medians.at(i)));
            ++out.imputed;
        }
    }
    out.ok = true;
    return out;
}

CrowdPrediction crowdPredictionFrom(const CrowdModelMeta &meta,
                                    const QList<double> &probabilities, qint32 imputed)
{
    CrowdPrediction out;
    out.imputed = imputed;
    if (!meta.ok) {
        out.error = QStringLiteral("no usable model metadata: %1").arg(meta.error);
        return out;
    }
    if (probabilities.size() != meta.classes.size()) {
        out.error = QStringLiteral("the graph answered %1 probabilities for %2 declared "
                                   "classes — the columns cannot be labelled")
                        .arg(probabilities.size())
                        .arg(meta.classes.size());
        return out;
    }
    double sum = 0.0;
    qsizetype top = 0;
    for (qsizetype i = 0; i < probabilities.size(); ++i) {
        const double p = probabilities.at(i);
        if (!std::isfinite(p) || p < -1e-6 || p > 1.0 + 1e-6) {
            out.error = QStringLiteral("probability %1 for %2 is not a probability")
                            .arg(p)
                            .arg(meta.classes.at(i));
            return out;
        }
        sum += p;
        if (p > probabilities.at(top)) {
            top = i;
        }
    }
    // Float noise is tolerated; anything further off is not a distribution and repairing it
    // (renormalising) would manufacture an opinion the graph never gave.
    if (std::abs(sum - 1.0) > 0.02) {
        out.error = QStringLiteral("the answered values sum to %1, not a distribution").arg(sum);
        return out;
    }
    out.classes = meta.classes;
    out.probabilities = probabilities;
    out.topClass = meta.classes.at(top);
    out.topProbability = probabilities.at(top);
    out.ok = true;
    return out;
}

} // namespace trading::crowd
