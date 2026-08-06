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

    //! @tstid TS-DEC-007 @design DES-DOM-DEC
    // @relation(REQ-F-008, REQ-F-009, REQ-F-030, scope=function)
    void TS_DEC_007_theEvidencePromptCarriesEverySourceItClaimsTo()
    {
        // The prompt IS the interface to the models (cloud and local alike), so what
        // it does and does not say about a candidate is behaviour, not formatting.
        MarketSnapshot m;
        m.vixValid = true;
        m.vix = 31.0;                       // risk-off
        m.fgValid = true;
        m.fearGreed = 9.0;                  // fear
        m.screenerRows << row(QStringLiteral("UP"), trend(120, 1.004, 1.001))
                       << row(QStringLiteral("DOWN"), trend(120, 0.996, 1.001));
        WebRating rating;
        rating.m15 = 0.8;
        rating.h1 = 0.6;
        rating.d1 = std::nan("");           // a missing timeframe is simply left out
        QVERIFY(rating.valid());
        QVERIFY(std::abs(rating.consensus() - 0.7) < 1e-9);
        static_cast<void>(m.ratingBySymbol.insert(QStringLiteral("UP"), rating));
        NewsHeadline head;
        head.title = QStringLiteral("Rally broadens as inflation cools");
        static_cast<void>(m.newsBySymbol.insert(QStringLiteral("UP"), {head}));

        const QList<DecisionRow> rows = computeDecisionRows(m);
        const DecisionRow up = rowFor(rows, QStringLiteral("UP"));
        QVERIFY(up.haveRating);
        QVERIFY(std::abs(up.rating - 0.7) < 1e-9);
        QVERIFY(up.haveNews);

        const QString evidence = buildDecisionEvidence(rows, m);
        QVERIFY(evidence.contains(QStringLiteral("risk-off")));      // the VIX regime
        QVERIFY(evidence.contains(QStringLiteral("fear")));          // the crowd
        QVERIFY(evidence.contains(QStringLiteral("TV rating")));     // the web rating
        QVERIFY(evidence.contains(QStringLiteral("News")));          // the sentiment read
        QVERIFY(evidence.contains(head.title));                      // the headline itself
        QVERIFY(evidence.contains(QStringLiteral("maxLev")));
        QVERIFY(evidence.contains(QStringLiteral("BUY")));
        QVERIFY(evidence.contains(QStringLiteral("SELL")));          // both sides are offered

        // The other regimes word themselves differently, and a market with nothing
        // to say says exactly that instead of presenting an empty list as choice.
        m.vix = 12.0;
        QVERIFY(buildDecisionEvidence(rows, m).contains(QStringLiteral("risk-on")));
        m.vix = 20.0;
        QVERIFY(buildDecisionEvidence(rows, m).contains(QStringLiteral("neutral")));
        m.fearGreed = 88.0;
        QVERIFY(buildDecisionEvidence(rows, m).contains(QStringLiteral("greed")));
        QVERIFY(buildDecisionEvidence({}, m).contains(QStringLiteral("HOLD")));

        // The rating wording is one shared table — the ranked list, the signals panel
        // and the prompt must read a score identically.
        QCOMPARE(webRatingWord(0.9), QStringLiteral("Strong Buy"));
        QCOMPARE(webRatingWord(0.2), QStringLiteral("Buy"));
        QCOMPARE(webRatingWord(0.0), QStringLiteral("Neutral"));
        QCOMPARE(webRatingWord(-0.2), QStringLiteral("Sell"));
        QCOMPARE(webRatingWord(-0.9), QStringLiteral("Strong Sell"));

        // A rating with no timeframe at all contributes nothing rather than a NaN.
        const WebRating empty;
        QVERIFY(!empty.valid());
        QVERIFY(std::isnan(empty.consensus()));
        MarketSnapshot blank = m;
        blank.ratingBySymbol.clear();
        static_cast<void>(blank.ratingBySymbol.insert(QStringLiteral("UP"), empty));
        QVERIFY(!rowFor(computeDecisionRows(blank), QStringLiteral("UP")).haveRating);
    }

    //! @tstid TS-DEC-008 @design DES-DOM-DEC
    // @relation(REQ-F-022, scope=function)
    void TS_DEC_008_theSessionsOwnStructureIsReadBeforeAnyOscillator()
    {
        // The opening range: the first 30 minutes set a high and a low, and where the
        // price sits against them is the read professionals take first.
        QList<double> session;
        for (int i = 0; i < 30; ++i) {
            session << (100.0 + (i % 5));            // opens between 100 and 104
        }
        const OpeningRange inside = openingRange(session + QList<double>{102.0, 102.5, 103.0});
        QVERIFY(inside.valid);
        QCOMPARE(inside.low, 100.0);
        QCOMPARE(inside.high, 104.0);
        QCOMPARE(inside.bars, qsizetype{30});
        QCOMPARE(inside.breakDir, 0);                // still inside it
        QCOMPARE(inside.breakPct, 0.0);

        // A break upward, measured as a share of the range's OWN width (4.0 here), so
        // a probe and a decisive break are not the same number.
        const OpeningRange up = openingRange(session + QList<double>{104.5, 105.0, 106.0});
        QCOMPARE(up.breakDir, 1);
        QVERIFY(qAbs(up.breakPct - 50.0) < 1e-9);    // 2.0 beyond a 4.0-wide range
        const OpeningRange down = openingRange(session + QList<double>{99.0, 98.0, 98.0});
        QCOMPARE(down.breakDir, -1);
        QVERIFY(qAbs(down.breakPct - 50.0) < 1e-9);

        // Too little session, and a flat opening, both answer "no read" rather than
        // inventing a range.
        QVERIFY(!openingRange(QList<double>(10, 100.0)).valid);
        QVERIFY(!openingRange(QList<double>(40, 100.0)).valid);
        QVERIFY(!openingRange({}).valid);
        QVERIFY(!openingRange(session + QList<double>{102.0}, 0).valid);

        // Relative strength: the session return of one series minus the other, which
        // is how "is technology leading the broad market" gets answered from the two
        // futures series the app already fetches.
        const QList<double> nasdaq{100.0, 101.0, 102.0};      // +2.0%
        const QList<double> sp{100.0, 100.5, 101.0};          // +1.0%
        QVERIFY(qAbs(relativeStrength(nasdaq, sp) - 1.0) < 1e-9);
        QVERIFY(qAbs(relativeStrength(sp, nasdaq) + 1.0) < 1e-9);
        QCOMPARE(relativeStrength(nasdaq, {}), 0.0);          // no read, not a claim
        QCOMPARE(relativeStrength({}, sp), 0.0);
        QCOMPARE(relativeStrength({100.0}, {100.0}), 0.0);

        // Both reads reach the model: the prompt states the range and the leadership.
        MarketSnapshot m;
        m.screenerRows << row(QStringLiteral("UP"), trend(120, 1.004, 1.001));
        static_cast<void>(m.intradayBySymbol.insert(QStringLiteral("UP"),
                                                    session + QList<double>{106.0, 106.0, 106.0}));
        static_cast<void>(m.intradayBySymbol.insert(QStringLiteral("NSDQ100.24-7"), nasdaq));
        static_cast<void>(m.intradayBySymbol.insert(QStringLiteral("SP.24-7"), sp));
        const QString evidence = buildDecisionEvidence(computeDecisionRows(m), m);
        QVERIFY(evidence.contains(QStringLiteral("opening range")));
        QVERIFY(evidence.contains(QStringLiteral("Nasdaq future vs S&P future")));
        QVERIFY(evidence.contains(QStringLiteral("leading")));
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
    //! @tstid TS-DEC-009 @design DES-DOM-DEC
    // @relation(REQ-F-008, scope=function)
    void TS_DEC_009_theCompositeUsesWhatItHasAndSaysWhenItHasNothing()
    {
        // The rule the decision rests on: sources that are ABSENT are left out and the
        // remaining weights are renormalised — never filled in with a zero, which
        // would read as a neutral opinion nobody gave.
        MarketSnapshot m;
        m.screenerRows = {row(QStringLiteral("SPX500"), trend(120, 1.004, 1.001))};

        // A row with no sources beyond price still produces a decision…
        const QList<DecisionRow> bare = computeDecisionRows(m);
        QCOMPARE(bare.size(), 1);
        QVERIFY(bare.constFirst().haveTech);

        // …a row that is NOT ok, or has no closes at all, is skipped entirely rather
        // than decided on nothing.
        ScreenerRow broken = row(QStringLiteral("GOLD"), trend(120, 1.002, 0.999));
        broken.ok = false;
        ScreenerRow empty = row(QStringLiteral("OIL"), {});
        m.screenerRows = {broken, empty, row(QStringLiteral("SPX500"), trend(120, 1.004, 1.001))};
        const QList<DecisionRow> filtered = computeDecisionRows(m);
        QCOMPARE(filtered.size(), 1);
        QCOMPARE(filtered.constFirst().symbol, QStringLiteral("SPX500"));

        // A series too short for the ensemble leaves haveTech false — and the row is
        // still produced, because the other sources may still say something.
        m.screenerRows = {row(QStringLiteral("SPX500"), QList<double>(5, 100.0))};
        const QList<DecisionRow> shortSeries = computeDecisionRows(m);
        QCOMPARE(shortSeries.size(), 1);
        QVERIFY(!shortSeries.constFirst().haveTech);

        // The VIX regime has three bands and each one moves the composite its own way:
        // calm supports risk, elevated presses on it, and panic presses harder.
        const auto regimeOf = [](double vix) {
            MarketSnapshot s;
            s.vixValid = true;
            s.vix = vix;
            return marketRegime(s).tilt;
        };
        QVERIFY(regimeOf(12.0) > 0.0);      // calm
        QCOMPARE(regimeOf(20.0), 0.0);      // ordinary band: no opinion
        QVERIFY(regimeOf(28.0) < 0.0);      // elevated
        QVERIFY(regimeOf(40.0) < regimeOf(28.0));   // panic presses harder
        // No VIX at all is not a calm market: it is no reading.
        MarketSnapshot noVix;
        QCOMPARE(marketRegime(noVix).tilt, 0.0);

        // Event risk trims conviction rather than flipping direction — a scheduled
        // release makes a call less certain, not wrong.
        MarketSnapshot calm;
        calm.screenerRows = {row(QStringLiteral("SPX500"), trend(120, 1.004, 1.001))};
        MarketSnapshot risky = calm;
        EconomicEvent event;
        event.title = QStringLiteral("CPI");
        event.impact = QStringLiteral("High");
        event.when = QDateTime::currentDateTimeUtc().addSecs(1800);
        risky.events = {event};
        const DecisionRow quiet = computeDecisionRows(calm).constFirst();
        const DecisionRow loud = computeDecisionRows(risky).constFirst();
        QCOMPARE(quiet.dir, loud.dir);
        QVERIFY(loud.confidence <= quiet.confidence);
    }
};

QTEST_GUILESS_MAIN(TestDecisionEngine)
#include "tst_decisionengine.moc"
