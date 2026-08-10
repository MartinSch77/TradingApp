// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/Config.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace {

// Copy a string value out of a JSON object (only if the key is present).
void applyString(const QJsonObject &obj, const QString &key, QString &out)
{
    if (obj.contains(key)) {
        out = obj.value(key).toString();
    }
}

// Apply a JSON object onto a Config (only keys that are present).
void applyJson(Config &cfg, const QJsonObject &obj)
{
    applyString(obj, QStringLiteral("apiKey"), cfg.apiKey);
    applyString(obj, QStringLiteral("userKey"), cfg.userKey);
    applyString(obj, QStringLiteral("username"), cfg.username);
    applyString(obj, QStringLiteral("mode"), cfg.mode);
    applyString(obj, QStringLiteral("symbol"), cfg.symbol);
    applyString(obj, QStringLiteral("baseUrl"), cfg.baseUrl);
    applyString(obj, QStringLiteral("orderCurrency"), cfg.orderCurrency);
    applyString(obj, QStringLiteral("anthropicApiKey"), cfg.anthropicApiKey);
    applyString(obj, QStringLiteral("ollamaHost"), cfg.ollamaHost);
    applyString(obj, QStringLiteral("ollamaModel"), cfg.ollamaModel);
    applyString(obj, QStringLiteral("igApiKey"), cfg.igApiKey);
    applyString(obj, QStringLiteral("igIdentifier"), cfg.igIdentifier);
    applyString(obj, QStringLiteral("igPassword"), cfg.igPassword);
    if (obj.contains(QStringLiteral("igDemo"))) {
        cfg.igDemo = obj.value(QStringLiteral("igDemo")).toBool(cfg.igDemo);
    }
    if (obj.contains(QStringLiteral("defaultLeverage"))) {
        cfg.defaultLeverage = obj.value(QStringLiteral("defaultLeverage")).toDouble(cfg.defaultLeverage);
    }
    if (obj.contains(QStringLiteral("pollIntervalMs"))) {
        cfg.pollIntervalMs = obj.value(QStringLiteral("pollIntervalMs")).toInt(cfg.pollIntervalMs);
    }
    if (obj.contains(QStringLiteral("botDailyTarget"))) {
        cfg.botDailyTarget = obj.value(QStringLiteral("botDailyTarget")).toDouble(cfg.botDailyTarget);
    }
    if (obj.contains(QStringLiteral("botDailyLossLimit"))) {
        cfg.botDailyLossLimit =
            obj.value(QStringLiteral("botDailyLossLimit")).toDouble(cfg.botDailyLossLimit);
    }
}

void applyNonNegative(const QProcessEnvironment &env, const QString &key, double &target)
{
    if (!env.contains(key)) {
        return;
    }
    bool ok = false;
    const double v = env.value(key).toDouble(&ok);
    if (ok && (v >= 0.0)) {
        target = v;
    }
}

// Apply the first existing, parseable candidate file onto cfg (only keys present
// in the file are applied, so several files can layer).
void loadJsonFile(Config &cfg, const QStringList &candidates)
{
    for (const QString &path : candidates) {
        QFile f(path);
        if (!f.exists()) {
            continue;
        }
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray raw = f.readAll();
        QJsonParseError err {};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        const bool docIsObject = doc.isObject();
        if ((err.error == QJsonParseError::NoError) && docIsObject) {
            applyJson(cfg, doc.object());
            return;
        }
    }
}

// Candidate paths for a config file name: next to $ETORO_CONFIG when that is
// set, then the working directory, then the per-user app-config directory.
QStringList configCandidates(const QString &fileName)
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList candidates;
    if (env.contains(QStringLiteral("ETORO_CONFIG"))) {
        const QString explicitPath = env.value(QStringLiteral("ETORO_CONFIG"));
        // $ETORO_CONFIG names the main config file itself; companion files
        // (the API-key file) are looked up beside it.
        candidates << ((fileName == QStringLiteral("config.json"))
                           ? explicitPath
                           : QFileInfo(explicitPath).dir().filePath(fileName));
    }
    candidates << QDir::current().filePath(fileName);
    const QString appConfig =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!appConfig.isEmpty()) {
        candidates << QDir(appConfig).filePath(fileName);
    }
    return candidates;
}

void applyEnv(Config &cfg)
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    auto take = [&env](const char *key, QString &out) {
        if (env.contains(QString::fromLatin1(key))) {
            out = env.value(QString::fromLatin1(key));
        }
    };
    take("ETORO_API_KEY", cfg.apiKey);
    take("ETORO_USER_KEY", cfg.userKey);
    take("ETORO_USERNAME", cfg.username);
    take("ETORO_MODE", cfg.mode);
    take("ETORO_SYMBOL", cfg.symbol);
    take("ETORO_BASE_URL", cfg.baseUrl);
    take("ETORO_ORDER_CURRENCY", cfg.orderCurrency);
    take("ANTHROPIC_API_KEY", cfg.anthropicApiKey);
    take("OLLAMA_HOST", cfg.ollamaHost);
    take("OLLAMA_MODEL", cfg.ollamaModel);
    take("TRADINGAPP_IG_API_KEY", cfg.igApiKey);
    take("TRADINGAPP_IG_IDENTIFIER", cfg.igIdentifier);
    take("TRADINGAPP_IG_PASSWORD", cfg.igPassword);
    if (env.contains(QStringLiteral("TRADINGAPP_IG_DEMO"))) {
        const QString raw = env.value(QStringLiteral("TRADINGAPP_IG_DEMO")).trimmed();
        cfg.igDemo = !raw.isEmpty() && (raw != QStringLiteral("0"))
                     && (raw.compare(QStringLiteral("false"), Qt::CaseInsensitive) != 0);
    }

    if (env.contains(QStringLiteral("ETORO_POLL_MS"))) {
        bool ok = false;
        const qint32 v = env.value(QStringLiteral("ETORO_POLL_MS")).toInt(&ok);
        if (ok && (v >= 500)) {
            cfg.pollIntervalMs = v;
        }
    }
    if (env.contains(QStringLiteral("ETORO_LEVERAGE"))) {
        bool ok = false;
        const double v = env.value(QStringLiteral("ETORO_LEVERAGE")).toDouble(&ok);
        if (ok && (v >= 1.0)) {
            cfg.defaultLeverage = v;
        }
    }
    // 0 is meaningful here (it switches the rule off), so only a NEGATIVE value is
    // rejected — a typo must never quietly widen what the bot may lose.
    // A one-way switch: present at all (any value except "0"/"false") means the app
    // runs in simulation whatever the keys say. There is no env variable that turns
    // it back off, and nothing in the app sets it — only a caller can.
    if (env.contains(QStringLiteral("TRADINGAPP_FORCE_SIMULATION"))) {
        const QString raw = env.value(QStringLiteral("TRADINGAPP_FORCE_SIMULATION")).trimmed();
        cfg.forceSimulation = !raw.isEmpty() && (raw != QStringLiteral("0"))
                              && (raw.compare(QStringLiteral("false"), Qt::CaseInsensitive) != 0);
    }
    applyNonNegative(env, QStringLiteral("TRADINGAPP_BOT_TARGET"), cfg.botDailyTarget);
    applyNonNegative(env, QStringLiteral("TRADINGAPP_BOT_LOSS_LIMIT"), cfg.botDailyLossLimit);
}

} // namespace

bool Config::hasCredentials() const
{
    // The forced-simulation switch answers here, at the ONE place every mode
    // question ultimately reads: with no credentials the app is in SIMULATION by
    // construction — synthetic feed, no network to the broker, no order path at all
    // — so a GUI test suite cannot reach a real account however it is configured
    // (REQ-N-005). Deliberately not a UI-level check and not a convention in a test
    // script: a switch that only the scripts respect is a switch that stops working
    // the first time someone runs the app by hand.
    if (forceSimulation) {
        return false;
    }
    return !apiKey.isEmpty() && !userKey.isEmpty();
}

bool Config::isLive() const
{
    const bool wantsReal = mode.compare(QStringLiteral("real"), Qt::CaseInsensitive) == 0;
    return hasCredentials() && wantsReal;
}

QString Config::modeLabel() const
{
    if (forceSimulation) {
        // Says WHY, so a screenshot from a test run cannot be mistaken for the app
        // failing to find credentials that are in fact present.
        return QStringLiteral("SIMULATION — forced by TRADINGAPP_FORCE_SIMULATION "
                              "(credentials ignored)");
    }
    if (!hasCredentials()) {
        return QStringLiteral("SIMULATION — no API keys (synthetic price feed)");
    }
    if (isLive()) {
        return QStringLiteral("LIVE — REAL MONEY (eToro real account)");
    }
    return QStringLiteral("DEMO — eToro virtual account");
}

Config Config::load()
{
    Config cfg;
    // Layered: config.json holds the non-secret settings and is safe to commit;
    // apiKeyEtoro.json holds only the API keys and stays out of version control
    // (.gitignore). Both apply only the keys they contain, then env overrides.
    loadJsonFile(cfg, configCandidates(QStringLiteral("config.json")));
    loadJsonFile(cfg, configCandidates(QStringLiteral("apiKeyEtoro.json")));
    applyEnv(cfg);
    return cfg;
}
