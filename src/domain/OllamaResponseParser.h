// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_OLLAMARESPONSEPARSER_H
#define TRADINGAPP_DOMAIN_OLLAMARESPONSEPARSER_H

#include "domain/Models.h"

#include <QByteArray>
#include <QList>
#include <QString>

// The DEFENSIVE parse of a local model's free-text answer (REQ-F-030,
// services/OllamaAdvisor) — pulled out of that TU into domain (Qt Core only) so it
// is independently testable and fuzzable without a QNetworkAccessManager/JsonHttp
// dependency. A small model (measured on qwen2.5:1.5b) answers sloppily: prose or
// ```json fences around the payload, "rationality" for "rationale", "high" for a
// number, a symbol-keyed map instead of the requested array, and — when the token
// budget runs out mid-generation — a valid JSON PREFIX with no closing braces at
// all. None of those may become a silent no-trade (TS-OLLAMA-007), which is why
// every shape here is handled rather than treated as a parse failure.
namespace trading {

// The JSON value inside a model's answer, even wrapped in prose or ```json
// fences: the outermost {...} or [...], whichever starts first. Empty when
// there is neither.
[[nodiscard]] QByteArray jsonPayloadIn(const QString &text);

// A best-effort repair of JSON the model truncated by running out of tokens: trims
// to the last point where a container closed CLEANLY (so no half-written string or
// key survives) and appends the closing braces/brackets still open there. A keyed
// map `{"picks":{"BTC":{...},"ETH":{...<cut>}}` thus recovers the complete BTC
// pick, key and all, and drops the incomplete ETH one. Empty when not even one
// container closed (nothing to salvage).
[[nodiscard]] QByteArray repairTruncatedJson(const QString &text);

// The picks in a model's answer. Accepts what models really send: the requested
// {"picks":[...]}, a bare [...] array, an object keyed differently ("trades",
// "instruments"), a SYMBOL-KEYED map (with or without the "picks" wrapper), or a
// SINGLE pick object. Falls back to repairTruncatedJson when the raw text does not
// parse. Every shape here has been observed from a real local model.
[[nodiscard]] QList<AiDecision> picksFrom(const QString &text);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_OLLAMARESPONSEPARSER_H
