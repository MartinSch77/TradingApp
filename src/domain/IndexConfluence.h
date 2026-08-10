// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_INDEXCONFLUENCE_H
#define TRADINGAPP_DOMAIN_INDEXCONFLUENCE_H

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

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

// One ticker's session bars as the sweep delivers them: the closes, and — when the
// feed carries volume for that ticker — the volume of the SAME bars, index for index.
//
// The alignment is the entire point. The close sweep skips empty minutes, and a volume
// array parsed independently skips a DIFFERENT set of minutes; a VWAP computed from
// two such series is a number about nothing. These two lists are therefore filled
// together, and a bar is kept only when both halves are present.
struct VolumeSeries {
    QList<double> closes;
    QList<double> volumes;

    // Volume-weighted average price of the session so far, or nothing when the bars
    // cannot support one.
    [[nodiscard]] std::optional<double> vwap() const;
    [[nodiscard]] std::optional<double> totalVolume() const;
};

// Everything the reads are computed from, in ONE bundle — and the bundle exists to
// make a specific defect unrepresentable rather than for tidiness.
//
// Two books arrive from two different worlds: the Yahoo references are keyed by
// TICKER (^VIX, ^TNX, AAPL), the futures proxies by this app's own INSTRUMENT SYMBOL
// (SP.24-7, NSDQ100.24-7). They were once one parameter, and the futures-lead read —
// the most immediate directional signal there is — looked for NSDQ100.24-7 in the
// ticker book, where nothing ever put it. It was therefore permanently UNKNOWN in the
// running app while its unit test passed, because the test put the futures in the
// book the read was searching. Named fields make that mistake visible at every call
// site.
struct ReadInputs {
    QHash<QString, QList<double>> reference;      // by Yahoo ticker
    QHash<QString, VolumeSeries> volume;          // by Yahoo ticker, closes+volumes aligned
    QHash<QString, QList<double>> bySymbol;       // by APP symbol — the futures proxies
    QList<double> ownSeries;                      // the instrument's own 1-minute closes
};

// Everything the reference series say, for one index instrument.
//
// Nine reads, not five, and the three added from volume are the ones a professional
// dashboard would reach for first. What is deliberately NOT here: order flow (volume
// delta, cumulative delta, bid/ask imbalance, absorbed liquidity) needs CME level-2
// tick data, and this app sees one bid/ask with no sizes and candles with no volume at
// all. An honest gap beats a proxy wearing the name of the real thing.
struct IndexReads {
    Read futuresLead;      // Nasdaq future vs S&P future this session
    Read futuresMomentum;  // the leading future's 1/5/15-minute returns, agreeing or not
    Read volatility;       // the instrument's own volatility index, rising or falling
    Read yields;           // the US 10-year, rising (headwind) or falling (tailwind)
    Read curve;            // the short end against the long end — the policy read
    Read participation;    // how many of THAT index's heavyweights are up on the session
    Read aboveVwap;        // how many of them trade above their OWN session VWAP
    Read upDownVolume;     // whether the volume sits behind the up names or the down ones
    Read structure;        // where price sits against its own opening range
};

// The reads for `symbol`. It selects every index-specific one: NSDQ100 is judged by
// ^VXN, the Nasdaq-100 heavyweights and the Nasdaq future, everything else by ^VIX,
// the S&P 500's names and the S&P future.
[[nodiscard]] IndexReads indexReads(const QString &symbol, const ReadInputs &in);

// The bundle, assembled from the two books the app already keeps. Every caller goes
// through this: the mapping from "which book holds what" to "which read needs which"
// is stated ONCE, so a window and the bot cannot each get it wrong in a different way.
// `ownSeries` is the instrument's own series out of the symbol book — the same place
// the futures proxies live, because the traded instrument is one of this app's symbols
// too, not a Yahoo ticker.
[[nodiscard]] ReadInputs readInputsFor(const QString &symbol,
                                       const QHash<QString, QList<double>> &reference,
                                       const QHash<QString, VolumeSeries> &volume,
                                       const QHash<QString, QList<double>> &bySymbol);

// The volatility TERM STRUCTURE: nine-day expected volatility against thirty-day
// against three-month. Not a direction and never counted as one — an inverted curve
// (the near term priced above the far) says the market is paying up for protection
// RIGHT NOW, which is a statement about how much the next print can overrule, not
// about which way it goes. It therefore only ever reduces conviction.
struct TermStructure {
    bool known = false;
    bool inverted = false;
    double nearFarRatio = 1.0;   // ^VIX9D / ^VIX3M, or the closest pair available
    QString detail;
};
[[nodiscard]] TermStructure termStructure(const QHash<QString, QList<double>> &referenceSeries);

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
// were readable, how many are up, the AVERAGE move of the readable ones, and the
// CAP-WEIGHTED move — each name's session change scaled by its (approximate) share of
// the index, renormalised over the readable names.
//
// Two numbers, deliberately: the average treats the ten names equally; the cap-weighted
// number answers the question the user actually asked — "in which direction are the top
// constituents pulling the index, by their portion in it?" — since the index itself is
// cap-weighted and NVDA moving 2% is not AMZN moving 2%. The weights are a STATIC snapshot
// (see heavyweightWeight in the .cpp): this app does not fetch live index weightings, so
// the cap-weighted lead is an approximate stand-in, the honest counterpart of the breadth
// caveat above — not a claim to the index's exact construction. Both numbers share one
// sign convention: positive == the top names are net up.
struct HeavyweightPulse {
    QString indexName;              // "Nasdaq-100" / "S&P 500"
    QList<HeavyweightRow> rows;
    qint32 measured = 0;
    qint32 up = 0;
    double averageChangePct = 0.0;
    // The top-ten move weighted by each name's share of the index (the "constituent lead").
    // Its SIGN is the summarised direction the user asked to see: > 0 up together, < 0 down.
    double capWeightedChangePct = 0.0;
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
    // The compact "summarised indicator" the user asked for: the cap-weighted direction of
    // the top-ten as an arrow + signed percent + breadth (e.g. "Nasdaq-100 top-10 ▲ +0.42%
    // (8/10 up)"). Direction is the arrow AND the sign, never colour — it reads the same in
    // a monochrome capture and for a colour-blind trader, like the rest of the app's meters.
    [[nodiscard]] QString leadIndicator() const;
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
