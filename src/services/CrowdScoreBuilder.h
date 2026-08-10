// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_CROWDSCOREBUILDER_H
#define TRADINGAPP_SERVICES_CROWDSCOREBUILDER_H

#include "domain/CrowdScore.h"

#include <QDateTime>
#include <QString>

// Assembles the transparent Crowd Score from stored observations (REQ-F-040, Phase 2). This is
// the SERVICES-layer orchestration that connects the raw data layer to the pure domain score: it
// reads the store, normalizes each family's latest datum against its own PAST history (a
// leakage-safe z-score), orients the sign so a positive z is bullish, and hands the readings to
// the pure `crowdScore`. It never trades and never touches the network.
namespace trading::crowd {

class CrowdStore;

// The Crowd Score for `instrument` as of `now` (UTC), built from what the store already holds.
// A family with no stored datum, or too little prior history to normalize, is left UNMEASURED
// (never zero) so the result reports honest, reduced coverage. Retail is handed in RAW (positive
// = crowd long) and `crowdScore` applies the contrarian flip; the other families are oriented to
// bullish-positive here (a high put/call ratio is bearish, so options is negated).
[[nodiscard]] CrowdScoreResult buildCrowdScore(const CrowdStore &store, const QString &instrument,
                                               const QDateTime &now, const CrowdScoreConfig &cfg);

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_CROWDSCOREBUILDER_H
