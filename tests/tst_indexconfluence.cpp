#include "domain/IndexConfluence.h"

#include <QSet>
#include <QTest>

using namespace trading;

namespace {

// A session series that ends `changePct` away from where it started, long enough for
// the opening-range read to have something to work with.
QList<double> sessionWith(double changePct, double start = 100.0)
{
    // A real opening has a range: the first thirty bars wiggle (so the opening-range
    // read has a high and a low), and the session then ends `changePct` away from
    // where it opened.
    QList<double> out;
    for (int i = 0; i < 30; ++i) {
        out << (start * (1.0 + (((i % 3) - 1) * 0.0005)));
    }
    for (int i = 0; i < 10; ++i) {
        out << (start * (1.0 + ((changePct / 100.0) * (static_cast<double>(i + 1) / 10.0))));
    }
    return out;
}

// Every reference series present and pointing the same way: falling volatility,
// falling yields, technology leading, every heavyweight of BOTH indices up.
QHash<QString, QList<double>> bullishReferences()
{
    QHash<QString, QList<double>> out;
    static_cast<void>(out.insert(QStringLiteral("NSDQ100.24-7"), sessionWith(1.0)));
    static_cast<void>(out.insert(QStringLiteral("SP.24-7"), sessionWith(0.4)));
    static_cast<void>(out.insert(QStringLiteral("^VIX"), sessionWith(-3.0, 16.0)));
    static_cast<void>(out.insert(QStringLiteral("^VXN"), sessionWith(-4.0, 20.0)));
    static_cast<void>(out.insert(QStringLiteral("^TNX"), sessionWith(-1.5, 42.0)));
    for (const QString &name : referenceTickers()) {
        if (!name.startsWith(QLatin1Char('^'))) {
            static_cast<void>(out.insert(name, sessionWith(1.2)));
        }
    }
    return out;
}

} // namespace

class TestIndexConfluence : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's marker

private slots:
    //! @tstid TS-CONF-001 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    void TS_CONF_001_theReferenceListIsWhatItClaimsToCover()
    {
        // The list IS the documentation of what the participation read covers, so it
        // is worth pinning: the two volatility indices, the yield, and TEN heavyweights
        // per index.
        const QStringList tickers = referenceTickers();
        QVERIFY(tickers.contains(QStringLiteral("^VIX")));
        QVERIFY(tickers.contains(QStringLiteral("^VXN")));
        QVERIFY(tickers.contains(QStringLiteral("^TNX")));
        QCOMPARE(nasdaqHeavyweights().size(), 10);
        QCOMPARE(spHeavyweights().size(), 10);
        for (const QString &name : nasdaqHeavyweights() + spHeavyweights()) {
            QVERIFY2(tickers.contains(name), qPrintable(name));
        }
        // The two lists agree at the top and differ in the tail — which is the whole
        // reason there are two of them. Netflix and Costco carry the Nasdaq; Berkshire
        // and JPMorgan carry the S&P and are not in the Nasdaq-100 at all.
        QVERIFY(nasdaqHeavyweights().contains(QStringLiteral("NFLX")));
        QVERIFY(!spHeavyweights().contains(QStringLiteral("NFLX")));
        QVERIFY(spHeavyweights().contains(QStringLiteral("JPM")));
        QVERIFY(!nasdaqHeavyweights().contains(QStringLiteral("JPM")));
        // A symbol picks its own list: NSDQ100 the Nasdaq's, everything else the S&P's.
        QCOMPARE(indexHeavyweights(QStringLiteral("NSDQ100")), nasdaqHeavyweights());
        QCOMPARE(indexHeavyweights(QStringLiteral("NSDQ100.24-7")), nasdaqHeavyweights());
        QCOMPARE(indexHeavyweights(QStringLiteral("SPX500")), spHeavyweights());
        QCOMPARE(indexHeavyweights(QStringLiteral("GOLD")), spHeavyweights());
        // Three references plus the union of the lists, each ticker fetched ONCE: the
        // eight shared megacaps must not be fetched twice (3 + 8 + 2 + 2).
        QCOMPARE(tickers.size(), 15);
        QCOMPARE(QSet<QString>(tickers.cbegin(), tickers.cend()).size(), tickers.size());
    }

    //! @tstid TS-CONF-002 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    void TS_CONF_002_eachReadSaysWhatItMeasuredOrThatItCouldNot()
    {
        const QHash<QString, QList<double>> refs = bullishReferences();
        const IndexReads reads = indexReads(QStringLiteral("NSDQ100"), refs,
                                            sessionWith(0.8));

        // Technology leading the broad market supports a long, and the read carries
        // the number behind it rather than just a direction.
        QVERIFY(reads.futuresLead.known);
        QCOMPARE(reads.futuresLead.dir, 1);
        QVERIFY(reads.futuresLead.detail.contains(QStringLiteral("Nasdaq vs S&P")));

        // Volatility is read by its DIRECTION, and the Nasdaq is judged by ^VXN.
        QVERIFY(reads.volatility.known);
        QCOMPARE(reads.volatility.dir, 1);              // falling volatility = risk-on
        QVERIFY(reads.volatility.detail.contains(QStringLiteral("^VXN")));
        // …everything else by ^VIX.
        QVERIFY(indexReads(QStringLiteral("SPX500"), refs, sessionWith(0.8))
                    .volatility.detail.contains(QStringLiteral("^VIX")));

        // A falling yield is a tailwind for growth shares; a rising one is not.
        QVERIFY(reads.yields.known);
        QCOMPARE(reads.yields.dir, 1);
        QHash<QString, QList<double>> risingYield = refs;
        static_cast<void>(risingYield.insert(QStringLiteral("^TNX"), sessionWith(2.0, 42.0)));
        QCOMPARE(indexReads(QStringLiteral("NSDQ100"), risingYield, sessionWith(0.8)).yields.dir,
                 -1);

        // Participation: ten of ten up is support, none up is the opposite, and a
        // split field is measured but neutral. The read names WHICH index's ten it
        // counted, because "7 of 10 up" means different things for the two lists.
        QVERIFY(reads.participation.known);
        QCOMPARE(reads.participation.dir, 1);
        QVERIFY(reads.participation.detail.contains(QStringLiteral("10 of 10")));
        QVERIFY(reads.participation.detail.contains(QStringLiteral("Nasdaq-100")));
        QVERIFY(indexReads(QStringLiteral("SPX500"), refs, sessionWith(0.8))
                    .participation.detail.contains(QStringLiteral("S&P 500")));
        QHash<QString, QList<double>> mixed = refs;
        const QStringList heavies = nasdaqHeavyweights();
        for (qsizetype i = 0; i < heavies.size(); ++i) {
            static_cast<void>(mixed.insert(heavies.at(i), sessionWith((i % 2 == 0) ? 1.0 : -1.0)));
        }
        QCOMPARE(indexReads(QStringLiteral("NSDQ100"), mixed, sessionWith(0.8)).participation.dir,
                 0);
        // And the point of two lists: ONE set of series, two honest answers, because the
        // tails differ. Two shared megacaps down plus the S&P's own tail down leaves the
        // Nasdaq's ten broadly up (8 of 10) and the S&P's a split field (6 of 10).
        QHash<QString, QList<double>> tails = refs;
        const QStringList downNames{QStringLiteral("META"), QStringLiteral("TSLA"),
                                    QStringLiteral("BRK-B"), QStringLiteral("JPM")};
        for (const QString &down : downNames) {
            static_cast<void>(tails.insert(down, sessionWith(-1.0)));
        }
        QCOMPARE(indexReads(QStringLiteral("NSDQ100"), tails, sessionWith(0.8)).participation.dir,
                 1);
        QCOMPARE(indexReads(QStringLiteral("SPX500"), tails, sessionWith(0.8)).participation.dir,
                 0);

        // Structure: where price sits against its own opening range.
        QVERIFY(reads.structure.known);

        // And the honest part: with no reference series at all, every read reports
        // that it could not be measured — none of them defaults to bullish.
        const IndexReads blind = indexReads(QStringLiteral("NSDQ100"), {}, {});
        QVERIFY(!blind.futuresLead.known);
        QVERIFY(!blind.volatility.known);
        QVERIFY(!blind.yields.known);
        QVERIFY(!blind.participation.known);
        QVERIFY(!blind.structure.known);
        // Fewer than half the heavyweights readable is not a participation read.
        QHash<QString, QList<double>> thin;
        static_cast<void>(thin.insert(heavies.constFirst(), sessionWith(1.0)));
        QVERIFY(!indexReads(QStringLiteral("NSDQ100"), thin, {}).participation.known);
    }

    //! @tstid TS-CONF-003 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    void TS_CONF_003_unknownNeverCountsAsAgreement()
    {
        // The whole point of the score: "four of five agree" has to mean four MEASURED
        // things agreed, or the number is a lie told by absent feeds.
        const IndexReads bullish =
            indexReads(QStringLiteral("NSDQ100"), bullishReferences(), sessionWith(0.8));
        const Confluence forLong = confluenceFor(bullish, 1);
        QCOMPARE(forLong.unknown, 0);
        QVERIFY(forLong.met >= 4);
        QCOMPARE(forLong.against, 0);
        QCOMPARE(forLong.measured(), forLong.met + forLong.against);
        QVERIFY(forLong.reasons.join(u"; ").contains(QStringLiteral("agrees")));

        // The same reads, scored for the other side, disagree instead — a read is not
        // "bullish", it either supports the side asked about or contradicts it.
        const Confluence forShort = confluenceFor(bullish, -1);
        QCOMPARE(forShort.against, forLong.met);
        QCOMPARE(forShort.met, forLong.against);
        QVERIFY(forShort.reasons.join(u"; ").contains(QStringLiteral("disagrees")));

        // With nothing measurable, nothing agrees and nothing contradicts: five
        // unknowns, no confluence, and a caller that requires agreement gets none.
        const Confluence blind = confluenceFor(indexReads(QStringLiteral("NSDQ100"), {}, {}), 1);
        QCOMPARE(blind.met, 0);
        QCOMPARE(blind.against, 0);
        QCOMPARE(blind.unknown, 5);
        QCOMPARE(blind.measured(), 0);
        QVERIFY(blind.reasons.join(u"; ").contains(QStringLiteral("unknown")));

        // No side to agree with is not a score at all.
        const Confluence noSide = confluenceFor(bullish, 0);
        QCOMPARE(noSide.met, 0);
        QCOMPARE(noSide.measured(), 0);
        QVERIFY(noSide.reasons.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestIndexConfluence)
#include "tst_indexconfluence.moc"
