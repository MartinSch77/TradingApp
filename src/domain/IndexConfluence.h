#ifndef TRADINGAPP_DOMAIN_INDEXCONFLUENCE_H
#define TRADINGAPP_DOMAIN_INDEXCONFLUENCE_H

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// What the index instruments are really doing, from the reference series the app can
// actually fetch (REQ-F-035).
//
// The premise, which is worth stating because it contradicts how most indicator
// panels are built: RSI, MACD, stochastic and a stack of moving averages are all
// derived from the same closes, so adding them does not add evidence. What adds
// evidence is AGREEMENT BETWEEN INDEPENDENT THINGS — price structure, the futures
// that lead the cash market, expected volatility, the yield that moves growth
// shares, and whether the index's biggest constituents are actually participating.
// This module computes those reads and counts how many of them agree on a side.
//
// Two rules keep it honest:
//
//  * A read that could not be computed is UNKNOWN, and unknown never counts as
//    agreement. A confluence of "four conditions met" must mean four measured
//    conditions, not four absent feeds.
//  * Nothing here is breadth in the professional sense. Real breadth needs the
//    advance/decline line, up-volume and the share of constituents above VWAP —
//    per-constituent data this app does not fetch. Heavyweight participation is the
//    stand-in, and it is named as one wherever it is shown.
namespace trading {

// The Yahoo tickers the reference sweep fetches, in one list: ^VIX, ^VXN, ^TNX and
// the union of the two heavyweight lists below (15 tickers — the lists share eight
// megacaps, which is itself why a heavyweight read says something about both indices).
[[nodiscard]] QStringList referenceTickers();
// The ten names that carry most of each index by weight. Separate lists on purpose:
// the indices agree at the top and differ in the tail (NFLX/COST move the Nasdaq,
// BRK-B/JPM move the S&P), so reading the wrong tail reads a different index.
[[nodiscard]] QStringList nasdaqHeavyweights();
[[nodiscard]] QStringList spHeavyweights();
// The list that applies to `symbol`: the Nasdaq's for an NSDQ/NQ instrument, the
// S&P's for everything else (any other symbol borrows the broad-market read).
[[nodiscard]] QStringList indexHeavyweights(const QString &symbol);

// One read, and whether it was measurable at all.
struct Read {
    bool known = false;
    // +1 supports a long, −1 supports a short, 0 measured but neutral.
    qint32 dir = 0;
    QString detail;   // the number behind it, for the tooltip and the prompt
};

// Everything the reference series say, for one index instrument.
struct IndexReads {
    Read futuresLead;      // Nasdaq future vs S&P future this session
    Read volatility;       // the instrument's own volatility index, rising or falling
    Read yields;           // the US 10-year, rising (headwind) or falling (tailwind)
    Read participation;    // how many of THAT index's heavyweights are up on the session
    Read structure;        // where price sits against its own opening range
};

// `series` is keyed by reference ticker (as fetched) plus the app's own instrument
// series for the structure read. `symbol` selects both index-specific reads: NSDQ100
// is judged by ^VXN and the Nasdaq-100 heavyweights, everything else by ^VIX and the
// S&P 500's.
[[nodiscard]] IndexReads indexReads(const QString &symbol,
                                    const QHash<QString, QList<double>> &referenceSeries,
                                    const QList<double> &ownSeries);

// One heavyweight constituent, as the early-warning view shows it (REQ-F-035): the
// name, how far it has moved on the session, and whether that was measurable at all.
// A name whose series is missing is UNKNOWN — the same rule the reads follow, because
// "flat" and "not fetched" are different facts and only one of them is evidence.
struct HeavyweightRow {
    QString ticker;
    bool known = false;
    double changePct = 0.0;
};

// What the top-ten constituents of one index are doing, together: the rows, how many
// were readable, how many are up, and the AVERAGE move of the readable ones.
//
// Why average rather than index-weighted: this app does not fetch index weights, and
// a weighted number computed from invented weights would look more authoritative than
// it is. The average of the ten biggest names is an honest stand-in and is labelled
// as one — see the note on breadth above.
struct HeavyweightPulse {
    QString indexName;              // "Nasdaq-100" / "S&P 500"
    QList<HeavyweightRow> rows;
    qint32 measured = 0;
    qint32 up = 0;
    double averageChangePct = 0.0;
    // The strongest and the weakest readable name — one name carrying an index is a
    // different situation from ten moving together, and that is the whole point of
    // watching the constituents rather than only the index.
    QString leader;
    double leaderChangePct = 0.0;
    QString laggard;
    double laggardChangePct = 0.0;

    [[nodiscard]] bool isEmpty() const { return measured == 0; }
    // A one-line summary in the words the window shows.
    [[nodiscard]] QString headline() const;
};

// The pulse of the index `symbol` belongs to, from the reference series already
// fetched for the confluence reads — no additional feed.
[[nodiscard]] HeavyweightPulse heavyweightPulse(const QString &symbol,
                                                const QHash<QString, QList<double>> &series);

// How many reads agree with `dir`, how many contradict it, and how many could not be
// measured — plus a one-line summary naming each. `met >= 4` is the bar a
// professional dashboard would ask for; whether the bot enforces it is configuration.
struct Confluence {
    qint32 met = 0;
    qint32 against = 0;
    qint32 unknown = 0;
    QStringList reasons;   // "futures lead agrees (+0.42%)", "yields disagree (+1.8%)", …
    [[nodiscard]] qint32 measured() const { return met + against; }
};
[[nodiscard]] Confluence confluenceFor(const IndexReads &reads, qint32 dir);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_INDEXCONFLUENCE_H
