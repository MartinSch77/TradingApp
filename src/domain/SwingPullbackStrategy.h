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
// buffer so ordinary noise cannot tag it. Partial exits, the trailing stop and the
// time/session caps are EXIT-time rules that need PaperBook's multi-leg support
// (2026-08-12 redesign item 6, not yet built) and are deliberately NOT implemented
// here — this class answers only "enter or not, and where's the stop", the same
// scope EntrySignal has for the composite/lead bot.
class SwingPullbackStrategyV1 final : public ITradingStrategy
{
public:
    explicit SwingPullbackStrategyV1(SwingPullbackConfig config = {});

    [[nodiscard]] StrategyDecision evaluate(const StrategySnapshot &snapshot) const override;
    [[nodiscard]] QString version() const override;

private:
    SwingPullbackConfig m_config;
};

} // namespace trading

#endif // TRADINGAPP_DOMAIN_SWINGPULLBACKSTRATEGY_H
