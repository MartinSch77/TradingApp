// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// libFuzzer harness for the Yahoo v8 "chart" JSON parse (REQ-F-035/-022,
// src/domain/YahooChartParser.cpp) — an HTTP response body from a third party,
// navigated with no schema validation beyond "is this the shape we expect." Exercises
// the whole chain a real feed reply goes through: raw bytes -> QJsonDocument ->
// yahooChartResult -> {yahooCloses, yahooOhlc, yahooMetaSessionChange, yahooBars}.

#include "domain/YahooChartParser.h"

#include <QJsonDocument>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size)
{
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(data),
                                           static_cast<qsizetype>(size)));
    const QJsonObject result = trading::yahooChartResult(doc);

    const QList<double> closes = trading::yahooCloses(result, /*positiveOnly=*/false);
    const QList<double> positiveCloses = trading::yahooCloses(result, /*positiveOnly=*/true);
    if (positiveCloses.size() > closes.size()) {
        __builtin_trap();   // filtering can only shrink the series, never grow it
    }

    // yahooOhlc's four columns are deliberately NOT required to be the same length: a
    // payload can omit one of open/high/low/close entirely (that key's column comes
    // back empty) while still carrying the others, and domain/Candles::candlesFrom is
    // exactly the function that copes with that (it takes the shortest of the four,
    // never pads). There is no length invariant to assert here — calling it at all is
    // the coverage this harness wants.
    (void)trading::yahooOhlc(result);

    const QList<double> sessionChange = trading::yahooMetaSessionChange(result);
    if (!sessionChange.isEmpty() && (sessionChange.size() != 2)) {
        __builtin_trap();   // documented contract: empty, or exactly [previousClose, now]
    }

    const trading::VolumeSeries bars = trading::yahooBars(result);
    if (bars.closes.size() != bars.volumes.size()) {
        __builtin_trap();   // the whole point of yahooBars: aligned bars, index for index
    }
    return 0;
}
