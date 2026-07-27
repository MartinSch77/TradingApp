#ifndef TRADINGAPP_CONFIG_H
#define TRADINGAPP_CONFIG_H

#include <QString>

// Runtime configuration for the app.
//
// Resolution order (later overrides earlier):
//   1. Built-in defaults (demo mode, SPX500, no credentials).
//   2. config.json — the non-secret settings, safe to commit (see Config::load
//      for the search path).
//   3. apiKeyEtoro.json — the API keys only, looked up beside config.json and
//      kept OUT of version control (.gitignore).
//   4. Environment variables (ETORO_API_KEY, ETORO_USER_KEY, ETORO_MODE, ...).
//
// If no API credentials are found the app runs in a clearly-labelled
// SIMULATION mode with a synthetic price feed, so it is always runnable.
struct Config {
    QString apiKey;                                       // x-api-key header
    QString userKey;                                      // x-user-key header
    QString username;                                     // eToro username for the title bar
    QString mode = QStringLiteral("demo");                // "demo" or "real"
    QString symbol = QStringLiteral("SPX500");            // instrument to trade
    QString baseUrl = QStringLiteral("https://public-api.etoro.com/api");
    QString orderCurrency = QStringLiteral("usd");
    double defaultLeverage = 1.0;
    qint32 pollIntervalMs = 5000;                         // price/portfolio poll cadence
    QString anthropicApiKey;                              // optional: enables the Claude AI
                                                          // source in the decision window

    [[nodiscard]] bool hasCredentials() const;  // out-of-line: keeps coverage records unambiguous

    // True only when we have credentials AND the user explicitly asked for real
    // money. Everything else (demo, or no credentials) never touches real funds.
    [[nodiscard]] bool isLive() const;

    // Human-readable description of the active mode, for the UI badge.
    [[nodiscard]] QString modeLabel() const;

    static Config load();
};

#endif // TRADINGAPP_CONFIG_H
