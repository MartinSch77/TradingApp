// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

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

// Every reference series present and pointing the same way: falling volatility, a
// normal term structure, falling yields, every heavyweight of BOTH indices up. Keyed by
// YAHOO TICKER — the futures proxies deliberately do NOT live here (see futuresBook).
QHash<QString, QList<double>> bullishReferences()
{
    QHash<QString, QList<double>> out;
    static_cast<void>(out.insert(QStringLiteral("^VIX"), sessionWith(-3.0, 16.0)));
    static_cast<void>(out.insert(QStringLiteral("^VXN"), sessionWith(-4.0, 20.0)));
    static_cast<void>(out.insert(QStringLiteral("^TNX"), sessionWith(-1.5, 42.0)));
    // Near leg below the far leg: a normal, non-inverted curve.
    static_cast<void>(out.insert(QStringLiteral("^VIX9D"), sessionWith(-2.0, 14.0)));
    static_cast<void>(out.insert(QStringLiteral("^VIX3M"), sessionWith(-1.0, 18.0)));
    // The front end falling faster than the long end: easing pressure.
    static_cast<void>(out.insert(QStringLiteral("2YY=F"), sessionWith(-3.0, 4.0)));
    for (const QString &name : nasdaqHeavyweights() + spHeavyweights()) {
        static_cast<void>(out.insert(name, sessionWith(1.2)));
    }
    return out;
}

// The futures proxies, keyed by APP SYMBOL. A separate book on purpose: these are this
// app's own instruments, not Yahoo tickers, and the futures reads look for them HERE.
QHash<QString, QList<double>> futuresBook()
{
    QHash<QString, QList<double>> out;
    static_cast<void>(out.insert(QStringLiteral("NSDQ100.24-7"), sessionWith(1.0)));
    static_cast<void>(out.insert(QStringLiteral("SP.24-7"), sessionWith(0.4)));
    return out;
}

// Volume bars for every heavyweight of both indices, all bought up through the session.
QHash<QString, VolumeSeries> bullishVolumes();

// The bundle as production assembles it, with the instrument's own session supplied
// explicitly (the traded index is not one of the two futures symbols).
ReadInputs inputsFor(const QString &symbol, const QHash<QString, QList<double>> &refs,
                     const QList<double> &own,
                     const QHash<QString, VolumeSeries> &volumes = {})
{
    ReadInputs in = readInputsFor(symbol, refs, volumes, futuresBook());
    in.ownSeries = own;
    return in;
}

// Bars whose closes rise/fall by `changePct` on a flat volume profile: the last close
// then sits above (or below) the session VWAP by construction.
VolumeSeries barsWith(double changePct, double volumePerBar = 1000.0)
{
    VolumeSeries out;
    out.closes = sessionWith(changePct);
    for (const double close : out.closes) {
        static_cast<void>(close);   // one volume bar per close bar, aligned by construction
        out.volumes.append(volumePerBar);
    }
    return out;
}

QHash<QString, VolumeSeries> bullishVolumes()
{
    QHash<QString, VolumeSeries> out;
    for (const QString &name : nasdaqHeavyweights() + spHeavyweights()) {
        static_cast<void>(out.insert(name, barsWith(1.2)));
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
        // The term-structure legs and the short end of the curve.
        QVERIFY(tickers.contains(QStringLiteral("^VIX9D")));
        QVERIFY(tickers.contains(QStringLiteral("^VIX3M")));
        QVERIFY(tickers.contains(QStringLiteral("2YY=F")));
        QVERIFY(tickers.contains(QStringLiteral("^IRX")));
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
        // Seven non-instrument references plus the union of the lists, each ticker
        // fetched ONCE: the eight shared megacaps must not be fetched twice
        // (7 + 8 + 2 + 2).
        QCOMPARE(tickers.size(), 19);
        QCOMPARE(QSet<QString>(tickers.cbegin(), tickers.cend()).size(), tickers.size());
    }

    //! @tstid TS-CONF-002 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    void TS_CONF_002_eachReadSaysWhatItMeasuredOrThatItCouldNot()
    {
        const QHash<QString, QList<double>> refs = bullishReferences();
        const IndexReads reads =
            indexReads(QStringLiteral("NSDQ100"), inputsFor(QStringLiteral("NSDQ100"), refs,
                                                            sessionWith(0.8)));

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
        QVERIFY(indexReads(QStringLiteral("SPX500"),
                           inputsFor(QStringLiteral("SPX500"), refs, sessionWith(0.8)))
                    .volatility.detail.contains(QStringLiteral("^VIX")));

        // A falling yield is a tailwind for growth shares; a rising one is not.
        QVERIFY(reads.yields.known);
        QCOMPARE(reads.yields.dir, 1);
        QHash<QString, QList<double>> risingYield = refs;
        static_cast<void>(risingYield.insert(QStringLiteral("^TNX"), sessionWith(2.0, 42.0)));
        QCOMPARE(indexReads(QStringLiteral("NSDQ100"),
                            inputsFor(QStringLiteral("NSDQ100"), risingYield, sessionWith(0.8)))
                     .yields.dir,
                 -1);

        // Participation: ten of ten up is support, none up is the opposite, and a
        // split field is measured but neutral. The read names WHICH index's ten it
        // counted, because "7 of 10 up" means different things for the two lists.
        QVERIFY(reads.participation.known);
        QCOMPARE(reads.participation.dir, 1);
        QVERIFY(reads.participation.detail.contains(QStringLiteral("10 of 10")));
        QVERIFY(reads.participation.detail.contains(QStringLiteral("Nasdaq-100")));
        QVERIFY(indexReads(QStringLiteral("SPX500"),
                           inputsFor(QStringLiteral("SPX500"), refs, sessionWith(0.8)))
                    .participation.detail.contains(QStringLiteral("S&P 500")));
        QHash<QString, QList<double>> mixed = refs;
        const QStringList heavies = nasdaqHeavyweights();
        for (qsizetype i = 0; i < heavies.size(); ++i) {
            static_cast<void>(mixed.insert(heavies.at(i), sessionWith((i % 2 == 0) ? 1.0 : -1.0)));
        }
        QCOMPARE(indexReads(QStringLiteral("NSDQ100"),
                            inputsFor(QStringLiteral("NSDQ100"), mixed, sessionWith(0.8)))
                     .participation.dir,
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
        QCOMPARE(indexReads(QStringLiteral("NSDQ100"),
                            inputsFor(QStringLiteral("NSDQ100"), tails, sessionWith(0.8)))
                     .participation.dir,
                 1);
        QCOMPARE(indexReads(QStringLiteral("SPX500"),
                            inputsFor(QStringLiteral("SPX500"), tails, sessionWith(0.8)))
                     .participation.dir,
                 0);

        // Structure: where price sits against its own opening range.
        QVERIFY(reads.structure.known);

        // And the honest part: with no reference series at all, every read reports
        // that it could not be measured — none of them defaults to bullish.
        const IndexReads blind = indexReads(QStringLiteral("NSDQ100"), ReadInputs{});
        QVERIFY(!blind.futuresLead.known);
        QVERIFY(!blind.futuresMomentum.known);
        QVERIFY(!blind.volatility.known);
        QVERIFY(!blind.yields.known);
        QVERIFY(!blind.curve.known);
        QVERIFY(!blind.participation.known);
        QVERIFY(!blind.aboveVwap.known);
        QVERIFY(!blind.upDownVolume.known);
        QVERIFY(!blind.structure.known);
        // Fewer than half the heavyweights readable is not a participation read.
        QHash<QString, QList<double>> thin;
        static_cast<void>(thin.insert(heavies.constFirst(), sessionWith(1.0)));
        QVERIFY(!indexReads(QStringLiteral("NSDQ100"),
                            inputsFor(QStringLiteral("NSDQ100"), thin, {}))
                     .participation.known);
    }

    //! @tstid TS-CONF-003 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    void TS_CONF_003_unknownNeverCountsAsAgreement()
    {
        // The whole point of the score: "four of five agree" has to mean four MEASURED
        // things agreed, or the number is a lie told by absent feeds.
        // Every one of the nine reads measurable — including the two that need volume,
        // because "no read is unknown" is only a meaningful claim when all of them could
        // have been.
        const IndexReads bullish =
            indexReads(QStringLiteral("NSDQ100"),
                       inputsFor(QStringLiteral("NSDQ100"), bullishReferences(),
                                 sessionWith(0.8), bullishVolumes()));
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
        const Confluence blind =
            confluenceFor(indexReads(QStringLiteral("NSDQ100"), ReadInputs{}), 1);
        QCOMPARE(blind.met, 0);
        QCOMPARE(blind.against, 0);
        QCOMPARE(blind.unknown, 9);
        QCOMPARE(blind.measured(), 0);
        QVERIFY(blind.reasons.join(u"; ").contains(QStringLiteral("unknown")));

        // No side to agree with is not a score at all.
        const Confluence noSide = confluenceFor(bullish, 0);
        QCOMPARE(noSide.met, 0);
        QCOMPARE(noSide.measured(), 0);
        QVERIFY(noSide.reasons.isEmpty());
    }
    //! @tstid TS-CONF-004 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    void TS_CONF_004_theHeavyweightPulseSummarisesWhatWasActuallyRead()
    {
        // The early-warning view (the "Heavyweights" window) is built from this, and
        // its whole value depends on two things being true: the numbers are the ones
        // measured, and a name that could not be read is not counted as flat.
        QHash<QString, QList<double>> series;
        const QStringList nasdaq = nasdaqHeavyweights();
        // Eight of the ten up by a little, NFLX up a lot, COST down a lot.
        for (const QString &name : nasdaq) {
            double move = 0.5;
            if (name == QStringLiteral("NFLX")) {
                move = 4.0;
            } else if (name == QStringLiteral("COST")) {
                move = -3.0;
            }
            static_cast<void>(series.insert(name, sessionWith(move)));
        }

        const HeavyweightPulse pulse = heavyweightPulse(QStringLiteral("NSDQ100"), series);
        QCOMPARE(pulse.indexName, QStringLiteral("Nasdaq-100"));
        QCOMPARE(pulse.rows.size(), 10);
        QCOMPARE(pulse.measured, 10);
        QCOMPARE(pulse.up, 9);
        QCOMPARE(pulse.leader, QStringLiteral("NFLX"));
        QCOMPARE(pulse.laggard, QStringLiteral("COST"));
        QVERIFY(pulse.leaderChangePct > pulse.laggardChangePct);
        // The average is of the READABLE names, and it sits between the extremes.
        QVERIFY(pulse.averageChangePct < pulse.leaderChangePct);
        QVERIFY(pulse.averageChangePct > pulse.laggardChangePct);
        // The headline names the index and carries the count, so the window's summary
        // line cannot drift away from the numbers behind it.
        QVERIFY(pulse.headline().contains(QStringLiteral("Nasdaq-100")));
        QVERIFY(pulse.headline().contains(QStringLiteral("9 of 10")));
        QVERIFY(pulse.headline().contains(QStringLiteral("NFLX")));

        // An index whose names were NOT fetched: every row is present but unknown,
        // and nothing is counted — "no data" must not read as "a flat market".
        const HeavyweightPulse blind = heavyweightPulse(QStringLiteral("SPX500"), {});
        QCOMPARE(blind.indexName, QStringLiteral("S&P 500"));
        QCOMPARE(blind.rows.size(), 10);
        QVERIFY(blind.isEmpty());
        QCOMPARE(blind.measured, 0);
        QCOMPARE(blind.up, 0);
        QCOMPARE(blind.averageChangePct, 0.0);
        QVERIFY(blind.headline().contains(QStringLiteral("no constituent prices")));
        for (const HeavyweightRow &row : blind.rows) {
            QVERIFY(!row.known);
            QVERIFY(!row.ticker.isEmpty());
        }

        // A PARTIAL field: three of the S&P's ten readable, two of them up. The counts
        // are of what was measured, never of the list length.
        QHash<QString, QList<double>> partial;
        const QStringList sp = spHeavyweights();
        static_cast<void>(partial.insert(sp.at(0), sessionWith(1.0)));
        static_cast<void>(partial.insert(sp.at(1), sessionWith(2.0)));
        static_cast<void>(partial.insert(sp.at(2), sessionWith(-1.0)));
        const HeavyweightPulse thin = heavyweightPulse(QStringLiteral("SPX500"), partial);
        QCOMPARE(thin.measured, 3);
        QCOMPARE(thin.up, 2);
        QCOMPARE(thin.rows.size(), 10);
        QVERIFY(thin.headline().contains(QStringLiteral("2 of 3")));
        // …and the two indices really do read different names: the Nasdaq's pulse over
        // the same S&P-only series is emptier than the S&P's.
        QVERIFY(heavyweightPulse(QStringLiteral("NSDQ100"), partial).measured <= thin.measured);
    }

    //! @tstid TS-CONF-007 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    //
    // The CAP-WEIGHTED constituent lead: the summarised up/down indicator the user asked for,
    // weighting each name by its share of the index. Its whole reason to exist is that it can
    // disagree with the equal-weight count — "the index lags its top constituents" — so the
    // decisive test is exactly that case: the two HEAVIEST names carry the field one way while
    // the majority (and the plain average) point the other.
    void TS_CONF_007_theCapWeightedLeadCanDisagreeWithTheCount()
    {
        QHash<QString, QList<double>> series;
        // NVDA (heaviest) and MSFT (second) up strongly; the other eight down. So the COUNT is
        // 2-of-10 up and the equal-weight AVERAGE is negative, but the two megacaps outweigh the
        // rest, so the cap-weighted move is positive: the top names are pulling the index up.
        for (const QString &name : nasdaqHeavyweights()) {
            const bool heavy = (name == QStringLiteral("NVDA")) || (name == QStringLiteral("MSFT"));
            static_cast<void>(series.insert(name, sessionWith(heavy ? 5.0 : -2.0)));
        }
        const HeavyweightPulse pulse = heavyweightPulse(QStringLiteral("NSDQ100"), series);
        QCOMPARE(pulse.measured, 10);
        QCOMPARE(pulse.up, 2);
        QVERIFY(pulse.averageChangePct < 0.0);          // the equal-weight read says DOWN…
        QVERIFY(pulse.capWeightedChangePct > 0.0);      // …the cap-weighted read says UP.
        // The headline carries both numbers, and the compact indicator carries the direction as
        // an arrow (▲ for the positive cap-weighted move) plus the index and the breadth count.
        QVERIFY(pulse.headline().contains(QStringLiteral("cap-wt")));
        const QString indicator = pulse.leadIndicator();
        QVERIFY(indicator.contains(QStringLiteral("Nasdaq-100")));
        QVERIFY(indicator.contains(QString(QChar(0x25B2))));   // ▲ up
        QVERIFY(indicator.contains(QStringLiteral("2/10 up")));

        // When every name is DOWN, the indicator points down (▼), and an unread field says so
        // rather than inventing a flat 0% direction.
        QHash<QString, QList<double>> allDown;
        for (const QString &name : nasdaqHeavyweights()) {
            static_cast<void>(allDown.insert(name, sessionWith(-1.0)));
        }
        const HeavyweightPulse down = heavyweightPulse(QStringLiteral("NSDQ100"), allDown);
        QVERIFY(down.capWeightedChangePct < 0.0);
        QVERIFY(down.leadIndicator().contains(QString(QChar(0x25BC))));   // ▼ down
        QVERIFY(heavyweightPulse(QStringLiteral("SPX500"), {}).leadIndicator()
                    .contains(QStringLiteral("no prices")));
    }

    //! @tstid TS-CONF-005 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    void TS_CONF_005_theVolumeReadsAnswerWhereTheBuyingHappened()
    {
        // Volume is what separates "the names are up" from "the names are being bought",
        // and both reads here are UNKNOWN without it rather than assumed neutral.
        const QStringList nasdaq = nasdaqHeavyweights();
        QHash<QString, VolumeSeries> volumes;
        for (const QString &name : nasdaq) {
            static_cast<void>(volumes.insert(name, barsWith(1.5)));
        }
        const IndexReads reads =
            indexReads(QStringLiteral("NSDQ100"),
                       inputsFor(QStringLiteral("NSDQ100"), bullishReferences(),
                                 sessionWith(0.8), volumes));

        // Ten of ten closing above their own session VWAP is the strongest form the
        // stand-in takes — and it says out loud that it IS a stand-in for breadth.
        QVERIFY(reads.aboveVwap.known);
        QCOMPARE(reads.aboveVwap.dir, 1);
        QVERIFY(reads.aboveVwap.detail.contains(QStringLiteral("10 of 10")));
        QVERIFY(reads.aboveVwap.detail.contains(QStringLiteral("stand-in for breadth")));
        QVERIFY(reads.upDownVolume.known);
        QCOMPARE(reads.upDownVolume.dir, 1);

        // The read that the plain up-count cannot make: SIX names up, four down, so the
        // count is positive — but the four down names carry five times the volume, so
        // the volume is behind the sellers and the read says so.
        QHash<QString, VolumeSeries> heavySelling;
        for (qsizetype i = 0; i < nasdaq.size(); ++i) {
            const bool up = (i < 6);
            static_cast<void>(heavySelling.insert(
                nasdaq.at(i), barsWith(up ? 1.0 : -1.0, up ? 1000.0 : 5000.0)));
        }
        const IndexReads split =
            indexReads(QStringLiteral("NSDQ100"),
                       inputsFor(QStringLiteral("NSDQ100"), bullishReferences(),
                                 sessionWith(0.8), heavySelling));
        QVERIFY(split.upDownVolume.known);
        QCOMPARE(split.upDownVolume.dir, -1);
        // …and the same field read WITHOUT volume weighting is positive by count, which
        // is exactly the disagreement that makes this an independent read.
        QCOMPARE(split.aboveVwap.dir, 0);   // six above, four below: a split field

        // No volume at all — a volatility or yield ticker, or a feed that served none.
        // The reads must be unknown, never a neutral zero.
        const IndexReads dry =
            indexReads(QStringLiteral("NSDQ100"),
                       inputsFor(QStringLiteral("NSDQ100"), bullishReferences(),
                                 sessionWith(0.8)));
        QVERIFY(!dry.aboveVwap.known);
        QVERIFY(!dry.upDownVolume.known);

        // Bars whose two halves have drifted out of step are refused rather than
        // averaged across the shift: a VWAP off by one minute is not a VWAP.
        VolumeSeries misaligned = barsWith(1.0);
        misaligned.volumes.removeLast();
        QVERIFY(!misaligned.vwap().has_value());
        // And bars with prices but no traded volume behind them (an index ticker).
        const VolumeSeries noTurnover = barsWith(1.0, 0.0);
        QVERIFY(!noTurnover.vwap().has_value());
        QVERIFY(!noTurnover.totalVolume().has_value());
    }

    //! @tstid TS-CONF-006 @design DES-DOM-CONFLUENCE
    // @relation(REQ-F-035, scope=function)
    void TS_CONF_006_theFuturesReadsComeFromTheSymbolBookNotTheTickerBook()
    {
        // A REGRESSION test, and the reason this one exists is worth stating: the futures
        // lead — the most immediate directional signal there is — used to be looked up in
        // the Yahoo TICKER book, while the futures proxies are keyed by this app's own
        // INSTRUMENT SYMBOL and live in the other book. It was therefore permanently
        // unknown in the running app, and the unit test passed because the test put them
        // in the book the read was searching.
        const QHash<QString, QList<double>> refs = bullishReferences();

        // With the symbol book supplied, both futures reads are measurable.
        const IndexReads wired =
            indexReads(QStringLiteral("NSDQ100"),
                       inputsFor(QStringLiteral("NSDQ100"), refs, sessionWith(0.8)));
        QVERIFY(wired.futuresLead.known);
        QCOMPARE(wired.futuresLead.dir, 1);            // technology leading the broad market
        QVERIFY(wired.futuresMomentum.known);
        QCOMPARE(wired.futuresMomentum.dir, 1);        // 1m, 5m and 15m all pushing up
        QVERIFY(wired.futuresMomentum.detail.contains(QStringLiteral("NSDQ100.24-7")));

        // The futures in the TICKER book and nothing in the symbol book is the defect's
        // shape: both reads must report unmeasurable rather than find them anyway.
        QHash<QString, QList<double>> wrongBook = refs;
        static_cast<void>(wrongBook.insert(QStringLiteral("NSDQ100.24-7"), sessionWith(1.0)));
        static_cast<void>(wrongBook.insert(QStringLiteral("SP.24-7"), sessionWith(0.4)));
        ReadInputs misfiled;
        misfiled.reference = wrongBook;
        misfiled.ownSeries = sessionWith(0.8);
        const IndexReads unwired = indexReads(QStringLiteral("NSDQ100"), misfiled);
        QVERIFY(!unwired.futuresLead.known);
        QVERIFY(!unwired.futuresMomentum.known);

        // Momentum is ONE read over three horizons, not three reads: horizons that
        // disagree are the neutral they describe, not a vote for the shortest one.
        // Twenty minutes of steady selling, then a five-minute bounce: the 15-minute
        // return is negative while the 1- and 5-minute returns are positive.
        QList<double> reversing;
        for (int i = 0; i < 20; ++i) {
            reversing.append(100.0 - (i * 0.1));
        }
        for (int i = 0; i < 5; ++i) {
            reversing.append(98.1 + ((i + 1) * 0.05));
        }
        QHash<QString, QList<double>> reversal;
        static_cast<void>(reversal.insert(QStringLiteral("NSDQ100.24-7"), reversing));
        ReadInputs mixedHorizons;
        mixedHorizons.reference = refs;
        mixedHorizons.bySymbol = reversal;
        const IndexReads fading = indexReads(QStringLiteral("NSDQ100"), mixedHorizons);
        QVERIFY(fading.futuresMomentum.known);
        QCOMPARE(fading.futuresMomentum.dir, 0);
        QVERIFY(fading.futuresMomentum.detail.contains(QStringLiteral("disagree")));

        // The curve read: the front end falling faster than the long end is easing
        // pressure and supports a long; the reverse presses on growth shares. It names
        // WHICH front-end instrument it actually read.
        QVERIFY(wired.curve.known);
        QCOMPARE(wired.curve.dir, 1);
        QVERIFY(wired.curve.detail.contains(QStringLiteral("US 2y")));
        QHash<QString, QList<double>> tightening = refs;
        static_cast<void>(tightening.insert(QStringLiteral("2YY=F"), sessionWith(4.0, 4.0)));
        QCOMPARE(indexReads(QStringLiteral("NSDQ100"),
                            inputsFor(QStringLiteral("NSDQ100"), tightening, sessionWith(0.8)))
                     .curve.dir,
                 -1);
        // Without the 2-year, the 13-week bill stands in — and the read says which.
        QHash<QString, QList<double>> billOnly = refs;
        static_cast<void>(billOnly.remove(QStringLiteral("2YY=F")));
        static_cast<void>(billOnly.insert(QStringLiteral("^IRX"), sessionWith(-3.0, 5.0)));
        const Read fallback =
            indexReads(QStringLiteral("NSDQ100"),
                       inputsFor(QStringLiteral("NSDQ100"), billOnly, sessionWith(0.8)))
                .curve;
        QVERIFY(fallback.known);
        QVERIFY(fallback.detail.contains(QStringLiteral("US 13w")));

        // The term structure is a REGIME read and never a direction: it reports whether
        // the near leg is priced above the far one, and nothing about which way to trade.
        const TermStructure normal = termStructure(refs);
        QVERIFY(normal.known);
        QVERIFY(!normal.inverted);
        QVERIFY(normal.nearFarRatio < 1.0);
        QHash<QString, QList<double>> stressed = refs;
        static_cast<void>(stressed.insert(QStringLiteral("^VIX9D"), sessionWith(20.0, 30.0)));
        const TermStructure inverted = termStructure(stressed);
        QVERIFY(inverted.known);
        QVERIFY(inverted.inverted);
        QVERIFY(inverted.detail.contains(QStringLiteral("INVERTED")));
        // Thirty-day stands in when three-month is missing, and an absent near leg is
        // honestly unmeasurable.
        QHash<QString, QList<double>> noFarLeg = refs;
        static_cast<void>(noFarLeg.remove(QStringLiteral("^VIX3M")));
        QVERIFY(termStructure(noFarLeg).known);
        QHash<QString, QList<double>> noNearLeg = refs;
        static_cast<void>(noNearLeg.remove(QStringLiteral("^VIX9D")));
        QVERIFY(!termStructure(noNearLeg).known);
        QVERIFY(termStructure(noNearLeg).detail.contains(QStringLiteral("not measurable")));
    }
};

QTEST_GUILESS_MAIN(TestIndexConfluence)
#include "tst_indexconfluence.moc"
