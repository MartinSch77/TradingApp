// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_CROWDINFERENCE_H
#define TRADINGAPP_DOMAIN_CROWDINFERENCE_H

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// The PURE half of the crowd-model inference (REQ-F-042, Phase 5): the contract embedded in a
// Phase 4 model file, the by-name feature assembly against it, and the shaping of the answered
// probabilities. Everything here is testable without the ONNX runtime, a file or a network —
// the services-layer OnnxCrowdModel supplies only the graph execution.
//
// The metadata is the point: the trainer embedded the feature names, the imputation values and
// the class order INTO the model precisely so a consumer cannot pair columns by position or
// invent a fill-in — both drift silently; a name mismatch is refused out loud.
namespace trading::crowd {

// What a Phase 4 export declares about itself (train_crowd_model.py writes these as ONNX
// custom metadata). `ok = false` carries the reason a file was refused instead.
struct CrowdModelMeta {
    bool ok = false;
    QString error;
    QStringList features;   // ordered; inputs are matched BY NAME against this list
    QList<double> medians;  // the trainer's imputation value per feature, same length
    QStringList classes;    // the label of each probability column, in the graph's order
    qint32 manifestVersion = 0;
    QString instrument;
    qint32 horizonDays = 0;
    double deadZonePct = 0.0;
    qint64 trainingRows = 0;
};

// Parse the metadata key/value map. Refused with a named reason when the contract keys are
// absent or unparsable, when the medians do not pair 1:1 with the features, or when the class
// list is empty — a model that cannot say what it eats or answers must not be scored.
[[nodiscard]] CrowdModelMeta crowdModelMetaFromProps(const QHash<QString, QString> &props);

// One assembled input row in the metadata's feature order. `imputed` counts the features the
// caller did NOT supply (or supplied as non-finite): they carry the trainer's median, exactly
// as training did — and the count is reported because a prediction made mostly of fill-ins is
// a weaker claim than one made of measurements.
struct CrowdFeatureVector {
    bool ok = false;
    QString error;
    QList<float> values;
    qint32 imputed = 0;
};

[[nodiscard]] CrowdFeatureVector assembleCrowdFeatures(const CrowdModelMeta &meta,
                                                       const QHash<QString, double> &byName);

// The shaped answer: probabilities labelled by the METADATA's classes, never by an assumed
// column order. Refused when the answer does not form a probability distribution over the
// stated classes — repaired numbers would be an invented opinion.
struct CrowdPrediction {
    bool ok = false;
    QString error;
    QStringList classes;
    QList<double> probabilities;  // aligned with `classes`
    QString topClass;
    double topProbability = 0.0;
    qint32 imputed = 0;           // carried through from the assembled input
};

[[nodiscard]] CrowdPrediction crowdPredictionFrom(const CrowdModelMeta &meta,
                                                  const QList<double> &probabilities,
                                                  qint32 imputed);

} // namespace trading::crowd

#endif // TRADINGAPP_DOMAIN_CROWDINFERENCE_H
