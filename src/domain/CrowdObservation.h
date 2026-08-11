// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_DOMAIN_CROWDOBSERVATION_H
#define TRADINGAPP_DOMAIN_CROWDOBSERVATION_H

#include <QDateTime>
#include <QString>

// The normalized, immutable unit the Crowd Sentiment & AI subsystem is built on (REQ-F-039,
// Phase 1). Every provider — retail positioning, options, institutional positioning, social,
// macro, market — turns its raw payload into a stream of these, and every feature and score is
// computed FROM them, never the other way round. Pure Qt Core, so it lives in the domain layer
// and is testable without a network, a database or a GUI.
namespace trading::crowd {

// Which family an observation belongs to — the categories the eventual Crowd Score combines.
// Append-only (a new provider extends it) so stored ordinals never shift. Phase 1 only wires
// the free, licence-safe sources (CFTC COT, FRED/VIX); the rest exist so the schema and the
// mock provider already cover them.
enum class Source {
    RetailPositioning,        // IG client sentiment, eToro %-long — read CONTRARIAN
    Options,                  // put/call ratios, VIX term structure
    Volatility,               // VIX level / change (FRED: VIXCLS)
    InstitutionalPositioning, // CFTC COT asset-manager / leveraged-fund, NAAIM
    Social,                   // Stocktwits / Reddit / news sentiment
    Macro,                    // FRED macro series (yields, USD, releases)
    Market,                   // price / volume (SPY/QQQ, later NQ/ES)
};

// Freshness of an observation relative to a reference time. Absent is deliberately DISTINCT
// from Stale: "not fetched" and "too old" are different facts, and only one of them is even a
// datum — the absent-is-not-zero rule the whole app already follows.
enum class Freshness { Live, Stale, Absent };

// Data-quality flags, a bitfield so one observation can carry several. Ok means it passed every
// check; anything else is CARRIED and surfaced, never silently dropped or coerced to a value.
enum class Quality : quint32 {
    Ok = 0,
    Estimated = 1U << 0U,     // provider marked it provisional / subject to revision
    Interpolated = 1U << 1U,  // filled from surrounding points, not directly observed
    OutOfRange = 1U << 2U,    // failed a documented sanity bound
    LateArrival = 1U << 3U,   // received well after its event time
    Duplicate = 1U << 4U,     // a same-key observation already existed in the store
};

// One NORMALIZED, IMMUTABLE observation. ALL timestamps are UTC.
//
// The TWO times are both kept and are load-bearing for leakage prevention: `eventTime` is what
// the datum is ABOUT (a COT report is about the Tuesday close), `receivedTime` is when it became
// KNOWN (that report publishes the following Friday). A model that treats eventTime as if it
// were known then has looked into the future; every downstream consumer must reason in
// receivedTime. Storing both is what makes that checkable rather than assumed.
struct Observation {
    QString instrument;        // "SPX500" / "NSDQ100"; empty = market-wide
    Source source = Source::Market;
    QString sourceName;        // the CONCRETE provider: "CFTC-COT" / "FRED" / "mock"
    QString seriesId;          // source-specific series/field id ("VIXCLS", "ES-ASSET-MGR-NET")
    QDateTime eventTime;       // UTC: what the datum is ABOUT
    QDateTime receivedTime;    // UTC: when it became KNOWN (publication / fetch)
    double value = 0.0;
    QString unit;              // "index" / "percent" / "contracts" / "ratio"
    bool valid = false;        // false = missing/unusable — NEVER read as 0.0
    quint32 quality = static_cast<quint32>(Quality::Ok);
    qint32 schemaVersion = 1;

    // A stable identity for de-duplication and the store's unique key: the same source, series,
    // instrument and eventTime is the SAME datum however many times it is fetched.
    [[nodiscard]] QString dedupKey() const;
    // Seconds between `receivedTime` and `now` (both UTC). Negative when receivedTime is in the
    // future (clock skew) — reported honestly rather than clamped, so the caller can decide.
    [[nodiscard]] qint64 ageSeconds(const QDateTime &now) const;
    // Live within `staleAfterSec`, Stale beyond it, Absent when the observation is not valid or
    // carries no receivedTime.
    [[nodiscard]] Freshness freshness(const QDateTime &now, qint64 staleAfterSec) const;
    [[nodiscard]] bool hasQuality(Quality flag) const;
    void setQuality(Quality flag);
};

// Stable enum <-> string mapping, for storage and display (the store persists the NAME, not the
// ordinal, so re-ordering the enum can never silently repoint historical rows).
[[nodiscard]] QString sourceToString(Source source);
[[nodiscard]] Source sourceFromString(const QString &name);

// Human freshness word — never colour alone, matching the rest of the app.
[[nodiscard]] QString freshnessWord(Freshness freshness);

} // namespace trading::crowd

#endif // TRADINGAPP_DOMAIN_CROWDOBSERVATION_H
