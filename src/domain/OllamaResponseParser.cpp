// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "domain/OllamaResponseParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// Upper bound on how many picks are taken from one answer: enough that the risk
// budget, not the protocol, decides how many trades happen, while a runaway
// generation cannot flood the books.
constexpr qsizetype kMaxPicks = 16;

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

// The last position in a JSON fragment at which a container closed CLEANLY, plus
// the containers still open there — everything needed to trim a truncated answer
// back to a valid prefix and re-balance it.
struct SafeCut {
    qsizetype len = -1;         // length of the cleanly-closed prefix, or -1 if none
    QList<QChar> openStack;     // the containers still open at that cut, outermost first
};

// Scan a JSON fragment, tracking string/escape state so brackets inside strings do
// not count, and record the LAST clean close and the open stack at that point.
// Split out of repairTruncatedJson so neither function carries the whole state
// machine's complexity.
SafeCut scanToSafeCut(const QString &s)
{
    SafeCut cut;
    QList<QChar> stack;
    bool inStr = false;
    bool esc = false;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (inStr) {
            if (esc) {
                esc = false;
            } else if (c == QLatin1Char('\\')) {
                esc = true;
            } else if (c == QLatin1Char('"')) {
                inStr = false;
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            inStr = true;
        } else if ((c == QLatin1Char('{')) || (c == QLatin1Char('['))) {
            stack.append(c);
        } else if ((c == QLatin1Char('}')) || (c == QLatin1Char(']'))) {
            if (!stack.isEmpty()) {
                stack.removeLast();
            }
            cut.len = i + 1;          // a container just closed cleanly here
            cut.openStack = stack;    // …with these still open
        }
    }
    return cut;
}

} // namespace

namespace trading {

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

QByteArray repairTruncatedJson(const QString &text)
{
    const qsizetype start = text.indexOf(QLatin1Char('{'));
    const qsizetype startArr = text.indexOf(QLatin1Char('['));
    const qsizetype from =
        (startArr >= 0 && (start < 0 || startArr < start)) ? startArr : start;
    if (from < 0) {
        return {};
    }
    const QString s = text.mid(from);
    const SafeCut cut = scanToSafeCut(s);
    if (cut.len < 0) {
        return {};                // nothing completed — no picks to recover
    }
    QString repaired = s.left(cut.len);
    for (qsizetype i = cut.openStack.size() - 1; i >= 0; --i) {
        repaired +=
            (cut.openStack.at(i) == QLatin1Char('{')) ? QLatin1Char('}') : QLatin1Char(']');
    }
    return repaired.toUtf8();
}

QList<AiDecision> picksFrom(const QString &text)
{
    QJsonDocument doc = QJsonDocument::fromJson(jsonPayloadIn(text));
    if (doc.isNull()) {
        // The answer did not parse — most often a token-budget truncation. Try to salvage
        // the complete picks from the valid prefix before giving up.
        doc = QJsonDocument::fromJson(repairTruncatedJson(text));
    }
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

} // namespace trading
