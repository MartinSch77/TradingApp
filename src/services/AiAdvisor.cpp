#include "services/AiAdvisor.h"

#include "services/JsonHttp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <chrono>
#include <utility>

AiAdvisor::AiAdvisor(QString apiKey, QObject *parent)
    : QObject(parent)
    , m_apiKey(std::move(apiKey))
    , m_nam(new QNetworkAccessManager(this))
    , m_http(new JsonHttp(m_nam, this))
{
    // The manager default guards against silent stalls; the actual request below
    // overrides it with a longer per-request timeout while Claude composes.
    m_nam->setTransferTimeout(std::chrono::seconds{30});

}

void AiAdvisor::setEndpointBaseForTesting(const QString &base)
{
    m_endpointBaseForTesting = base;
}

void AiAdvisor::requestDecision(const QString &evidencePrompt)
{
    if (!isConfigured()) {
        AiDecision d;
        d.error = QStringLiteral("No anthropicApiKey configured.");
        emit decisionReady(d);
        return;
    }

    // Structured-output schema so the reply is guaranteed-parseable JSON.
    QJsonObject props{
        {QStringLiteral("symbol"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("action"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                     {QStringLiteral("enum"), QJsonArray{QStringLiteral("BUY"), QStringLiteral("SELL"),
                                                         QStringLiteral("HOLD")}}}},
        {QStringLiteral("confidence"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("leverage"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
        {QStringLiteral("rationale"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
    };
    QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), props},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("symbol"), QStringLiteral("action"),
                                                QStringLiteral("confidence"), QStringLiteral("leverage"),
                                                QStringLiteral("rationale")}},
        {QStringLiteral("additionalProperties"), false},
    };
    const QJsonObject body{
        {QStringLiteral("model"), QStringLiteral("claude-opus-4-8")},
        {QStringLiteral("max_tokens"), 1024},
        {QStringLiteral("system"),
         QStringLiteral("You are a trading decision assistant. From the evidence, pick the single "
                        "best instrument to trade right now and how. Weigh the sources; be decisive "
                        "but note key risks briefly. Respond only via the required JSON schema. "
                        "This is not financial advice.")},
        {QStringLiteral("output_config"),
         QJsonObject{{QStringLiteral("format"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                  {QStringLiteral("schema"), schema}}}}},
        {QStringLiteral("messages"),
         QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), evidencePrompt}}}},
    };

    const QString host = m_endpointBaseForTesting.isEmpty()
                             ? QStringLiteral("https://api.anthropic.com")
                             : m_endpointBaseForTesting;
    QNetworkRequest req(QUrl(host + QStringLiteral("/v1/messages")));
    const QByteArray apiKeyValue = m_apiKey.toUtf8();
    req.setRawHeader("x-api-key", apiKeyValue);
    const QByteArray versionName("anthropic-version");
    const QByteArray versionValue("2023-06-01");
    req.setRawHeader(versionName, versionValue);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Non-streaming generation can go quiet for well over the manager's 30s transfer
    // timeout while Claude composes; override it so a slow answer isn't aborted.
    req.setTransferTimeout(std::chrono::seconds{180});
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam->post(req, payload);
    m_http->handleReply(reply, [this](bool ok, qint32 status, const QJsonDocument &doc,
                                      const QByteArray &raw, const QString &netError) {
        AiDecision d;
        if (!ok) {
            d.error = QStringLiteral("Claude request failed (HTTP %1): %2")
                          .arg(status)
                          .arg(netError.isEmpty() ? QString::fromUtf8(raw.left(200)) : netError);
            emit decisionReady(d);
            return;
        }
        // Find the first text block; with output_config.format it holds the JSON object.
        QString jsonText;
        const QJsonArray content = doc.object().value(QStringLiteral("content")).toArray();
        for (const auto &b : content) {
            const QJsonObject o = b.toObject();
            if (o.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                jsonText = o.value(QStringLiteral("text")).toString();
                break;
            }
        }
        const QJsonObject r = QJsonDocument::fromJson(jsonText.toUtf8()).object();
        if (r.isEmpty()) {
            d.error = QStringLiteral("Claude returned an unparsable response.");
            emit decisionReady(d);
            return;
        }
        d.ok = true;
        d.symbol = r.value(QStringLiteral("symbol")).toString();
        d.action = r.value(QStringLiteral("action")).toString().toUpper();
        d.confidence = r.value(QStringLiteral("confidence")).toDouble();
        d.leverage = r.value(QStringLiteral("leverage")).toInt();
        d.rationale = r.value(QStringLiteral("rationale")).toString();
        emit decisionReady(d);
    });
}
