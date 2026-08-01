// Unit tests for the multi-source decision engine (DES-DOM-DEC).

#include "domain/DecisionEngine.h"

#include <QtTest/QtTest>

#include <algorithm>

using namespace trading;

namespace {

QList<double> trend(qint32 n, double factorEven, double factorOdd)
{
    QList<double> s;
    double p = 100.0;
    for (qint32 i = 0; i < n; ++i) {
        p *= (i % 2 == 0) ? factorEven : factorOdd;
        s.append(p);
    }
    return s;
}

ScreenerRow row(const QString &sym, const QList<double> &closes, qint32 maxLev = 20)
{
    ScreenerRow r;
    r.symbol = sym;
    r.closes = closes;
    r.lastPrice = closes.isEmpty() ? 0.0 : closes.last();
    r.maxLeverage = maxLev;
    r.ok = true;
    return r;
}

// The row for one symbol, or a default-constructed row when it is absent (all
// "have…" flags false, so the assertions below fail on a miss). Returning a
// value keeps the analyzers out of a dead end: neither cppcheck nor the Clang
// Static Analyzer can see that QVERIFY returns early on failure, so the
// idiomatic "QVERIFY(found); use(*found)" reads to them as a null dereference /
// out-of-bounds access.
DecisionRow rowFor(const QList<DecisionRow> &rows, const QString &symbol)
{
    const auto it = std::find_if(rows.cbegin(), rows.cend(),
                                 [&symbol](const DecisionRow &d) { return d.symbol == symbol; });
    return (it == rows.cend()) ? DecisionRow{} : *it;
}

} // namespace

class TestDecisionEngine : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-DEC-001 @design DES-DOM-DEC
    // @relation(REQ-F-009, scope=function)
    void TS_DEC_001_crowdTilt()
    {
        QVERIFY(crowdTilt(5.0) > 0.4);     // extreme fear → contrarian bullish
        QVERIFY(crowdTilt(95.0) < -0.4);   // extreme greed → contrarian bearish
        QVERIFY(crowdTilt(60.0) > 0.0);    // mild greed → mild momentum tilt
        QVERIFY(crowdTilt(40.0) < 0.0);
        QCOMPARE(crowdTilt(50.0), 0.0);
        QVERIFY(crowdTilt(-50.0) <= 1.0);  // clamped input stays bounded
        QVERIFY(crowdTilt(500.0) >= -1.0);
    }

    //! @tstid TS-DEC-002 @design DES-DOM-DEC
    // @relation(REQ-F-008, scope=function)
    void TS_DEC_002_newsSentimentSign()
    {
        QList<NewsHeadline> pos;
        NewsHeadline h;
        h.title = QStringLiteral("Stocks surge to record on strong earnings");
        pos << h;
        QVERIFY(newsSentimentScore(pos).score > 0.0);
        QCOMPARE(newsSentimentScore(pos).count, 1);

        QList<NewsHeadline> neg;
        h.title = QStringLiteral("Markets plunge as recession fear grows");
        neg << h;
        QVERIFY(newsSentimentScore(neg).score < 0.0);

        QList<NewsHeadline> neutral;
        h.title = QStringLiteral("Exchange announces new listing schedule");
        neutral << h;
        QCOMPARE(newsSentimentScore(neutral).score, 0.0);
    }

    //! @tstid TS-DEC-003 @design DES-DOM-DEC
    // @relation(REQ-F-008, scope=function)
    void TS_DEC_003_marketRegime()
    {
        MarketSnapshot m;
        m.vixValid = true;
        m.vix = 30.0;
        QVERIFY(marketRegime(m).tilt < 0.0);   // risk-off
        QVERIFY(!marketRegime(m).eventRisk);   // no events supplied
        m.vix = 13.0;
        QVERIFY(marketRegime(m).tilt > 0.0);   // risk-on

        EconomicEvent e;
        e.when = QDateTime::currentDateTime().addSecs(3600);
        e.impact = QStringLiteral("High");
        m.events << e;
        QVERIFY(marketRegime(m).eventRisk);  // imminent high-impact event flagged
    }

    //! @tstid TS-DEC-004 @design DES-DOM-DEC
    // @relation(REQ-F-008, REQ-F-009, scope=function)
    void TS_DEC_004_compositeWeightingAndSort()
    {
        MarketSnapshot m;
        m.screenerRows << row(QStringLiteral("UP"), trend(120, 1.004, 1.001))
                       << row(QStringLiteral("FLAT"), QList<double>(120, 100.0));
        const QList<DecisionRow> plain = computeDecisionRows(m);
        QCOMPARE(plain.size(), 2);
        // Sorted by confidence descending; the trending instrument leads.
        QVERIFY(plain[0].confidence >= plain[1].confidence);
        QCOMPARE(plain[0].symbol, QStringLiteral("UP"));
        QCOMPARE(plain[0].dir, 1);
        QVERIFY(!plain[0].haveCrowd);
        // Renormalisation over the AVAILABLE sources: with only the technical
        // ensemble present (plus the always-on regime term, neutral here), the
        // composite must be 0.35·tech / (0.35 + 0.15) — NOT tech divided by the
        // full five-source weight sum, which would dilute lone sources.
        const double techSigned = plain[0].techDir * (plain[0].techConf / 100.0);
        QVERIFY(std::abs(plain[0].composite - ((0.35 * techSigned) / 0.50)) < 1e-9);

        // Extreme greed tilts the composite of every instrument bearish.
        m.fgValid = true;
        m.fearGreed = 95.0;
        const QList<DecisionRow> greedy = computeDecisionRows(m);
        const DecisionRow up = rowFor(greedy, QStringLiteral("UP"));
        QVERIFY(up.haveCrowd);
        QVERIFY(up.crowd < -0.4);
        // With the bearish crowd source blended in, the composite must drop.
        QVERIFY(up.composite < plain.value(0).composite);
        // And the weights renormalise over the grown source set: the crowd
        // enters at 0.10 and the divisor grows from 0.50 to 0.60.
        const double crowdBlend = ((0.35 * techSigned) + (0.10 * up.crowd)) / 0.60;
        QVERIFY(std::abs(up.composite - crowdBlend) < 1e-9);
    }

    //! @tstid TS-DEC-005 @design DES-DOM-DEC
    // @relation(REQ-F-008, REQ-F-009, scope=function)
    void TS_DEC_005_evidenceMentionsCrowd()
    {
        MarketSnapshot m;
        m.fgValid = true;
        m.fearGreed = 12.0;
        m.screenerRows << row(QStringLiteral("UP"), trend(120, 1.004, 1.001));
        const QList<DecisionRow> rows = computeDecisionRows(m);
        const QString evidence = buildDecisionEvidence(rows, m);
        QVERIFY(evidence.contains(QStringLiteral("Fear & Greed")));
        QVERIFY(evidence.contains(QStringLiteral("UP")));
    }

    //! @tstid TS-DEC-006 @design DES-DOM-DEC
    // @relation(REQ-F-022, scope=function)
    void TS_DEC_006_yahooIntradaySource()
    {
        // The tilt reads where the last price sits in the session distribution.
        QList<double> rising;
        QList<double> falling;
        for (qint32 i = 0; i < 120; ++i) {
            rising << 100.0 + i;
            falling << 220.0 - i;
        }
        QVERIFY(intradayTilt(rising) > 0.5);           // last far above the mean
        QVERIFY(intradayTilt(falling) < -0.5);         // last far below the mean
        QCOMPARE(intradayTilt(QList<double>(120, 5.0)), 0.0);  // flat → no read
        QCOMPARE(intradayTilt({1.0, 2.0}), 0.0);       // too short → no read

        // A bullish intraday series lifts the composite; the source is flagged.
        MarketSnapshot m;
        m.screenerRows << row(QStringLiteral("UP"), trend(120, 1.004, 1.001));
        const QList<DecisionRow> without = computeDecisionRows(m);
        m.intradayBySymbol.insert(QStringLiteral("UP"), rising);
        const QList<DecisionRow> with = computeDecisionRows(m);
        QVERIFY(!with.isEmpty());
        QVERIFY(with.value(0).haveYahoo);
        QVERIFY(with.value(0).yahoo > 0.5);
        QVERIFY(with.value(0).composite > without.value(0).composite);
    }
};

QTEST_GUILESS_MAIN(TestDecisionEngine)
#include "tst_decisionengine.moc"
