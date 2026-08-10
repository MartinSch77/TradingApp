// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_IGSENTIMENTPROVIDER_H
#define TRADINGAPP_SERVICES_IGSENTIMENTPROVIDER_H

#include "services/CrowdHttpProvider.h"

// The IG Client Sentiment provider (REQ-F-039, Phase 3): reads the percentage of IG clients
// positioned LONG in an index market from IG's official REST API — the retail-positioning family
// the Crowd Score reads CONTRARIAN. Strictly OPTIONAL: it needs an IG account, and all three
// credentials (API key, identifier, password) must be present before it makes a single call;
// anything missing means isConfigured() is false and refresh() returns silently, exactly like
// FRED without a key. No credential is ever committed, logged, or interpolated into an error
// string — they live in the git-ignored apiKeyEtoro.json or the TRADINGAPP_IG_* environment
// variables (Config carries them; docs/crowd-ai.md documents the opt-in setup).
//
// IG authenticates per SESSION: POST /session (VERSION 2) answers with the CST and
// X-SECURITY-TOKEN response HEADERS, which every later call carries. The tokens are valid for
// ~6 hours (extended in use), so the provider logs in once and reuses them until they age out;
// the login POST is never auto-retried (it is not idempotent), while the sentiment GET keeps the
// shared base's retry. Licence: the official documented API on the user's own account — no
// scraping, and the data is not redistributed.
namespace trading::crowd {

class IgSentimentProvider : public CrowdHttpProvider
{
    Q_OBJECT

public:
    explicit IgSentimentProvider(QObject *parent = nullptr);

    [[nodiscard]] QString name() const override;
    [[nodiscard]] Source category() const override;
    [[nodiscard]] bool isConfigured() const override;   // all three credentials present
    void refresh(const QString &instrument, const QDateTime &now) override;

    // Set the credentials explicitly (production wires them from Config; the tests use dummies).
    // Kept out of any log, fixture or diagnostic.
    void setCredentials(const QString &apiKey, const QString &identifier,
                        const QString &password);
    void setDemoAccount(bool demo);   // demo-api.ig.com instead of api.ig.com

private:
    void fetchSentiment(const QString &instrument, const QString &marketId,
                        const QDateTime &now);

    QString m_apiKey;
    QString m_identifier;
    QString m_password;
    bool m_demo = false;
    QByteArray m_cst;              // session tokens from the last login, reused while fresh
    QByteArray m_securityToken;
    QDateTime m_loginTime;         // UTC; empty = no session yet
};

} // namespace trading::crowd

#endif // TRADINGAPP_SERVICES_IGSENTIMENTPROVIDER_H
