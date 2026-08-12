// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_SWINGPULLBACKSTRATEGY_H
#define TRADINGAPP_DOMAIN_SWINGPULLBACKSTRATEGY_H

#include "domain/TradingStrategy.h"

namespace trading {

// Tunable parameters, broken out so the strategy is testable against synthetic bar
// counts far shorter than a real 200+ session history.
struct SwingPullbackConfig {
    qint32 fastEmaPeriod = 20;
    qint32 midEmaPeriod = 50;
    qint32 slowEmaPeriod = 200;
    qint32 atrPeriod = 14;
    qint32 minPullbackSessions = 2;
    qint32 maxPullbackSessions = 5;
    // How close the pullback's lowest close must come to the fast EMA, in units of
    // ATR, to count as "pulled back toward EMA20" rather than a shallow non-event.
    double emaProximityAtr = 1.0;
    double stopAtrBuffer = 0.5;
    // A VIX9D/VIX3M ratio above this is a strongly inverted term structure — the
    // market paying up for near-term protection right now, which this strategy
    // sits out rather than sizes down (contrast with termStructure()'s own use
    // elsewhere as a damper, never a direction: here it is strong enough to
    // refuse the entry outright).
    double maxTermStructureRatio = 1.05;

    // --- Exit-time parameters (2026-08-12 redesign, item 6) ---------------------
    // Take partialCloseFraction off the stake once the gain reaches this many R
    // (multiples of the ORIGINAL entry-to-stop distance). 2R is the design's own
    // figure: banking part of the winner before the give-back rule ever has
    // something to give back.
    double partialTargetR = 2.0;
    double partialCloseFraction = 0.45;   // 40-50%, the design's own range's midpoint
    // If still under timeStopMinR after timeStopSessions, the setup has not
    // delivered what it needed to by now — close rather than keep waiting.
    qint32 timeStopSessions = 5;
    double timeStopMinR = 0.5;
    // Close regardless past this many sessions, win or lose — a swing trade is
    // meant to resolve within days, not become an unbounded hold.
    qint32 maxHoldSessions = 10;
};

// The trend-pullback strategy (2026-08-12 redesign, item 5): only trades WITH an
// established daily uptrend, on a shallow, controlled pullback that has just turned
// back up — never a breakout chase and never a falling knife. SPX500-only at first;
// the class itself is instrument-agnostic — what makes it SPX500-only is which
// snapshot the caller builds and feeds it.
//
// Filters, in the order the design specifies:
//  1. Daily close above EMA200, and EMA20 > EMA50 > EMA200 (an established uptrend).
//  2. No strongly inverted VIX term structure.
//  3. No imminent high-impact event.
//  4. A controlled pullback of 2-5 sessions off a recent swing high — "controlled"
//     read here as a clean run of non-increasing closes, not a choppy round trip.
//  5. The pullback's lowest close comes within one ATR of the fast EMA (today's
//     value is used as the pullback-window proxy, since both move slowly over a
//     2-5 day window). The design's other two qualifying approaches — a prior
//     breakout level and an anchored VWAP — are NOT implemented yet; a pullback
//     that only qualifies by one of those two is refused here rather than
//     silently approved on the EMA test alone, which is why this is stated
//     rather than left to be discovered as a gap.
//  6. Confirmed TODAY: a higher low than yesterday, or a close above yesterday's
//     high — the reversal has to show itself, not be inferred from the pullback
//     merely ending.
//
// The stop sits below the pullback's own low (by bar LOW, not close), with an ATR
// buffer so ordinary noise cannot tag it. This class answers only "enter or not,
// and where's the stop" — the same scope EntrySignal has for the composite/lead
// bot; the exit-time rules below (swingExitDecision) are a separate pure function,
// not a method here, because they need day-by-day STATE (has the partial already
// fired, how many sessions held) that an entry evaluation does not.
class SwingPullbackStrategyV1 final : public ITradingStrategy
{
public:
    explicit SwingPullbackStrategyV1(SwingPullbackConfig config = {});

    [[nodiscard]] StrategyDecision evaluate(const StrategySnapshot &snapshot) const override;
    [[nodiscard]] QString version() const override;

private:
    SwingPullbackConfig m_config;
};

// What must be tracked ACROSS calls for one open swing position — kept separate
// from PaperTrade so this one strategy's bookkeeping does not grow a struct
// every other strategy also carries; the caller (BotSimRunner, once wired)
// persists this itself alongside the position id. stopPrice must be SEEDED with
// the entry's own initial stop when the position opens — swingExitDecision only
// ever tightens it from there, never loosens it (the design's own "stop never
// moved outward" rule).
struct SwingPositionState {
    bool partialTaken = false;
    double stopPrice = 0.0;
    qint32 sessionsHeld = 0;
};

// What to do with an open swing position today, and the state to keep for
// tomorrow's call.
struct SwingExitAction {
    bool partialClose = false;   // true -> close partialFraction of the stake now
    double partialFraction = 0.0;
    bool fullClose = false;      // true -> close what remains (mutually exclusive
                                 // with partialClose: the position is gone either way)
    QString code;                // "hold" while holding, else the reason category
    QString why;
    SwingPositionState nextState;
};

// What swingExitDecision needs beyond the state it tracks and the config it
// reads, bundled so the function itself stays under the parameter-count
// budget. entryPrice/initialStopPrice fix R = entryPrice - initialStopPrice —
// long-only, matching SwingPullbackStrategyV1's own scope.
struct SwingExitInputs {
    double entryPrice = 0.0;
    double initialStopPrice = 0.0;
    double todayClose = 0.0;
    double fiveDayLow = 0.0;
    double ema10 = 0.0;
};

// The four exit rules the design specifies, checked in the order that matters —
// a full close beats a partial, and a stale/degenerate R refuses to compute
// anything rather than dividing by a non-positive number:
//  1. Max hold: past maxHoldSessions, close everything regardless of result.
//  2. Time stop: past timeStopSessions still under timeStopMinR, close
//     everything — the setup has not delivered what it needed to by now.
//  3. The 2R partial (once): close partialCloseFraction of the stake.
//  4. The trailing stop: once in profit, tighten (never loosen) toward the
//     higher of the 5-day low or the 10-day EMA — reported back in
//     nextState.stopPrice, not enforced here (a live price-crossing check
//     against it is the caller's job, the same way a stop/target barrier is
//     checked elsewhere in this codebase).
[[nodiscard]] SwingExitAction swingExitDecision(const SwingPositionState &state,
                                                const SwingExitInputs &in,
                                                const SwingPullbackConfig &cfg);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_SWINGPULLBACKSTRATEGY_H
