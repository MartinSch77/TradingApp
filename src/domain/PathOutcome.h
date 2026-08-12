// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_PATHOUTCOME_H
#define TRADINGAPP_DOMAIN_PATHOUTCOME_H

#include "domain/PredictionLedger.h"
#include "domain/TradingStrategy.h"

// Path-dependent outcome labelling (2026-08-12 redesign, item 8): every RECORDED
// decision — including the ones that stayed out — gets a later label, and the
// label is decided by the PATH the price actually took, not by an endpoint
// question ("was it higher after 180 minutes"). If the stop would have been hit
// before a later favourable move, that was never a winning trade regardless of
// where the price ended up — the same principle StrategyBacktest's conservative
// same-candle rule already encodes for a live position, applied here to every
// decision point instead of only the ones that were actually taken. This closes
// the selection-bias gap BotNet's current training has: it only ever sees
// outcomes for entries the existing gate already chose, never what a REFUSED
// candidate or the OPPOSITE side would have done.
//
// Follows PredictionLedger's own pattern deliberately: a label is RESOLVED on
// demand from later data (like resolveOutcome), never stored as a permanent
// field on Prediction — a ledger row's own facts (what was measured, what was
// decided) stay separate from what can only be known once the future arrives.
namespace trading {

enum class PathLabel : quint8 { NoTrade = 0, Long, Short };

[[nodiscard]] QString pathLabelWord(PathLabel label);

// What "+1R"/"-1R" and "cleared costs" mean, made explicit rather than
// hardcoded: R is stopFraction × entryPrice: cfg.targetR/cfg.stopR scale it
// into the target/stop distance actually walked, and costBufferFraction adds
// a round-trip-cost buffer ON TOP of the raw R target — a move that only
// reaches +1R nominally but does not also clear that buffer does not count as
// a real win ("deckte Kosten und Mindestbewegung").
struct PathOutcomeConfig {
    double targetR = 1.0;
    double stopR = 1.0;
    double stopFraction = 0.0;         // R as a fraction of entry price; must be > 0
    double costBufferFraction = 0.0;   // round-trip cost, added to the target distance
    qint32 maxBars = 20;               // how far ahead to look before giving up (NoTrade)
};

// What ONE hypothetical direction (long or short) achieved walking the path
// bar by bar from entryPrice: whether its own target was reached before its
// own stop, within maxBars. Same-candle ambiguity (a bar reaching both) is
// resolved the SAME conservative way StrategyBacktest's resolveBarAgainstStop
// resolves a live stop: the stop is assumed to have been hit FIRST.
struct DirectionalPathResult {
    bool reachedTargetFirst = false;
    qint32 barsToResolve = 0;   // 0 = never resolved within maxBars
};
[[nodiscard]] DirectionalPathResult walkPath(const QList<DailyBar> &bars, double entryPrice,
                                             bool isLong, const PathOutcomeConfig &cfg);

// The full label: walks BOTH a hypothetical long and a hypothetical short from
// entryPrice over `bars` (the bars AFTER the decision point, oldest first).
// Long/Short cannot both win on the same path (their stops/targets sit on
// opposite sides of entry); when neither resolves in its own favour — either
// bar's own combination never triggers wins, or neither ever resolves within
// maxBars at all — the label is NoTrade: neither side covered its own costs
// and minimum move, which is exactly the design's own definition of it.
[[nodiscard]] PathLabel resolvePathLabel(const QList<DailyBar> &bars, double entryPrice,
                                        const PathOutcomeConfig &cfg);

// Bridges resolvePathLabel to PredictionLedger's own data, per the design's
// explicit instruction that training data should come FROM the ledger. A
// Prediction's `price` is a single point, not an OHLC bar — later rows for
// the SAME instrument (any order, matching resolveOutcome's own convention)
// become degenerate bars (open = high = low = close = that row's price).
// This UNDERSTATES how far price actually reached between two recorded
// points — a real intrabar excursion the ledger never sampled is invisible
// here — which is a real, stated limitation of resolving against point
// samples rather than true market bars, not a hidden one. Rows are matched by
// symbol and sorted by time before walking; nothing before `decision.at` is
// included (no lookahead by construction, since only `later` is consulted).
[[nodiscard]] PathLabel resolvePathLabelForPrediction(const Prediction &decision,
                                                       const QList<Prediction> &later,
                                                       const PathOutcomeConfig &cfg);

} // namespace trading

#endif // TRADINGAPP_DOMAIN_PATHOUTCOME_H
