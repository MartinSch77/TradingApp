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
    // Optional LOCAL model via Ollama (REQ-F-030): the trading-proposal source the
    // bot simulation can run on. No key — it is a service on your own machine, so
    // only the host and the model name are configuration. An empty model means
    // "not configured": the feature reports itself unavailable and nothing else
    // changes.
    QString ollamaHost = QStringLiteral("http://localhost:11434");
    QString ollamaModel;                                  // e.g. "llama3.2" or "qwen2.5:7b"

    // The bot's daily stopping rules (REQ-F-031), in EUR of BOOKED net. The target
    // is what the day aims at, the limit what it refuses to lose; reaching either
    // stops opening for that day. Configurable because "how much per day" is a
    // capital decision, not a code decision — 0 disables the rule it belongs to.
    double botDailyTarget = 350.0;
    double botDailyLossLimit = 350.0;

    // Forced SIMULATION, set by TRADINGAPP_FORCE_SIMULATION and by nothing else.
    // While it is on, hasCredentials() answers false — so isLive() is false, the
    // broker client uses its synthetic feed, and no order path exists to be reached.
    // It is what makes a GUI test suite (Squish) safe to point at this app on a
    // machine that HAS real keys: the guarantee is in the app, not in the scripts.
    bool forceSimulation = false;

    [[nodiscard]] bool hasCredentials() const;  // out-of-line: keeps coverage records unambiguous

    // True only when we have credentials AND the user explicitly asked for real
    // money. Everything else (demo, or no credentials) never touches real funds.
    [[nodiscard]] bool isLive() const;

    // Human-readable description of the active mode, for the UI badge.
    [[nodiscard]] QString modeLabel() const;

    static Config load();
};

#endif // TRADINGAPP_CONFIG_H
