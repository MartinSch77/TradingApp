// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_CROWDSCORE_H
#define TRADINGAPP_DOMAIN_CROWDSCORE_H

#include "domain/CrowdObservation.h"

#include <QList>
#include <QString>
#include <QStringList>

// The TRANSPARENT, rule-based Crowd Score (REQ-F-040, Phase 2). It is deliberately NOT a model:
// it is a weighted combination of four normalized component readings, computed by code a person
// can read, so it can serve as the BASELINE a trained model (later phases) must demonstrably
// beat. Nothing here is a probability, and nothing here trades — the score is evidence, gated
// downstream by the deterministic risk rules that already exist.
//
// Pure Qt Core (domain layer): given the readings it produces the score with no store, network
// or GUI, so every rule below is unit-tested directly.
namespace trading::crowd {

// One normalized component reading, as the builder hands it in. `zscore` is oriented so that a
// POSITIVE value is BULLISH — EXCEPT that crowdScore applies the one documented contrarian
// convention itself (retail). `measured` false means the family had no usable value (absent, or
// too little history to normalize): it is then EXCLUDED and named, never counted as zero.
struct ComponentReading {
    Source family = Source::Market;
    bool measured = false;
    double zscore = 0.0;
    Freshness freshness = Freshness::Absent;
    qint64 ageSec = -1;   // age of the underlying datum; -1 when absent
};

// One component as the score reports it — the input plus how it counted.
struct ScoreComponent {
    QString label;                 // "retail" / "options" / "institutional" / "social"
    Source family = Source::Market;
    bool measured = false;
    bool contrarian = false;       // true only for retail (a crowd that is long is bearish)
    double weight = 0.0;           // the configured weight for this family
    double zscore = 0.0;           // the raw reading (before the contrarian flip)
    double contribution = 0.0;     // signed, weight-scaled, in the score's own units
    Freshness freshness = Freshness::Absent;
    qint64 ageSec = -1;
};

// The weights are HYPOTHESES, not validated trading rules — configurable precisely so a backtest
// can tune them without a code change. The defaults are the ones the user proposed.
//
// SIGN CONVENTION, documented per component:
//   * retail (0.35) — CONTRARIAN: a high net-long z is a BEARISH input (crowdScore negates it);
//   * options (0.30) — the builder orients it so bullish positioning is a positive z;
//   * institutional (0.20) — asset-manager/leveraged-fund net long is a positive z;
//   * social (0.15) — net-bullish text is a positive z.
struct CrowdScoreConfig {
    double retailWeight = 0.35;
    double optionsWeight = 0.30;
    double institutionalWeight = 0.20;
    double socialWeight = 0.15;
    double clampZ = 3.0;           // a z beyond this is clamped before scaling into [-1, 1]
    double neutralBand = 0.05;     // |score| within this reads "neutral" rather than a direction
    int minHistory = 3;            // samples of prior history required before a z is trusted
    qint64 staleAfterSec = 24LL * 3600;  // an input older than this is stale (halves confidence)
    qint32 version = 1;            // calculation version stamped on every result
};

// The transparent result. `score` is in [-1, +1]; positive is a crowd-implied BULLISH lean after
// the per-component sign conventions. `confidence` and `coverage` are both in [0, 1].
struct CrowdScoreResult {
    double score = 0.0;
    QString direction;             // "bullish" / "bearish" / "neutral" — word + sign, never colour
    double confidence = 0.0;       // coverage x freshness — low when little or stale data
    double coverage = 0.0;         // measured weight / total weight
    qint64 newestAgeSec = -1;      // freshest and stalest measured input, for the freshness read
    qint64 oldestAgeSec = -1;
    QList<ScoreComponent> components;   // ALWAYS the four families, measured or not
    QStringList warnings;          // every missing or stale component, named
    qint32 version = 1;
    [[nodiscard]] bool isEmpty() const { return coverage <= 0.0; }
    [[nodiscard]] QString headline() const;
};

// Combine the readings into the score. Each of the four families always appears in the result
// (measured or not). MISSING families are excluded, named in warnings, and lower BOTH coverage
// and confidence — never treated as zero, which would fake a neutral crowd. The retail component
// is negated (contrarian). Each measured z is clamped to +/-clampZ and scaled into [-1, 1], then
// weight-averaged over the MEASURED families (weights renormalized), so a partial field still
// yields an honest score at a stated coverage.
[[nodiscard]] CrowdScoreResult crowdScore(const QList<ComponentReading> &readings,
                                          const CrowdScoreConfig &cfg);

} // namespace trading::crowd

#endif // TRADINGAPP_DOMAIN_CROWDSCORE_H
