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
                              "TRADINGAPP_BOT_LOSS_LIMIT"};

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
