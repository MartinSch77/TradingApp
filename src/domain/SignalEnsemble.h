// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_SIGNALENSEMBLE_H
#define TRADINGAPP_DOMAIN_SIGNALENSEMBLE_H

#include <QList>
#include <QString>

// The buy/sell ensemble: a directional vote across the technical indicators,
// shared by the live signals panel, the leverage screener and the decision
// window so a symbol ranks identically everywhere.
namespace trading {

// Returns the raw score/votes and the pre-trim confidence; the VIX-level and
// event-risk confidence haircuts are applied separately (applyVixHaircut) as
// they fold in live, instrument-agnostic context. `vixValid`/`vixChangePct`
// drive the VIX directional vote (a VIX stretched far from its norm is
// risk-off/on for indices).
struct Ensemble {
    bool valid = false;    // false = not enough data (series too short)
    qint32 score = 0;      // net directional vote
    qint32 votes = 0;      // number of indicators that voted
    double confidence = 0.0;  // |score| / votes * 100 (raw, before trims)
    qint32 dir = 0;        // sign(score): +1 up / -1 down / 0 flat
    double vol = 0.0;      // volatilityPct(series, 20), the expected per-bar move
    QString signal;        // "BUY" (score>=2) / "SELL" (score<=-2) / "NEUTRAL"
    qint32 signalDir = 0;  // +1 BUY / -1 SELL / 0 NEUTRAL
};

[[nodiscard]] Ensemble computeEnsemble(const QList<double> &series, bool vixValid,
                                       double vixChangePct);

// High absolute VIX = a fearful, choppy tape: trim confidence (instrument-
// agnostic). The same haircut is applied wherever an ensemble confidence is
// shown, so the live panel, screener and recommendations agree.
[[nodiscard]] double applyVixHaircut(double confidence, bool vixValid,
                                     double vixLevel) noexcept;

} // namespace trading

#endif // TRADINGAPP_DOMAIN_SIGNALENSEMBLE_H
