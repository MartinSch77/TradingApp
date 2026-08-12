// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/OllamaAdvisor.h"

#include "domain/OllamaResponseParser.h"
#include "services/JsonHttp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <chrono>
#include <utility>

namespace {

// What the model is asked to be. Short on purpose: a 1-3 B parameter model
// follows a brief instruction far better than a long one, and the evidence prompt
// (trading::buildDecisionEvidence) already carries the answer contract.
const char *const kSystemPrompt =
    "You are a trading decision assistant. From the evidence, list EVERY instrument "
    "worth trading right now, best first — as many or as few as the evidence "
    "supports, at most 16. Answer ONLY with a JSON object {\"picks\": [ ... ]} where "
    "each pick has the keys symbol (string, exactly as spelled in the evidence), "
    "action (\"BUY\", \"SELL\", \"HOLD\" or \"CLOSE\" for a position you hold and want "
    "out of), confidence (number 0-100), leverage "
    "(integer) and rationale (one short sentence). Use an empty picks list if "
    "nothing is worth trading. No prose, no markdown.";

// A local model can take a long time to answer on CPU, and being aborted
// mid-generation looks exactly like a broken feature. The probe is the opposite:
// it must fail fast, because "is Ollama there" has to be answerable now.
constexpr auto kGenerateTimeout = std::chrono::seconds{300};
constexpr auto kProbeTimeout = std::chrono::seconds{5};

} // namespace

// jsonPayloadIn / repairTruncatedJson / picksFrom (the defensive parse of a
// model's free-text answer) live in domain/OllamaResponseParser — Qt Core
// only, so they are independently testable and fuzzable without this TU's
// QNetworkAccessManager/JsonHttp dependency. See that header for the
// rationale (TS-OLLAMA-007).
using trading::jsonPayloadIn;
using trading::picksFrom;
using trading::repairTruncatedJson;

OllamaAdvisor::OllamaAdvisor(QString host, QString model, QObject *parent)
    : QObject(parent)
    , m_host(std::move(host))
    , m_model(std::move(model))
    , m_nam(new QNetworkAccessManager(this))
    , m_http(new JsonHttp(m_nam, this))
{
    // Ollama's own OLLAMA_HOST is conventionally written without a scheme
    // ("127.0.0.1:11434"), and that is exactly what a user already running Ollama
    // has in their environment — so accept it instead of failing on a bad URL.
    if (!m_host.isEmpty() && !m_host.contains(QLatin1String("://"))) {
        m_host.prepend(QLatin1String("http://"));
    }
    while (m_host.endsWith(QLatin1Char('/'))) {
        m_host.chop(1);
    }
    m_nam->setTransferTimeout(kProbeTimeout);  // per-request overrides follow
}

void OllamaAdvisor::setEndpointBaseForTesting(const QString &base)
{
    m_endpointBaseForTesting = base;
}

QString OllamaAdvisor::endpointBase() const
{
    return m_endpointBaseForTesting.isEmpty() ? m_host : m_endpointBaseForTesting;
}

QNetworkReply *OllamaAdvisor::postGenerate(const QJsonObject &body)
{
    QNetworkRequest req(QUrl(endpointBase() + QStringLiteral("/api/generate")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(kGenerateTimeout);
    m_inFlight = true;
    return m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void OllamaAdvisor::checkAvailability()
{
    if (!isConfigured()) {
        emit availability(false,
                          QStringLiteral("no model configured (set ollamaModel in config.json "
                                         "or OLLAMA_MODEL in the environment)"),
                          {});
        return;
    }
    QNetworkRequest req(QUrl(endpointBase() + QStringLiteral("/api/tags")));
    req.setTransferTimeout(kProbeTimeout);
    QNetworkReply *reply = m_nam->get(req);
    m_http->handleReply(reply, [this](bool ok, qint32 status, const QJsonDocument &doc,
                                      const QByteArray & /*raw*/, const QString &netError) {
        if (!ok) {
            emit availability(false,
                              QStringLiteral("%1 is not reachable (%2) — is `ollama serve` running?")
                                  .arg(m_host,
                                       netError.isEmpty() ? QStringLiteral("HTTP %1").arg(status)
                                                          : netError),
                              {});
            return;
        }
        QStringList models;
        const QJsonArray arr = doc.object().value(QStringLiteral("models")).toArray();
        for (const auto &m : arr) {
            const QString name = m.toObject().value(QStringLiteral("name")).toString();
            if (!name.isEmpty()) {
                models << name;
            }
        }
        // A tag may be implicit: "llama3.2" is served as "llama3.2:latest".
        const bool installed =
            std::any_of(models.cbegin(), models.cend(), [this](const QString &name) {
                return (name == m_model) || name.startsWith(m_model + QLatin1Char(':'));
            });
        if (installed) {
            emit availability(true, QStringLiteral("%1 ready at %2").arg(m_model, m_host), models);
            return;
        }
        emit availability(false,
                          QStringLiteral("%1 is up but does not serve '%2' — run `ollama pull %2` "
                                         "(installed: %3)")
                              .arg(m_host, m_model,
                                   models.isEmpty() ? QStringLiteral("none") : models.join(u", ")),
                          models);
    });
}

void OllamaAdvisor::requestExplanation(const QString &evidence)
{
    if (!isConfigured()) {
        emit explanationReady({}, QStringLiteral("No Ollama model configured."));
        return;
    }
    if (m_inFlight) {
        emit explanationReady({},
                              QStringLiteral("Ollama is still answering the previous request."));
        return;
    }
    const QJsonObject body{
        {QStringLiteral("model"), m_model},
        // The instruction rides along, but the SAFETY property is the caller's wiring: the
        // answer is displayed and consumed by nothing (REQ-F-045).
        {QStringLiteral("system"),
         QStringLiteral("You explain market evidence to a human reader in plain language. "
                        "Answer ONLY with JSON of the form {\"explanation\": \"two to four "
                        "sentences\"}. Do not give prices, targets, position sizes, stop "
                        "levels, probabilities, or buy/sell instructions.")},
        {QStringLiteral("prompt"), evidence},
        {QStringLiteral("stream"), false},
        {QStringLiteral("format"), QStringLiteral("json")},
        {QStringLiteral("options"),
         QJsonObject{{QStringLiteral("temperature"), 0.3},
                     {QStringLiteral("num_predict"), 400}}},
    };
    QNetworkReply *reply = postGenerate(body);
    m_http->handleReply(reply, [this](bool ok, qint32 status, const QJsonDocument &doc,
                                      const QByteArray &raw, const QString &netError) {
        m_inFlight = false;
        if (!ok) {
            emit explanationReady({}, QStringLiteral("Ollama request failed (HTTP %1): %2")
                                          .arg(status)
                                          .arg(netError.isEmpty()
                                                   ? QString::fromUtf8(raw.left(200))
                                                   : netError));
            return;
        }
        const QString text = doc.object().value(QStringLiteral("response")).toString();
        // Defensive, like the proposal parse — but for PROSE the honest fallback is the prose
        // itself: unlike a trade proposal, a mis-parse here cannot cause an action.
        QJsonDocument answer = QJsonDocument::fromJson(jsonPayloadIn(text));
        if (!answer.isObject()) {
            answer = QJsonDocument::fromJson(repairTruncatedJson(text));
        }
        QString words;
        if (answer.isObject()) {
            const QJsonObject object = answer.object();
            for (const auto &key :
                 {QStringLiteral("explanation"), QStringLiteral("summary"),
                  QStringLiteral("text"), QStringLiteral("answer")}) {
                if (words.isEmpty()) {
                    words = object.value(key).toString().trimmed();
                }
            }
            for (const auto &value : object) {   // a small model's key of the day
                if (words.isEmpty() && value.isString()) {
                    words = value.toString().trimmed();
                }
            }
        } else if (!text.trimmed().startsWith(QLatin1Char('{'))) {
            words = text.trimmed();
        }
        if (words.isEmpty()) {
            emit explanationReady({}, QStringLiteral("%1 answered nothing readable: %2")
                                          .arg(m_model, text.left(160).simplified()));
            return;
        }
        emit explanationReady(words, QString());
    });
}

void OllamaAdvisor::requestDecision(const QString &evidencePrompt)
{
    if (!isConfigured()) {
        emit proposalsReady({}, QStringLiteral("No Ollama model configured."));
        return;
    }
    if (m_inFlight) {
        emit proposalsReady({}, QStringLiteral("Ollama is still answering the previous request."));
        return;
    }

    const QJsonObject body{
        {QStringLiteral("model"), m_model},
        {QStringLiteral("system"), QString::fromLatin1(kSystemPrompt)},
        {QStringLiteral("prompt"), evidencePrompt},
        {QStringLiteral("stream"), false},
        // Ollama's structured-output switch: the daemon constrains the sampler to
        // emit valid JSON. The parsing below stays defensive anyway.
        {QStringLiteral("format"), QStringLiteral("json")},
        {QStringLiteral("options"),
         QJsonObject{
             // Low temperature: a trading call should not be a dice roll, and it
             // makes a small model's JSON far more reliable.
             {QStringLiteral("temperature"), 0.2},
             // Room for a LIST of picks, not just one (kMaxPicks × ~40 tokens).
             // Room for a LIST of picks WITH rationales. 700 truncated the JSON mid-answer
             // on qwen2.5:1.5b (measured: a response cut off at "...low volatilit"), which
             // parsed to nothing and wasted the model's whole turn. 1500 lets it close the
             // structure; repairTruncatedJson salvages the complete picks if it still cuts off.
             {QStringLiteral("num_predict"), 1500},
         }},
    };

    QNetworkReply *reply = postGenerate(body);
    m_http->handleReply(reply, [this](bool ok, qint32 status, const QJsonDocument &doc,
                                      const QByteArray &raw, const QString &netError) {
        m_inFlight = false;
        if (!ok) {
            emit proposalsReady({}, QStringLiteral("Ollama request failed (HTTP %1): %2")
                                        .arg(status)
                                        .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(200))
                                                                : netError));
            return;
        }
        // /api/generate with stream=false answers one object whose "response" holds
        // the model's text — the JSON we asked for.
        const QString text = doc.object().value(QStringLiteral("response")).toString();
        const QList<AiDecision> picks = picksFrom(text);
        if (picks.isEmpty()) {
            // Either no JSON at all, or a well-formed "nothing worth trading". Both
            // are reported as-is; the caller must not read a parse failure as a HOLD.
            emit proposalsReady({}, QStringLiteral("%1 named no tradable pick: %2")
                                        .arg(m_model, text.left(160).simplified()));
            return;
        }
        emit proposalsReady(picks, QString());
    });
}
