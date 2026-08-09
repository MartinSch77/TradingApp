// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_CROWDMODEL_H
#define TRADINGAPP_SERVICES_CROWDMODEL_H

#include "domain/CrowdInference.h"

#include <QHash>
#include <QString>

#include <memory>

// The mock-able crowd-model inference seam (REQ-F-042, Phase 5). One interface, mirroring the
// ICrowdProvider/OrderGateway seams: consumers and tests hold an ICrowdModel and never need the
// real runtime. The ONNX-backed implementation is OPTIONAL AT BUILD TIME — a machine without
// the runtime builds this class as a stub whose available() answers false and whose status()
// names the remedy, so the capability is visibly absent rather than silently broken.
//
// Nothing here trades: a prediction is evidence, and any consumer stays paper/advisory behind
// the deterministic risk rules (REQ-N-005 untouched).
namespace trading::crowd {

class ICrowdModel
{
public:
    ICrowdModel() = default;
    virtual ~ICrowdModel() = default;
    ICrowdModel(const ICrowdModel &) = delete;
    ICrowdModel &operator=(const ICrowdModel &) = delete;
    ICrowdModel(ICrowdModel &&) = delete;
    ICrowdModel &operator=(ICrowdModel &&) = delete;

    // Whether this BUILD can run a model at all (the runtime was present at configure time).
    [[nodiscard]] virtual bool available() const = 0;
    // Whether a model is loaded and scoreable right now.
    [[nodiscard]] virtual bool ready() const = 0;
    // Human words for the current state — "ready", the load refusal, or the missing-runtime
    // remedy. Never empty, so a view can always say WHY nothing is scored.
    [[nodiscard]] virtual QString status() const = 0;
    // The loaded model's own declared contract (empty/not-ok before a successful load).
    [[nodiscard]] virtual CrowdModelMeta meta() const = 0;
    // Load an exported model file. False leaves status() carrying the reason; a failed load
    // never keeps a half-usable session.
    virtual bool load(const QString &path) = 0;
    // Score one input, matched BY NAME against the model's metadata (missing features carry
    // the trainer's medians and are counted in the answer). Errors are results, not crashes.
    [[nodiscard]] virtual CrowdPrediction predict(const QHash<QString, double> &featuresByName) = 0;
};

// The ONNX Runtime implementation. Compiled in every build: with the runtime it executes the
// graph; without it every call answers the same honest "unavailable". The runtime types stay
// behind the Impl pointer so no consumer ever includes an ONNX header.
class OnnxCrowdModel final : public ICrowdModel
{
public:
    OnnxCrowdModel();
    ~OnnxCrowdModel() override;
    OnnxCrowdModel(const OnnxCrowdModel &) = delete;
    OnnxCrowdModel &operator=(const OnnxCrowdModel &) = delete;
    OnnxCrowdModel(OnnxCrowdModel &&) = delete;
    OnnxCrowdModel &operator=(OnnxCrowdModel &&) = delete;

    [[nodiscard]] bool available() const override;
    [[nodiscard]] bool ready() const override;
    [[nodiscard]] QString status() const override;
    [[nodiscard]] CrowdModelMeta meta() const override;
    bool load(const QString &path) override;
    [[nodiscard]] CrowdPrediction predict(const QHash<QString, double> &featuresByName) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; // null until a successful load (and always, in a stub build)
    CrowdModelMeta m_meta;
    QString m_status;
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_CROWDMODEL_H
