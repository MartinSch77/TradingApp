// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_MOCKCROWDPROVIDER_H
#define TRADINGAPP_SERVICES_MOCKCROWDPROVIDER_H

#include "services/CrowdProvider.h"

// A deterministic, offline provider (REQ-F-039, Phase 1). It is what runs everywhere a real API
// is not available: the unit suite, and the application without any paid subscription. Given the
// same instrument and the same UTC DAY it returns the SAME observations (a seeded generator) —
// reproducibility the app already requires elsewhere. It spans every Source family so the store,
// the freshness metadata and (Phase 2) the Crowd Score have a complete shape to exercise, and it
// models the CFTC publication LAG (the positioning datum is ABOUT the most recent Tuesday but is
// KNOWN only the following Friday), so leakage-aware timing is present from the first phase.
//
// No network, no credentials, no randomness a test cannot reproduce.
namespace trading::crowd {

class MockCrowdProvider : public ICrowdProvider
{
public:
    // `configured` false makes it report itself UNAVAILABLE — the recoverable no-credentials
    // state a real provider has, so that path is testable without a real provider.
    explicit MockCrowdProvider(bool configured = true);

    [[nodiscard]] QString name() const override;
    [[nodiscard]] Source category() const override;   // Market: it deliberately spans families
    [[nodiscard]] bool isConfigured() const override;
    [[nodiscard]] ProviderResult fetch(const QString &instrument, const QDateTime &now) override;

private:
    bool m_configured;
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_MOCKCROWDPROVIDER_H
