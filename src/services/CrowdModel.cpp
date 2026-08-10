// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/CrowdModel.h"

#include <QFileInfo>

#ifdef TRADINGAPP_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>

#include <array>
#include <span>
#include <string>
#include <vector>
#endif

namespace trading::crowd {

#ifdef TRADINGAPP_HAS_ONNXRUNTIME

// Everything ONNX lives here, so consumers of the header never see the runtime and a build
// without it compiles the stub half below instead.
struct OnnxCrowdModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "TradingApp"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::string probabilitiesName;
};

OnnxCrowdModel::OnnxCrowdModel() : m_status(QStringLiteral("no model loaded")) {}

OnnxCrowdModel::~OnnxCrowdModel() = default;

bool OnnxCrowdModel::available() const
{
    return true;
}

bool OnnxCrowdModel::load(const QString &path)
{
    // A failed load must leave NO half-usable session — ready() answers from m_impl alone.
    m_impl.reset();
    m_meta = CrowdModelMeta{};
    if (!QFileInfo::exists(path)) {
        m_status = QStringLiteral("model file not found: %1").arg(path);
        return false;
    }
    try {
        auto impl = std::make_unique<Impl>();
        impl->options.SetIntraOpNumThreads(1);
        impl->session = std::make_unique<Ort::Session>(impl->env, path.toUtf8().constData(),
                                                       impl->options);

        // The file's own declared contract — refused before anything is scored.
        const Ort::AllocatorWithDefaultOptions allocator;
        const Ort::ModelMetadata modelMeta = impl->session->GetModelMetadata();
        QHash<QString, QString> props;
        for (const auto &key : modelMeta.GetCustomMetadataMapKeysAllocated(allocator)) {
            const auto value = modelMeta.LookupCustomMetadataMapAllocated(key.get(), allocator);
            if (value != nullptr) {
                props.insert(QString::fromUtf8(key.get()), QString::fromUtf8(value.get()));
            }
        }
        const CrowdModelMeta parsed = crowdModelMetaFromProps(props);
        if (!parsed.ok) {
            m_status = parsed.error;
            return false;
        }

        // The graph must eat exactly the row the metadata describes.
        if (impl->session->GetInputCount() != 1) {
            m_status = QStringLiteral("the graph declares %1 inputs, not the one feature row")
                           .arg(impl->session->GetInputCount());
            return false;
        }
        impl->inputName = impl->session->GetInputNameAllocated(0, allocator).get();
        const auto shape =
            impl->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 2 || (shape.at(1) > 0 && shape.at(1) != parsed.features.size())) {
            m_status = QStringLiteral("the graph expects %1 features but the metadata names %2")
                           .arg(shape.size() == 2 ? shape.at(1) : -1)
                           .arg(parsed.features.size());
            return false;
        }
        // The probabilities output by NAME — the exporter disables zipmap precisely so this
        // is a plain float tensor.
        impl->probabilitiesName.clear();
        for (size_t i = 0; i < impl->session->GetOutputCount(); ++i) {
            const std::string name = impl->session->GetOutputNameAllocated(i, allocator).get();
            if (name == "probabilities") {
                impl->probabilitiesName = name;
            }
        }
        if (impl->probabilitiesName.empty()) {
            m_status = QStringLiteral("the graph has no 'probabilities' output — not a Phase 4 "
                                      "export");
            return false;
        }

        m_impl = std::move(impl);
        m_meta = parsed;
        m_status = QStringLiteral("ready: %1 features, classes %2 (%3 training rows)")
                       .arg(m_meta.features.size())
                       .arg(m_meta.classes.join(QStringLiteral("/")))
                       .arg(m_meta.trainingRows);
        return true;
    } catch (const Ort::Exception &error) {
        m_status = QStringLiteral("ONNX Runtime refused the model: %1")
                       .arg(QString::fromUtf8(error.what()));
        return false;
    }
}

CrowdPrediction OnnxCrowdModel::predict(const QHash<QString, double> &featuresByName)
{
    CrowdPrediction failed;
    if (m_impl == nullptr) {
        failed.error = QStringLiteral("no model loaded (%1)").arg(m_status);
        return failed;
    }
    CrowdFeatureVector row = assembleCrowdFeatures(m_meta, featuresByName);
    if (!row.ok) {
        failed.error = row.error;
        return failed;
    }
    try {
        const Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 2> shape{1, static_cast<int64_t>(row.values.size())};
        const Ort::Value input = Ort::Value::CreateTensor<float>(
            memory, row.values.data(), static_cast<size_t>(row.values.size()), shape.data(),
            shape.size());
        const std::array<const char *, 1> inputNames{m_impl->inputName.c_str()};
        const std::array<const char *, 1> outputNames{m_impl->probabilitiesName.c_str()};
        const auto outputs = m_impl->session->Run(Ort::RunOptions{nullptr}, inputNames.data(),
                                                  &input, 1, outputNames.data(), 1);
        const auto info = outputs.front().GetTensorTypeAndShapeInfo();
        const auto *data = outputs.front().GetTensorData<float>();
        const std::span<const float> answered(data, info.GetElementCount());
        QList<double> probabilities;
        probabilities.reserve(static_cast<qsizetype>(answered.size()));
        for (const float p : answered) {
            probabilities.append(static_cast<double>(p));
        }
        return crowdPredictionFrom(m_meta, probabilities, row.imputed);
    } catch (const Ort::Exception &error) {
        failed.error = QStringLiteral("scoring failed inside ONNX Runtime: %1")
                           .arg(QString::fromUtf8(error.what()));
        return failed;
    }
}

#else // TRADINGAPP_HAS_ONNXRUNTIME

// The stub half: identical interface, honest answers, no runtime anywhere. The struct still
// exists so the unique_ptr's destructor has a complete type.
struct OnnxCrowdModel::Impl {
};

namespace {

QString missingRuntimeStatus()
{
    return QStringLiteral("built without ONNX Runtime — run ./setup.sh ml, then reconfigure "
                          "the build to enable in-app inference");
}

} // namespace

OnnxCrowdModel::OnnxCrowdModel() : m_status(missingRuntimeStatus()) {}

OnnxCrowdModel::~OnnxCrowdModel() = default;

bool OnnxCrowdModel::available() const
{
    return false;
}

bool OnnxCrowdModel::load(const QString &path)
{
    Q_UNUSED(path);
    m_status = missingRuntimeStatus();
    return false;
}

CrowdPrediction OnnxCrowdModel::predict(const QHash<QString, double> &featuresByName)
{
    Q_UNUSED(featuresByName);
    CrowdPrediction failed;
    failed.error = missingRuntimeStatus();
    return failed;
}

#endif // TRADINGAPP_HAS_ONNXRUNTIME

bool OnnxCrowdModel::ready() const
{
    return m_impl != nullptr;
}

QString OnnxCrowdModel::status() const
{
    return m_status;
}

CrowdModelMeta OnnxCrowdModel::meta() const
{
    return m_meta;
}

} // namespace trading::crowd
