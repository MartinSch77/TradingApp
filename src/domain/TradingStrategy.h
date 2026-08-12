// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_TRADINGSTRATEGY_H
#define TRADINGAPP_DOMAIN_TRADINGSTRATEGY_H

#include "domain/IndexConfluence.h"

#include <QList>
#include <QString>

// Strategy abstraction (2026-08-12 redesign, item 5): the bot's default composite/lead
// blend stays exactly as it is, but new trading IDEAS — a swing pullback strategy today,
// others later — go through this interface instead of another conditions chain folded
// into BotSimRunner. The point is comparability: several strategies can be run and
// scored SEPARATELY (see Prediction::strategyVersion), rather than one growing chain
// whose pieces cannot be measured apart from each other.
namespace trading {

// One daily OHLC bar, oldest first — the timeframe a swing strategy reasons over,
// distinct from the 1-minute/hourly series the rest of the app uses for intraday
// confluence and Monte-Carlo planning.
struct DailyBar {
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;   // 0 = unknown — some feeds omit it; never guessed at
};

// Everything ONE strategy evaluation needs, as plain data — mirroring
// CandidateInput's own "everything the bot knows, as a bundle" pattern, so a
// strategy is a pure function of its snapshot and testable without a live feed.
struct StrategySnapshot {
    QString symbol;
    QList<DailyBar> bars;              // oldest first, enough history for the longest EMA used
    TermStructure termStructure;       // VIX9D/VIX3M — a regime damper, reused from IndexConfluence
    bool eventRiskImminent = false;    // a scheduled CPI/NFP/FOMC print is near
};

// What a strategy decided, and why — mirroring EntrySignal/EntryVerdict's own
// "the why is what the log states" convention.
struct StrategyDecision {
    bool enter = false;
    bool isBuy = true;
    double stopFraction = 0.0;   // stop distance from entry, as a fraction of price
    QString why;                  // one line, always populated — a refusal explains itself too
    QString code;                 // stable refusal/entry category for a scan summary
};

// One trading strategy, evaluated over one instrument's own daily bars.
class ITradingStrategy
{
public:
    ITradingStrategy() = default;
    virtual ~ITradingStrategy() = default;
    // Deleted rather than defaulted: copying/assigning through the base would slice a
    // concrete strategy down to nothing but the interface's (empty) own state.
    ITradingStrategy(const ITradingStrategy &) = delete;
    ITradingStrategy &operator=(const ITradingStrategy &) = delete;
    ITradingStrategy(ITradingStrategy &&) = delete;
    ITradingStrategy &operator=(ITradingStrategy &&) = delete;
    [[nodiscard]] virtual StrategyDecision evaluate(const StrategySnapshot &snapshot) const = 0;
    // A short, stable name recorded in Prediction::strategyVersion, so a ledger
    // mixing several strategies' calls can be scored separately.
    [[nodiscard]] virtual QString version() const = 0;
};

} // namespace trading

#endif // TRADINGAPP_DOMAIN_TRADINGSTRATEGY_H
