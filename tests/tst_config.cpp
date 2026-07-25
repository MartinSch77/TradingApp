// Integration tests for the layered configuration (DES-SVC-CFG): built-in
// defaults ← config.json ← apiKeyEtoro.json ← environment variables.

#include "services/Config.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(content), static_cast<qint64>(content.size()));
}

// The env vars Config::load consults; cleared around every test so the host
// environment (and test order) cannot leak into the results.
const char *kEnvVars[] = {"ETORO_CONFIG", "ETORO_API_KEY", "ETORO_USER_KEY",
                          "ETORO_USERNAME", "ETORO_MODE", "ETORO_SYMBOL",
                          "ETORO_BASE_URL", "ETORO_ORDER_CURRENCY",
                          "ETORO_POLL_MS", "ETORO_LEVERAGE", "ANTHROPIC_API_KEY"};

} // namespace

class TestConfig : public QObject
{
    Q_OBJECT
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

    //! @tstid TS-CFG-001 @verifies REQ-F-017 REQ-F-018 @design DES-SVC-CFG
    void TS_CFG_001_defaultsWithoutFiles()
    {
        QTemporaryDir dir;  // empty: no config.json, no apiKeyEtoro.json
        QVERIFY(QDir::setCurrent(dir.path()));
        const Config cfg = Config::load();
        QCOMPARE(cfg.mode, QStringLiteral("demo"));
        QCOMPARE(cfg.symbol, QStringLiteral("SPX500"));
        QVERIFY(!cfg.hasCredentials());
        QVERIFY(!cfg.isLive());
        QVERIFY(cfg.modeLabel().contains(QStringLiteral("SIMULATION")));
    }

    //! @tstid TS-CFG-002 @verifies REQ-F-018 REQ-N-004 @design DES-SVC-CFG
    void TS_CFG_002_secretsFileLayersOverConfig()
    {
        QTemporaryDir dir;
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

    //! @tstid TS-CFG-003 @verifies REQ-F-018 @design DES-SVC-CFG
    void TS_CFG_003_envOverridesFiles()
    {
        QTemporaryDir dir;
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

    //! @tstid TS-CFG-004 @verifies REQ-F-017 @design DES-SVC-CFG
    void TS_CFG_004_liveRequiresCredentialsAndRealMode()
    {
        QTemporaryDir dir;
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
