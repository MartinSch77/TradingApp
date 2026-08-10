// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_CROWDPROVIDER_H
#define TRADINGAPP_SERVICES_CROWDPROVIDER_H

#include "domain/CrowdObservation.h"

#include <QDateTime>
#include <QList>
#include <QString>

// The provider SEAM for the Crowd Sentiment subsystem (REQ-F-039, Phase 1). One interface plus
// a `category()`, mirroring the existing OrderGateway seam rather than a fan of near-identical
// per-source interfaces — the family is data, not type. A mock and a recorded-fixture provider
// implement the same shape so the whole subsystem runs and is tested WITHOUT any paid API, and
// a missing credential is a recoverable "unavailable", never a crash.
namespace trading::crowd {

// The outcome of a fetch. `available` false means the provider is not configured (no key); it
// is a normal, recoverable state that the store and score treat as absent data, not an error.
struct ProviderResult {
    QList<Observation> observations;
    bool available = true;
    QString note;   // human reason when unavailable or degraded
};

// One data provider. In production the real implementations fetch ASYNCHRONOUSLY over Qt Network
// off the GUI thread; the interface itself is a plain pull returning what is known AS OF `now`
// (UTC), so a mock and a fixture provider are trivially driven in a unit test.
class ICrowdProvider
{
public:
    ICrowdProvider() = default;
    virtual ~ICrowdProvider() = default;
    ICrowdProvider(const ICrowdProvider &) = delete;
    ICrowdProvider &operator=(const ICrowdProvider &) = delete;
    ICrowdProvider(ICrowdProvider &&) = delete;
    ICrowdProvider &operator=(ICrowdProvider &&) = delete;

    // The concrete provider name that ends up in Observation::sourceName ("CFTC-COT", "FRED",
    // "mock"), the family it fills, and whether it has what it needs to run at all.
    [[nodiscard]] virtual QString name() const = 0;
    [[nodiscard]] virtual Source category() const = 0;
    [[nodiscard]] virtual bool isConfigured() const = 0;

    // The observations this provider can supply for `instrument` (empty = market-wide) as known
    // at `now` (UTC). Every observation it returns carries its own eventTime/receivedTime, so a
    // publication-lagged source (COT) reports the truth about WHEN it became known.
    [[nodiscard]] virtual ProviderResult fetch(const QString &instrument,
                                               const QDateTime &now) = 0;
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_CROWDPROVIDER_H
