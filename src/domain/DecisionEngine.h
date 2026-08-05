#ifndef TRADINGAPP_DOMAIN_DECISIONENGINE_H
#define TRADINGAPP_DOMAIN_DECISIONENGINE_H

#include "domain/Models.h"

#include <QHash>
#include <QList>
#include <QString>

// The decision engine: blends every independent source (technical ensemble,
// TradingView rating, news sentiment, VIX/calendar regime) into one weighted
// composite call per instrument. Pure functions over a MarketSnapshot — the UI
// gathers the data, the engine decides.
namespace trading {

// Everything the engine needs to reason about the market, captured as plain
// data. The UI fills one from its latest feeds and hands it in.
struct MarketSnapshot {
    QList<ScreenerRow> screenerRows;                    // per-instrument close series
    QHash<QString, WebRating> ratingBySymbol;           // web rating per instrument
    QHash<QString, QList<NewsHeadline>> newsBySymbol;   // headlines per instrument
    bool vixValid = false;                              // a VIX reading has been received
    double vix = 0.0;                                   // latest VIX level
    double vixChangePct = 0.0;                          // VIX deviation vs. its norm (%)
    QList<EconomicEvent> events;                        // upcoming calendar events
    bool fgValid = false;                               // a Fear & Greed reading arrived
    double fearGreed = 50.0;                            // CNN Fear & Greed, 0..100
    // Independent Yahoo Finance intraday series (1-minute closes, session so
    // far) per instrument — an additional source for the composite.
    QHash<QString, QList<double>> intradayBySymbol;
    // The reference series that are not instruments: ^VIX, ^VXN, ^TNX and the top-ten
    // constituents of the Nasdaq-100 and the S&P 500 (REQ-F-035), keyed by Yahoo ticker.
    QHash<QString, QList<double>> referenceSeries;
};

// One aggregated row per instrument, combining every source into a weighted
// composite call; the strongest is the recommendation.
struct DecisionRow {
    QString symbol;
    qint32 dir = 0;           // +1 BUY / -1 SELL / 0 neutral (sign of composite)
    double composite = 0.0;   // weighted blend of the sources, in [-1, 1]
    double confidence = 0.0;  // |composite| * 100, after any event/VIX haircut
    qint32 maxLev = 0;
    // Per-source sub-reads (for the sources table + the AI evidence prompt).
    bool haveTech = false;
    qint32 techDir = 0;
    double techConf = 0.0;
    QString techLabel;        // BUY / SELL / NEUTRAL
    bool haveRating = false;
    double rating = 0.0;      // multi-timeframe consensus, [-1, 1]
    bool haveNews = false;
    double newsScore = 0.0;   // [-1, 1]
    qint32 newsCount = 0;
    double regime = 0.0;      // VIX/market regime tilt, [-1, 1]
    bool haveCrowd = false;   // a Fear & Greed reading fed the composite
    double crowd = 0.0;       // crowd-sentiment tilt, [-1, 1] (see crowdTilt)
    bool haveYahoo = false;   // a Yahoo intraday series fed the composite
    double yahoo = 0.0;       // intraday momentum tilt, [-1, 1] (see intradayTilt)
    bool eventRisk = false;   // an imminent high-impact event tempered the confidence
};

// Directional tilt from the CNN Fear & Greed index (0..100): mid readings
// follow the crowd mildly (risk-on lifts indices), extremes fade it (extreme
// fear reads as capitulation, extreme greed as froth — the classic contrarian
// use of the gauge). Returns a value in [-1, 1].
[[nodiscard]] double crowdTilt(double fearGreed) noexcept;

// Crude keyword sentiment over recent headlines.
struct NewsRead {
    double score = 0.0;  // [-1, 1]
    qint32 count = 0;    // headlines scored
};
[[nodiscard]] NewsRead newsSentimentScore(const QList<NewsHeadline> &news);

// Session-momentum tilt in [-1, 1] from an independent intraday close series
// (Yahoo Finance, 1-minute bars): where the last price sits relative to the
// session mean, in units of the session's own dispersion. Needs ≥ 30 points;
// returns 0 otherwise (and for flat series).
[[nodiscard]] double intradayTilt(const QList<double> &closes);

// ---------------------------------------------------------------------------
// Session structure (REQ-F-022)
// ---------------------------------------------------------------------------
//
// Two reads that professionals watch before any oscillator, computed from the
// 1-minute session series the app already fetches — no new feed, no new key:
//
//  * the OPENING RANGE. The first half hour of a session sets a high and a low
//    that the rest of the day trades around; a break of it with the session
//    behind it is the classic intraday continuation signal, and a trade taken
//    AGAINST a fresh break is the classic way to be run over.
//  * RELATIVE STRENGTH between two instruments. The Nasdaq future leading or
//    lagging the S&P future says whether a move is broad or is technology
//    alone, which is the cheapest available stand-in for market breadth here:
//    real breadth needs per-constituent data this app does not fetch.
struct OpeningRange {
    bool valid = false;
    double high = 0.0;
    double low = 0.0;
    qsizetype bars = 0;    // how many of the session's minutes formed the range
    // +1 = the latest price is above the range, −1 = below, 0 = still inside it.
    qint32 breakDir = 0;
    // How far beyond the range, as a percentage of the range's own width. A big
    // number is a decisive break; a fraction is a probe.
    double breakPct = 0.0;
};

// `minutes` is the length of the opening window in BARS of the series (the app's
// intraday series is one bar per minute). Needs at least the window plus a few
// bars after it, or the answer is "not valid" rather than a guess.
[[nodiscard]] OpeningRange openingRange(const QList<double> &closes, qsizetype minutes = 30);

// Session return of `leader` minus that of `benchmark`, in percentage points:
// positive = the leader is outperforming. Both series must cover the same session;
// an empty or single-point series yields 0 (no read), never a fabricated one.
[[nodiscard]] double relativeStrength(const QList<double> &leader,
                                      const QList<double> &benchmark);

// Market regime from the VIX plus the calendar: the risk-on/off tilt and
// whether a high-impact event is imminent (within six hours).
struct RegimeRead {
    double tilt = 0.0;      // [-1, 1], risk-off negative
    bool eventRisk = false; // an imminent high-impact calendar event
};
[[nodiscard]] RegimeRead marketRegime(const MarketSnapshot &m);

// One DecisionRow per instrument with data, sorted by confidence descending.
[[nodiscard]] QList<DecisionRow> computeDecisionRows(const MarketSnapshot &m);

// The plain-text evidence prompt handed to the AI advisor: the candidates, their
// per-source reads and the market context, plus the answer contract.
//
// `maxCandidates` bounds how many instruments the model is SHOWN, and therefore how
// many it can possibly say anything about — an instrument missing from the prompt
// cannot be given an opinion, however much the reader expects one. The decision
// window asks for a handful (it wants the best pick); the bot asks for many more,
// because its window reports the model's read per instrument and "no opinion" has
// to mean "it looked and passed", not "it was never shown this one".
[[nodiscard]] QString buildDecisionEvidence(const QList<DecisionRow> &rows,
                                            const MarketSnapshot &m,
                                            qsizetype maxCandidates = 6);

// TradingView's rating buckets for a recommendation score in [-1, 1]. One
// shared bucket table: the ranked table, the signals panel and the feed label
// must word the same score identically.
[[nodiscard]] QString webRatingWord(double score);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_DECISIONENGINE_H
