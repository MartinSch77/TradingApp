// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_YAHOOCHARTPARSER_H
#define TRADINGAPP_DOMAIN_YAHOOCHARTPARSER_H

#include "domain/Candles.h"
#include "domain/IndexConfluence.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QList>

// Parsing of the Yahoo Finance v8 "chart" JSON envelope (REQ-F-035/-022) — pulled out
// of services/MarketFeeds into domain (Qt Core only) so it is independently testable
// and fuzzable without that TU's QNetworkAccessManager/JsonHttp dependency. This is
// UNTRUSTED external text: an HTTP response body from a third party, parsed with no
// schema validation beyond "is this the shape we expect" — exactly the kind of
// hand-rolled JSON navigation a malformed/adversarial payload can break in ways a
// happy-path unit test never exercises.
namespace trading {

// First result object of a Yahoo Finance v8 chart payload ({} when absent) — the
// VIX, reference-quote and intraday endpoints all share this envelope.
// QJsonArray::first() on an empty array yields Undefined, so every step degrades
// to an empty object/array instead of faulting.
[[nodiscard]] QJsonObject yahooChartResult(const QJsonDocument &doc);

// The close series of a Yahoo chart result (indicators.quote[0].close). Gaps come
// through as null and are skipped. positiveOnly additionally drops zero/negative
// values (a volatility-index baseline must not average in placeholder zeros).
[[nodiscard]] QList<double> yahooCloses(const QJsonObject &chartResult, bool positiveOnly);

// The candles of a chart result: open/high/low/close as four sibling arrays with
// INDEPENDENT gaps — this hands over the raw columns; domain/Candles decides which
// bars may be drawn.
[[nodiscard]] CandleColumns yahooOhlc(const QJsonObject &chartResult);

// The session change from the chart result's META block, as a two-point series
// [previousClose, regularMarketPrice] — or empty when the meta cannot supply both.
[[nodiscard]] QList<double> yahooMetaSessionChange(const QJsonObject &chartResult);

// The close AND volume arrays of a Yahoo chart result, as ALIGNED bars (a bar
// survives only when both halves are present and positive, so index i always means
// one real minute in both arrays — see IndexConfluence.h's VolumeSeries).
[[nodiscard]] VolumeSeries yahooBars(const QJsonObject &chartResult);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_YAHOOCHARTPARSER_H
