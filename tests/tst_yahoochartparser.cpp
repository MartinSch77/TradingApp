// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Direct unit tests for the Yahoo v8 "chart" JSON parse (2026-08-12, tooling backlog
// item 7): extracted from services/MarketFeeds into domain/YahooChartParser so it is
// testable and fuzzable (fuzz/yahoo_chart_fuzzer.cpp) without a QtNetwork dependency.
// yahooBars in particular had ZERO direct test coverage before this file — every
// existing test exercised it only indirectly through MarketFeeds' HTTP mock.

#include "domain/YahooChartParser.h"

#include <QJsonDocument>
#include <QtTest/QtTest>

using namespace trading;

namespace {
QJsonObject resultFrom(const QByteArray &json)
{
    return yahooChartResult(QJsonDocument::fromJson(json));
}
} // namespace

class TestYahooChartParser : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-YAHOO-001 @design DES-DOM-YAHOOPARSE
    // @relation(REQ-F-035, scope=function)
    void TS_YAHOO_001_emptyOrMalformedDocumentDegradesToEmptyObject()
    {
        QCOMPARE(resultFrom(QByteArray()), QJsonObject());
        QCOMPARE(resultFrom(R"({"chart":{"result":[]}})"), QJsonObject());
        QCOMPARE(resultFrom(R"({"not chart":1})"), QJsonObject());
        QCOMPARE(resultFrom("not json at all"), QJsonObject());
    }

    //! @tstid TS-YAHOO-002 @design DES-DOM-YAHOOPARSE
    // @relation(REQ-F-035, scope=function)
    void TS_YAHOO_002_closesSkipsNullsAndOptionallyNonPositives()
    {
        const QJsonObject result = resultFrom(
            R"({"chart":{"result":[{"indicators":{"quote":)"
            R"([{"close":[5000.5,null,5001.5,0.0,-1.0]}]}}]}})");
        QCOMPARE(yahooCloses(result, /*positiveOnly=*/false), (QList<double>{5000.5, 5001.5, 0.0, -1.0}));
        QCOMPARE(yahooCloses(result, /*positiveOnly=*/true), (QList<double>{5000.5, 5001.5}));
    }

    //! @tstid TS-YAHOO-003 @design DES-DOM-YAHOOPARSE
    // @relation(REQ-F-022, scope=function)
    void TS_YAHOO_003_ohlcColumnsCanDifferInLengthWhenAKeyIsAbsent()
    {
        // No "open"/"high"/"low" keys at all — only close+volume, which some feeds send.
        const QJsonObject result = resultFrom(
            R"({"chart":{"result":[{"indicators":{"quote":)"
            R"([{"close":[1.0,2.0,3.0],"volume":[10,20,30]}]}}]}})");
        const CandleColumns ohlc = yahooOhlc(result);
        QCOMPARE(ohlc.closes.size(), 3);
        QCOMPARE(ohlc.opens.size(), 0);
        QCOMPARE(ohlc.highs.size(), 0);
        QCOMPARE(ohlc.lows.size(), 0);
    }

    //! @tstid TS-YAHOO-004 @design DES-DOM-YAHOOPARSE
    // @relation(REQ-F-022, scope=function)
    void TS_YAHOO_004_ohlcNullMinuteBecomesZeroRatherThanBeingDropped()
    {
        // A null in one column must not shift that column against the other three —
        // dropping it (as yahooCloses does) would misalign every later index.
        const QJsonObject result = resultFrom(
            R"({"chart":{"result":[{"indicators":{"quote":)"
            R"([{"open":[1.0,null,3.0],"high":[1.5,2.5,3.5],)"
            R"("low":[0.5,1.5,2.5],"close":[1.2,2.2,3.2]}]}}]}})");
        const CandleColumns ohlc = yahooOhlc(result);
        QCOMPARE(ohlc.opens, (QList<double>{1.0, 0.0, 3.0}));
        QCOMPARE(ohlc.opens.size(), ohlc.closes.size());
    }

    //! @tstid TS-YAHOO-005 @design DES-DOM-YAHOOPARSE
    // @relation(REQ-F-020, scope=function)
    void TS_YAHOO_005_metaSessionChangeNeedsBothFieldsPositive()
    {
        QCOMPARE(yahooMetaSessionChange(resultFrom(
                     R"({"chart":{"result":[{"meta":)"
                     R"({"regularMarketPrice":311.0,"previousClose":309.38}}]}})")),
                 (QList<double>{309.38, 311.0}));
        // Missing previousClose: no session change can be claimed.
        QVERIFY(yahooMetaSessionChange(
                    resultFrom(R"({"chart":{"result":[{"meta":{"regularMarketPrice":311.0}}]}})"))
                    .isEmpty());
        // Zero/negative base: refused rather than dividing by it downstream.
        QVERIFY(yahooMetaSessionChange(
                    resultFrom(R"({"chart":{"result":[{"meta":)"
                               R"({"regularMarketPrice":311.0,"previousClose":0.0}}]}})"))
                    .isEmpty());
    }

    //! @tstid TS-YAHOO-006 @design DES-DOM-YAHOOPARSE
    // @relation(REQ-F-035, scope=function)
    void TS_YAHOO_006_barsAreAlignedByIndexNeverHalfABar()
    {
        // Previously ZERO direct coverage: every existing test reached yahooBars only
        // through MarketFeeds' HTTP mock. A null/zero in EITHER column at index i must
        // drop that whole bar, not just the offending half — else closes[i] and
        // volumes[i] would stop referring to the same minute.
        const QJsonObject result = resultFrom(
            R"({"chart":{"result":[{"indicators":{"quote":)"
            R"([{"close":[5000.5,null,5001.5,0.0,5002.0],)"
            R"("volume":[10,20,0,30,null]}]}}]}})");
        const VolumeSeries bars = yahooBars(result);
        QCOMPARE(bars.closes, (QList<double>{5000.5}));
        QCOMPARE(bars.volumes, (QList<double>{10.0}));
    }

    //! @tstid TS-YAHOO-007 @design DES-DOM-YAHOOPARSE
    // @relation(REQ-F-035, scope=function)
    void TS_YAHOO_007_noVolumeAtAllIsHonestlyEmptyNotZero()
    {
        // The volatility/yield indices carry no volume at all — the read must come
        // back UNKNOWN (empty), never a fabricated zero series.
        const QJsonObject result = resultFrom(
            R"({"chart":{"result":[{"indicators":{"quote":)"
            R"([{"close":[1.0,2.0,3.0]}]}}]}})");
        QVERIFY(yahooBars(result).closes.isEmpty());
        QVERIFY(yahooBars(result).volumes.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestYahooChartParser)
#include "tst_yahoochartparser.moc"
