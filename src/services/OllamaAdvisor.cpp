#include "services/OllamaAdvisor.h"

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
    "supports, at most 10. Answer ONLY with a JSON object {\"picks\": [ ... ]} where "
    "each pick has the keys symbol (string, exactly as spelled in the evidence), "
    "action (\"BUY\", \"SELL\", \"HOLD\" or \"CLOSE\" for a position you hold and want "
    "out of), confidence (number 0-100), leverage "
    "(integer) and rationale (one short sentence). Use an empty picks list if "
    "nothing is worth trading. No prose, no markdown.";

// Upper bound on how many picks are taken from one answer: enough that the risk
// budget, not the protocol, decides how many trades happen, while a runaway
// generation cannot flood the books.
constexpr qsizetype kMaxPicks = 10;

// A local model can take a long time to answer on CPU, and being aborted
// mid-generation looks exactly like a broken feature. The probe is the opposite:
// it must fail fast, because "is Ollama there" has to be answerable now.
constexpr auto kGenerateTimeout = std::chrono::seconds{300};
constexpr auto kProbeTimeout = std::chrono::seconds{5};

// The JSON value inside a model's answer, even when it arrived wrapped in prose or
// ```json fences: the outermost {...} or [...], whichever starts first. Empty when
// there is neither.
QByteArray jsonPayloadIn(const QString &text)
{
    const qsizetype firstObj = text.indexOf(QLatin1Char('{'));
    const qsizetype firstArr = text.indexOf(QLatin1Char('['));
    const bool asArray = (firstArr >= 0) && ((firstObj < 0) || (firstArr < firstObj));
    const qsizetype first = asArray ? firstArr : firstObj;
    const qsizetype last =
        asArray ? text.lastIndexOf(QLatin1Char(']')) : text.lastIndexOf(QLatin1Char('}'));
    if ((first < 0) || (last <= first)) {
        return {};
    }
    return text.mid(first, (last - first) + 1).toUtf8();
}

// BUY / SELL / HOLD out of whatever the model wrote ("sell", "SELL (short)",
// "Action: BUY"). Anything else reads as HOLD — an unrecognised action must not
// become a trade.
QString normalizeAction(const QString &raw)
{
    const QString upper = raw.toUpper();
    // CLOSE first: "close the short" contains SHORT, and reading that as a new
    // SELL would open the very position the model asked to be rid of.
    if (upper.contains(QLatin1String("CLOSE")) || upper.contains(QLatin1String("EXIT"))
        || upper.contains(QLatin1String("FLAT"))) {
        return QStringLiteral("CLOSE");
    }
    if (upper.contains(QLatin1String("BUY")) || upper.contains(QLatin1String("LONG"))) {
        return QStringLiteral("BUY");
    }
    if (upper.contains(QLatin1String("SELL")) || upper.contains(QLatin1String("SHORT"))) {
        return QStringLiteral("SELL");
    }
    return QStringLiteral("HOLD");
}

// Confidence as a number, from a number, a numeric string ("62", "0.62") or the
// words small models like to use. 0 when it cannot be read — which the bot's own
// confidence floor then treats as "not actionable", the safe reading.
double normalizeConfidence(const QJsonValue &value)
{
    if (value.isDouble()) {
        const double raw = value.toDouble();
        return (raw > 0.0 && raw <= 1.0) ? (raw * 100.0) : raw;  // 0.62 means 62%
    }
    const QString text = value.toString().trimmed();
    bool ok = false;
    const double numeric = text.toDouble(&ok);
    if (ok) {
        return (numeric > 0.0 && numeric <= 1.0) ? (numeric * 100.0) : numeric;
    }
    const QString lower = text.toLower();
    if (lower.contains(QLatin1String("high")) || lower.contains(QLatin1String("strong"))) {
        return 75.0;
    }
    if (lower.contains(QLatin1String("medium")) || lower.contains(QLatin1String("moderate"))) {
        return 50.0;
    }
    if (lower.contains(QLatin1String("low")) || lower.contains(QLatin1String("weak"))) {
        return 25.0;
    }
    return 0.0;
}

qint32 normalizeLeverage(const QJsonValue &value)
{
    if (value.isDouble()) {
        return static_cast<qint32>(value.toDouble());
    }
    bool ok = false;
    // "x10" / "10x" / "10" all appear in practice.
    const QString digits = value.toString().remove(QLatin1Char('x'), Qt::CaseInsensitive).trimmed();
    const qint32 parsed = digits.toInt(&ok);
    return ok ? parsed : 0;
}

// One pick out of one JSON object, defensively. `rationale` is looked up under the
// aliases small models actually produce (qwen2.5:1.5b writes "rationality").
AiDecision pickFrom(const QJsonObject &obj)
{
    AiDecision d;
    d.ok = true;
    d.symbol = obj.value(QStringLiteral("symbol")).toString().trimmed();
    d.action = normalizeAction(obj.value(QStringLiteral("action")).toString());
    d.confidence = normalizeConfidence(obj.value(QStringLiteral("confidence")));
    d.leverage = normalizeLeverage(obj.value(QStringLiteral("leverage")));
    for (const auto *key : {"rationale", "reason", "rationality", "explanation"}) {
        const QString text = obj.value(QLatin1String(key)).toString().trimmed();
        if (!text.isEmpty()) {
            d.rationale = text;
            break;
        }
    }
    return d;
}

// Every pick in a SYMBOL-KEYED map — {"SPX500": {"action": …}, "GER40": {…}} —
// which is what qwen2.5:1.5b really answers when asked for a list (measured
// 2026-08-04). The symbol comes from the key unless the value carries its own.
QList<AiDecision> picksFromMap(const QJsonObject &map)
{
    QList<AiDecision> picks;
    for (auto it = map.constBegin(); (it != map.constEnd()) && (picks.size() < kMaxPicks); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        AiDecision pick = pickFrom(it.value().toObject());
        if (pick.symbol.isEmpty()) {
            pick.symbol = it.key();
        }
        if (!pick.symbol.isEmpty()) {
            picks.append(pick);
        }
    }
    return picks;
}

// Does this object look like ONE pick rather than a map of them? A pick names its
// own action or symbol; a map's values are objects.
bool looksLikeOnePick(const QJsonObject &obj)
{
    return obj.contains(QStringLiteral("action")) || obj.contains(QStringLiteral("symbol"));
}

// The picks in a model's answer. Accepts what models really send: the requested
// {"picks":[…]}, a bare […] array, an object keyed differently ("trades",
// "instruments"), a SYMBOL-KEYED map (with or without the "picks" wrapper), or a
// SINGLE pick object — one opinion is still an answer. Every shape here has been
// observed from a real local model; none of them may become a silent no-trade.
QList<AiDecision> picksFromArray(const QJsonArray &arr)
{
    QList<AiDecision> picks;
    for (const auto &v : arr) {
        if (!v.isObject()) {
            continue;
        }
        const AiDecision pick = pickFrom(v.toObject());
        if (!pick.symbol.isEmpty()) {
            picks.append(pick);
        }
        if (picks.size() >= kMaxPicks) {
            break;
        }
    }
    return picks;
}

QList<AiDecision> picksFrom(const QString &text)
{
    const QJsonDocument doc = QJsonDocument::fromJson(jsonPayloadIn(text));
    if (doc.isArray()) {
        return picksFromArray(doc.array());
    }
    if (!doc.isObject()) {
        return {};
    }
    const QJsonObject root = doc.object();
    // A container under one of the keys models use — as an array OR as a
    // symbol-keyed map, both of which have been seen in the wild.
    for (const auto *key : {"picks", "trades", "instruments", "candidates"}) {
        const QJsonValue held = root.value(QLatin1String(key));
        if (held.isArray()) {
            return picksFromArray(held.toArray());
        }
        if (held.isObject()) {
            return picksFromMap(held.toObject());
        }
    }
    // No wrapper: either one pick, or the map itself at the root.
    if (looksLikeOnePick(root)) {
        const AiDecision single = pickFrom(root);
        return single.symbol.isEmpty() ? QList<AiDecision>{} : QList<AiDecision>{single};
    }
    return picksFromMap(root);
}

} // namespace

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
             {QStringLiteral("num_predict"), 700},
         }},
    };

    QNetworkRequest req(QUrl(endpointBase() + QStringLiteral("/api/generate")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(kGenerateTimeout);
    m_inFlight = true;
    QNetworkReply *reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
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
