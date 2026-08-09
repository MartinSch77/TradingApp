// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_CFTCCOTPROVIDER_H
#define TRADINGAPP_SERVICES_CFTCCOTPROVIDER_H

#include "services/CrowdHttpProvider.h"

// The CFTC Commitments of Traders provider (REQ-F-039, Phase 3): the FIRST real data source,
// chosen because it is free, needs no key and has decades of public history. It reads the
// "Traders in Financial Futures" report from the CFTC's official public Socrata API and turns
// the latest release for an index's E-mini future into institutional-positioning observations
// (asset-manager and leveraged-fund NET contracts).
//
// The publication LAG is honoured, because it is the whole leakage point: the report is ABOUT a
// Tuesday but is only RELEASED the following Friday, so eventTime is the Tuesday and receivedTime
// is that Friday — never treated as known before it was.
//
// Licence: CFTC data is a US-government public-domain work; there is no key and no redistribution
// restriction. No scraping — this is the documented JSON API.
namespace trading::crowd {

class CftcCotProvider : public CrowdHttpProvider
{
    Q_OBJECT

public:
    explicit CftcCotProvider(QObject *parent = nullptr);

    [[nodiscard]] QString name() const override;
    [[nodiscard]] Source category() const override;
    [[nodiscard]] bool isConfigured() const override;   // always: public, no credential
    void refresh(const QString &instrument, const QDateTime &now) override;
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_CFTCCOTPROVIDER_H
