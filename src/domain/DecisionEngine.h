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
    bool eventRisk = false;   // an imminent high-impact event tempered the confidence
};

// Directional tilt from the CNN Fear & Greed index (0..100): mid readings
// follow the crowd mildly (risk-on lifts indices), extremes fade it (extreme
// fear reads as capitulation, extreme greed as froth — the classic contrarian
// use of the gauge). Returns a value in [-1, 1].
double crowdTilt(double fearGreed);

// Crude keyword sentiment over recent headlines -> [-1, 1]; countOut = headline count.
double newsSentimentScore(const QList<NewsHeadline> &news, qint32 &countOut);

// Market regime tilt in [-1, 1] from the VIX (risk-on/off); sets eventRiskOut when a
// high-impact calendar event is imminent (within six hours).
double marketRegime(const MarketSnapshot &m, bool &eventRiskOut);

// One DecisionRow per instrument with data, sorted by confidence descending.
QList<DecisionRow> computeDecisionRows(const MarketSnapshot &m);

// The plain-text evidence prompt handed to the AI advisor: the candidates, their
// per-source reads and the market context, plus the answer contract.
QString buildDecisionEvidence(const QList<DecisionRow> &rows, const MarketSnapshot &m);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_DECISIONENGINE_H
