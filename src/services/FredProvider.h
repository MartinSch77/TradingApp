// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_FREDPROVIDER_H
#define TRADINGAPP_SERVICES_FREDPROVIDER_H

#include "services/CrowdHttpProvider.h"

// The FRED (Federal Reserve Economic Data) provider (REQ-F-039, Phase 3): reads the CBOE
// volatility index close (series VIXCLS) from the St. Louis Fed's official JSON API and turns the
// latest value into a volatility observation.
//
// FRED requires a FREE API key, read from the TRADINGAPP_FRED_API_KEY environment variable (never
// from source, and never logged — it is only ever a query parameter on the request URL). Without
// the key the provider reports itself UNAVAILABLE and the application carries on, exactly as the
// mock's disabled path does. Licence: FRED data is redistributed under its published terms; the
// KEY is personal and is never committed or written to a fixture.
namespace trading::crowd {

class FredProvider : public CrowdHttpProvider
{
    Q_OBJECT

public:
    explicit FredProvider(QObject *parent = nullptr);

    [[nodiscard]] QString name() const override;
    [[nodiscard]] Source category() const override;
    [[nodiscard]] bool isConfigured() const override;   // a key is present
    void refresh(const QString &instrument, const QDateTime &now) override;

    // Set the key explicitly (the tests use a dummy; production reads the environment). Kept out
    // of any log or fixture.
    void setApiKey(const QString &key);

private:
    QString m_apiKey;
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_FREDPROVIDER_H
