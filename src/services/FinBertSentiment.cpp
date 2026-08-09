// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/FinBertSentiment.h"

#include <QFile>
#include <QFileInfo>

#include <cmath>

#ifdef TRADINGAPP_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>

#include <array>
#include <span>
#include <string>
#include <vector>
#endif

namespace trading::crowd {

namespace {

// The classifier input is a headline, not a filing: 64 tokens hold any headline whole, and a
// longer text is truncated with the framing kept intact (wordPieceEncode's rule).
constexpr qint32 kMaxTokens = 64;

QStringList fileLines(const QString &path, bool *ok)
{
    QFile file(path);
    *ok = file.open(QIODevice::ReadOnly | QIODevice::Text);
    if (!*ok) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
}

} // namespace

#ifdef TRADINGAPP_HAS_ONNXRUNTIME

struct FinBertSentiment::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "TradingApp"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNames;   // the graph's own declared inputs, fed by name
    std::string outputName;
};

namespace {

// The text-side contract beside the graph: the model's own vocabulary and the labels that
// give its columns meaning. Read and validated BEFORE any runtime work — every missing or
// meaningless piece is a named refusal.
struct SentimentContract {
    bool ok = false;
    QString error;
    WordPieceVocab vocab;
    QStringList labels;
    qint32 positive = -1;
    qint32 negative = -1;
};

SentimentContract readContract(const QString &directory)
{
    SentimentContract contract;
    bool ok = false;
    const QStringList vocabLines = fileLines(directory + QStringLiteral("/vocab.txt"), &ok);
    if (!ok) {
        contract.error = QStringLiteral("vocab.txt is missing beside the model — encoding "
                                        "against a guessed vocabulary would score noise");
        return contract;
    }
    contract.vocab = wordPieceVocabFromLines(vocabLines);
    if (!contract.vocab.ok) {
        contract.error = contract.vocab.error;
        return contract;
    }
    const QStringList labelLines = fileLines(directory + QStringLiteral("/labels.txt"), &ok);
    for (const QString &line : labelLines) {
        if (!line.trimmed().isEmpty()) {
            contract.labels.append(line.trimmed());
        }
    }
    if (!ok || contract.labels.isEmpty()) {
        contract.error = QStringLiteral("labels.txt is missing beside the model — the "
                                        "probability columns would be unlabelled");
        return contract;
    }
    contract.positive = static_cast<qint32>(
        contract.labels.indexOf(QStringLiteral("positive"), 0, Qt::CaseInsensitive));
    contract.negative = static_cast<qint32>(
        contract.labels.indexOf(QStringLiteral("negative"), 0, Qt::CaseInsensitive));
    if (contract.positive < 0 || contract.negative < 0) {
        contract.error = QStringLiteral("labels.txt (%1) does not name positive and negative "
                                        "— a net sentiment cannot be read from it")
                             .arg(contract.labels.join(QStringLiteral("/")));
        return contract;
    }
    contract.ok = true;
    return contract;
}

} // namespace

FinBertSentiment::FinBertSentiment() : m_status(QStringLiteral("no model loaded")) {}

FinBertSentiment::~FinBertSentiment() = default;

bool FinBertSentiment::available()
{
    return true;
}

bool FinBertSentiment::load(const QString &directory)
{
    m_impl.reset();
    m_vocab = WordPieceVocab{};
    m_labels.clear();
    const QString modelPath = directory + QStringLiteral("/model.onnx");
    if (!QFileInfo::exists(modelPath)) {
        m_status = QStringLiteral("no model.onnx in %1 (tools/ml/export_finbert.py exports "
                                  "one)").arg(directory);
        return false;
    }
    const SentimentContract contract = readContract(directory);
    if (!contract.ok) {
        m_status = contract.error;
        return false;
    }

    try {
        auto impl = std::make_unique<Impl>();
        impl->options.SetIntraOpNumThreads(1);
        impl->session = std::make_unique<Ort::Session>(impl->env,
                                                       modelPath.toUtf8().constData(),
                                                       impl->options);
        const Ort::AllocatorWithDefaultOptions allocator;
        bool hasIds = false;
        for (size_t i = 0; i < impl->session->GetInputCount(); ++i) {
            const std::string name = impl->session->GetInputNameAllocated(i, allocator).get();
            // Feed only inputs this runner knows how to fill; an alien one is a refusal.
            if (name != "input_ids" && name != "attention_mask" && name != "token_type_ids") {
                m_status = QStringLiteral("the graph wants an input this runner cannot "
                                          "supply: %1").arg(QString::fromStdString(name));
                return false;
            }
            hasIds = hasIds || name == "input_ids";
            impl->inputNames.push_back(name);
        }
        if (!hasIds || impl->session->GetOutputCount() < 1) {
            m_status = QStringLiteral("the graph is not a text classifier (no input_ids "
                                      "input or no output)");
            return false;
        }
        impl->outputName = impl->session->GetOutputNameAllocated(0, allocator).get();
        m_impl = std::move(impl);
        m_vocab = contract.vocab;
        m_labels = contract.labels;
        m_positive = contract.positive;
        m_negative = contract.negative;
        m_status = QStringLiteral("ready: %1 classes (%2), vocabulary of %3 tokens")
                       .arg(m_labels.size())
                       .arg(m_labels.join(QStringLiteral("/")))
                       .arg(m_vocab.ids.size());
        return true;
    } catch (const Ort::Exception &error) {
        m_status = QStringLiteral("ONNX Runtime refused the model: %1")
                       .arg(QString::fromUtf8(error.what()));
        return false;
    }
}

HeadlineSentiment FinBertSentiment::scoreText(const QString &text)
{
    HeadlineSentiment out;
    if (m_impl == nullptr) {
        out.error = QStringLiteral("no model loaded (%1)").arg(m_status);
        return out;
    }
    const QList<qint32> ids = wordPieceEncode(m_vocab, text, kMaxTokens);
    if (ids.size() < 3) {   // [CLS] [SEP] and nothing in between is not a text
        out.error = QStringLiteral("nothing to score in this text");
        return out;
    }
    try {
        std::vector<int64_t> tokenIds;
        tokenIds.reserve(static_cast<size_t>(ids.size()));
        for (const qint32 id : ids) {
            tokenIds.push_back(id);
        }
        std::vector<int64_t> ones(tokenIds.size(), 1);
        std::vector<int64_t> zeros(tokenIds.size(), 0);
        const std::array<int64_t, 2> shape{1, static_cast<int64_t>(tokenIds.size())};
        const Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputs;
        std::vector<const char *> names;
        for (const std::string &name : m_impl->inputNames) {
            auto *data = name == "input_ids"        ? tokenIds.data()
                         : name == "attention_mask" ? ones.data()
                                                    : zeros.data();
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, data, tokenIds.size(),
                                                               shape.data(), shape.size()));
            names.push_back(name.c_str());
        }
        const std::array<const char *, 1> outputNames{m_impl->outputName.c_str()};
        const auto answers = m_impl->session->Run(Ort::RunOptions{nullptr}, names.data(),
                                                  inputs.data(), inputs.size(),
                                                  outputNames.data(), 1);
        const auto info = answers.front().GetTensorTypeAndShapeInfo();
        const auto *logits = answers.front().GetTensorData<float>();
        const std::span<const float> row(logits, info.GetElementCount());
        if (static_cast<qsizetype>(row.size()) != m_labels.size()) {
            out.error = QStringLiteral("the graph answered %1 values for %2 labels")
                            .arg(row.size())
                            .arg(m_labels.size());
            return out;
        }
        // Softmax over the logits row — probabilities the labels file gives meaning to.
        auto maxLogit = static_cast<double>(row[0]);
        for (const float v : row) {
            maxLogit = std::max(maxLogit, static_cast<double>(v));
        }
        double sum = 0.0;
        for (const float v : row) {
            const double e = std::exp(static_cast<double>(v) - maxLogit);
            out.probabilities.append(e);
            sum += e;
        }
        for (double &p : out.probabilities) {
            p /= sum;
        }
        out.labels = m_labels;
        out.net = out.probabilities.at(m_positive) - out.probabilities.at(m_negative);
        out.ok = true;
        return out;
    } catch (const Ort::Exception &error) {
        out.error = QStringLiteral("scoring failed inside ONNX Runtime: %1")
                        .arg(QString::fromUtf8(error.what()));
        return out;
    }
}

#else // TRADINGAPP_HAS_ONNXRUNTIME

struct FinBertSentiment::Impl {
};

namespace {

QString missingRuntimeStatus()
{
    return QStringLiteral("built without ONNX Runtime — run ./setup.sh ml, then reconfigure "
                          "the build to enable local text sentiment");
}

} // namespace

FinBertSentiment::FinBertSentiment() : m_status(missingRuntimeStatus()) {}

FinBertSentiment::~FinBertSentiment() = default;

bool FinBertSentiment::available()
{
    return false;
}

bool FinBertSentiment::load(const QString &directory)
{
    Q_UNUSED(directory);
    m_status = missingRuntimeStatus();
    return false;
}

HeadlineSentiment FinBertSentiment::scoreText(const QString &text)
{
    Q_UNUSED(text);
    HeadlineSentiment out;
    out.error = missingRuntimeStatus();
    return out;
}

#endif // TRADINGAPP_HAS_ONNXRUNTIME

bool FinBertSentiment::ready() const
{
    return m_impl != nullptr;
}

QString FinBertSentiment::status() const
{
    return m_status;
}

SocialSentiment FinBertSentiment::scoreHeadlines(const QStringList &headlines)
{
    SocialSentiment out;
    if (!ready()) {
        out.error = m_status;
        return out;
    }
    double sum = 0.0;
    for (const QString &headline : headlines) {
        const HeadlineSentiment scored = scoreText(headline);
        if (scored.ok) {
            sum += scored.net;
            ++out.scored;
        }
    }
    if (out.scored == 0) {
        out.error = QStringLiteral("no headline could be scored — no sentiment is published, "
                                   "never a zero");
        return out;
    }
    out.net = sum / out.scored;
    out.ok = true;
    return out;
}

} // namespace trading::crowd
