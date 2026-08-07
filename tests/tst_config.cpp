// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

// Integration tests for the layered configuration (DES-SVC-CFG): built-in
// defaults ← config.json ← apiKeyEtoro.json ← environment variables.

#include "services/Config.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <array>

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(content), static_cast<qint64>(content.size()));
}

// The env vars Config::load consults; cleared around every test so the host
// environment (and test order) cannot leak into the results.
constexpr std::array kEnvVars{"ETORO_CONFIG",     "ETORO_API_KEY",
                              "ETORO_USER_KEY",   "ETORO_USERNAME",
                              "ETORO_MODE",       "ETORO_SYMBOL",
                              "ETORO_BASE_URL",   "ETORO_ORDER_CURRENCY",
                              "ETORO_POLL_MS",    "ETORO_LEVERAGE",
                              "ANTHROPIC_API_KEY", "TRADINGAPP_BOT_TARGET",
                              "TRADINGAPP_BOT_LOSS_LIMIT",
                              "TRADINGAPP_FORCE_SIMULATION"};

} // namespace

class TestConfig : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private:
    QString m_originalCwd;

private slots:
    void init()
    {
        for (const char *v : kEnvVars) {
            qunsetenv(v);
        }
        m_originalCwd = QDir::currentPath();
    }

    void cleanup()
    {
        static_cast<void>(QDir::setCurrent(m_originalCwd));
        for (const char *v : kEnvVars) {
            qunsetenv(v);
        }
    }

    //! @tstid TS-CFG-001 @design DES-SVC-CFG
    // @relation(REQ-F-017, REQ-F-018, scope=function)
    void TS_CFG_001_defaultsWithoutFiles()
    {
        const QTemporaryDir dir;  // empty: no config.json, no apiKeyEtoro.json
        QVERIFY(QDir::setCurrent(dir.path()));
        const Config cfg = Config::load();
        QCOMPARE(cfg.mode, QStringLiteral("demo"));
        QCOMPARE(cfg.symbol, QStringLiteral("SPX500"));
        QVERIFY(!cfg.hasCredentials());
        QVERIFY(!cfg.isLive());
        QVERIFY(cfg.modeLabel().contains(QStringLiteral("SIMULATION")));
    }

    //! @tstid TS-CFG-002 @design DES-SVC-CFG
    // @relation(REQ-F-018, REQ-N-004, scope=function)
    void TS_CFG_002_secretsFileLayersOverConfig()
    {
        const QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("config.json")),
                  R"({"mode":"real","symbol":"NSDQ100","defaultLeverage":10})");
        writeFile(dir.filePath(QStringLiteral("apiKeyEtoro.json")),
                  R"({"apiKey":"app-key-123","userKey":"user-key-456"})");
        QVERIFY(QDir::setCurrent(dir.path()));
        const Config cfg = Config::load();
        // Non-secret settings from config.json…
        QCOMPARE(cfg.mode, QStringLiteral("real"));
        QCOMPARE(cfg.symbol, QStringLiteral("NSDQ100"));
        QCOMPARE(cfg.defaultLeverage, 10.0);
        // …credentials only from the secrets file.
        QCOMPARE(cfg.apiKey, QStringLiteral("app-key-123"));
        QCOMPARE(cfg.userKey, QStringLiteral("user-key-456"));
        QVERIFY(cfg.isLive());
    }

    //! @tstid TS-CFG-003 @design DES-SVC-CFG
    // @relation(REQ-F-018, scope=function)
    void TS_CFG_003_envOverridesFiles()
    {
        const QTemporaryDir dir;
        writeFile(dir.filePath(QStringLiteral("config.json")),
                  R"({"mode":"real","symbol":"NSDQ100"})");
        writeFile(dir.filePath(QStringLiteral("apiKeyEtoro.json")),
                  R"({"apiKey":"file-key","userKey":"file-user"})");
        QVERIFY(QDir::setCurrent(dir.path()));
        qputenv("ETORO_MODE", "demo");
        qputenv("ETORO_API_KEY", "env-key");
        const Config cfg = Config::load();
        QCOMPARE(cfg.mode, QStringLiteral("demo"));           // env beats file
        QCOMPARE(cfg.apiKey, QStringLiteral("env-key"));      // env beats secrets file
        QCOMPARE(cfg.userKey, QStringLiteral("file-user"));   // untouched key stays
    }

    //! @tstid TS-CFG-005 @design DES-SVC-CFG
    // @relation(REQ-F-031, scope=function)
    void TS_CFG_005_dailyRulesAreConfigurable()
    {
        const QTemporaryDir dir;
        QVERIFY(QDir::setCurrent(dir.path()));
        const Config defaults = Config::load();
        QCOMPARE(defaults.botDailyTarget, 350.0);        // the documented default
        QCOMPARE(defaults.botDailyLossLimit, 350.0);

        writeFile(dir.filePath(QStringLiteral("config.json")),
                  R"({"botDailyTarget":500.0,"botDailyLossLimit":250.0})");
        const Config fromFile = Config::load();
        QCOMPARE(fromFile.botDailyTarget, 500.0);
        QCOMPARE(fromFile.botDailyLossLimit, 250.0);

        // 0 is a real value here: it switches the rule off. And a negative number is
        // rejected rather than applied — a typo must never widen what may be lost.
        qputenv("TRADINGAPP_BOT_TARGET", "0");
        qputenv("TRADINGAPP_BOT_LOSS_LIMIT", "-100");
        const Config fromEnv = Config::load();
        QCOMPARE(fromEnv.botDailyTarget, 0.0);
        QCOMPARE(fromEnv.botDailyLossLimit, 250.0);      // the file value stands
    }

    //! @tstid TS-CFG-006 @design DES-SVC-CFG
    // @relation(REQ-F-018, scope=function)
    void TS_CFG_006_numbersAndAnExplicitConfigPathAreHonouredOrRefused()
    {
        const QTemporaryDir dir;
        QVERIFY(QDir::setCurrent(dir.path()));
        writeFile(dir.filePath(QStringLiteral("config.json")),
                  R"({"defaultLeverage":7,"pollIntervalMs":2500})");
        const Config fromFile = Config::load();
        QCOMPARE(fromFile.defaultLeverage, 7.0);
        QCOMPARE(fromFile.pollIntervalMs, 2500);

        // Env wins — but only with a sane value: a poll faster than 500 ms or a
        // leverage below 1 is refused rather than applied, because both would
        // quietly change how the app trades.
        qputenv("ETORO_POLL_MS", "1500");
        qputenv("ETORO_LEVERAGE", "3.5");
        const Config good = Config::load();
        QCOMPARE(good.pollIntervalMs, 1500);
        QCOMPARE(good.defaultLeverage, 3.5);
        qputenv("ETORO_POLL_MS", "10");            // would hammer the API
        qputenv("ETORO_LEVERAGE", "0.2");          // not a leverage at all
        const Config refused = Config::load();
        QCOMPARE(refused.pollIntervalMs, 2500);    // the file value stands
        QCOMPARE(refused.defaultLeverage, 7.0);
        qputenv("ETORO_POLL_MS", "not a number");
        qputenv("ETORO_LEVERAGE", "neither");
        const Config junk = Config::load();
        QCOMPARE(junk.pollIntervalMs, 2500);
        QCOMPARE(junk.defaultLeverage, 7.0);
        qunsetenv("ETORO_POLL_MS");
        qunsetenv("ETORO_LEVERAGE");

        // $ETORO_CONFIG names the config file itself, and the secrets file is
        // looked up BESIDE it — so a config kept outside the working directory
        // still finds its own key file.
        const QTemporaryDir elsewhere;
        writeFile(elsewhere.filePath(QStringLiteral("other.json")),
                  R"({"mode":"real","symbol":"GER40"})");
        writeFile(elsewhere.filePath(QStringLiteral("apiKeyEtoro.json")),
                  R"({"apiKey":"beside-key","userKey":"beside-user"})");
        qputenv("ETORO_CONFIG", elsewhere.filePath(QStringLiteral("other.json")).toUtf8());
        const Config pointed = Config::load();
        QCOMPARE(pointed.symbol, QStringLiteral("GER40"));
        QCOMPARE(pointed.apiKey, QStringLiteral("beside-key"));
        QCOMPARE(pointed.userKey, QStringLiteral("beside-user"));

        // An unreadable or malformed file is skipped, not fatal: the app must still
        // start on its defaults rather than die on a stray character.
        writeFile(elsewhere.filePath(QStringLiteral("other.json")), "{ this is not json");
        const Config broken = Config::load();
        QVERIFY(!broken.symbol.isEmpty());   // the built-in default survived
    }

    //! @tstid TS-CFG-007 @design DES-UI-GUITEST
    // @relation(REQ-N-005, REQ-N-007, REQ-F-017, scope=function)
    void TS_CFG_007_forcedSimulationCannotBeTalkedOutOfIt()
    {
        // What makes a GUI test suite safe on a machine that HAS real keys: the
        // guarantee lives in the app, not in the test scripts. With the switch on, the
        // app has no credentials — so no live mode, no broker network, no order path —
        // however the keys and the mode are configured.
        const QTemporaryDir dir;
        QVERIFY(QDir::setCurrent(dir.path()));
        writeFile(dir.filePath(QStringLiteral("config.json")), R"({"mode":"real"})");
        writeFile(dir.filePath(QStringLiteral("apiKeyEtoro.json")),
                  R"({"apiKey":"real-key","userKey":"real-user"})");

        // Without it: exactly the dangerous configuration.
        const Config live = Config::load();
        QVERIFY(live.hasCredentials());
        QVERIFY(live.isLive());
        QVERIFY(live.modeLabel().contains(QStringLiteral("REAL MONEY")));

        // With it: the same files, and no way to reach the account.
        qputenv("TRADINGAPP_FORCE_SIMULATION", "1");
        const Config forced = Config::load();
        QVERIFY(forced.forceSimulation);
        QVERIFY(!forced.hasCredentials());   // the ONE place every mode question reads
        QVERIFY(!forced.isLive());
        QVERIFY(forced.modeLabel().contains(QStringLiteral("SIMULATION")));
        // …and it says WHY, so a screenshot from a test run is not mistaken for the
        // app failing to find keys that are in fact present.
        QVERIFY(forced.modeLabel().contains(QStringLiteral("FORCE_SIMULATION")));
        // The keys are still READ (nothing hides them) — they are simply unusable.
        QCOMPARE(forced.apiKey, QStringLiteral("real-key"));

        // "0", "false" and an empty value all mean "not forced": a switch that could
        // be turned on by accident would be as bad as one that cannot be turned on.
        for (const char *off : {"0", "false", "FALSE", ""}) {
            qputenv("TRADINGAPP_FORCE_SIMULATION", off);
            const Config notForced = Config::load();
            QVERIFY2(!notForced.forceSimulation, off);
            QVERIFY2(notForced.isLive(), off);
        }
        // …and anything else means on.
        for (const char *on : {"1", "yes", "true", "please"}) {
            qputenv("TRADINGAPP_FORCE_SIMULATION", on);
            QVERIFY2(!Config::load().isLive(), on);
        }
    }

    //! @tstid TS-CFG-004 @design DES-SVC-CFG
    // @relation(REQ-F-017, scope=function)
    void TS_CFG_004_liveRequiresCredentialsAndRealMode()
    {
        const QTemporaryDir dir;
        QVERIFY(QDir::setCurrent(dir.path()));
        qputenv("ETORO_API_KEY", "k");
        qputenv("ETORO_USER_KEY", "u");
        qputenv("ETORO_MODE", "demo");
        Config cfg = Config::load();
        QVERIFY(cfg.hasCredentials());
        QVERIFY(!cfg.isLive());
        QVERIFY(cfg.modeLabel().contains(QStringLiteral("DEMO")));

        qputenv("ETORO_MODE", "real");
        cfg = Config::load();
        QVERIFY(cfg.isLive());
        QVERIFY(cfg.modeLabel().contains(QStringLiteral("REAL")));
    }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "tst_config.moc"
