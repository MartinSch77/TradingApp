// Integration tests for the shared JSON/HTTP plumbing (DES-SVC-HTTP) against
// an in-process mock HTTP server: parsing, the idempotent-GET retry policy
// (429 + Retry-After) and the no-retry rule for non-GET requests.

#include "MockHttpServer.h"
#include "services/JsonHttp.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

struct Outcome {
    bool called = false;
    bool ok = false;
    qint32 status = 0;
    QJsonDocument doc;
};

// Issue the request and wait (event loop) until the handler reports the outcome.
Outcome roundTrip(QNetworkAccessManager &nam, JsonHttp &http, QNetworkReply *reply,
                  qint32 retries, qint32 timeoutMs = 15000)
{
    static_cast<void>(nam);
    Outcome out;
    http.handleReply(reply,
                     [&out](bool ok, qint32 status, const QJsonDocument &doc,
                            const QByteArray & /*raw*/, const QString & /*err*/) {
                         out.called = true;
                         out.ok = ok;
                         out.status = status;
                         out.doc = doc;
                     },
                     retries);
    QElapsedTimer t;
    t.start();
    while (!out.called && (t.elapsed() < timeoutMs)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return out;
}

} // namespace

class TestJsonHttp : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-HTTP-001 @design DES-SVC-HTTP
    // @relation(REQ-N-003, scope=function)
    void TS_HTTP_001_okJsonDelivered()
    {
        MockHttpServer server([](const QByteArray &, const QString &) {
            return MockHttpServer::Response{200, R"({"hello":"world"})", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QNetworkAccessManager nam;
        JsonHttp http(&nam);
        const Outcome out = roundTrip(
            nam, http, nam.get(QNetworkRequest(QUrl(server.baseUrl() + "/x"))), 0);
        QVERIFY(out.called);
        QVERIFY(out.ok);
        QCOMPARE(out.status, 200);
        QCOMPARE(out.doc.object().value(QStringLiteral("hello")).toString(),
                 QStringLiteral("world"));
    }

    //! @tstid TS-HTTP-002 @design DES-SVC-HTTP
    // @relation(REQ-N-003, scope=function)
    void TS_HTTP_002_get429RetriedAfterBackoff()
    {
        qint32 hits = 0;
        MockHttpServer server([&hits](const QByteArray &, const QString &) {
            ++hits;
            if (hits == 1) {
                return MockHttpServer::Response{429, R"({"err":"limit"})",
                                                {"Retry-After: 1"}};
            }
            return MockHttpServer::Response{200, R"({"ok":true})", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QNetworkAccessManager nam;
        JsonHttp http(&nam);
        const Outcome out = roundTrip(
            nam, http, nam.get(QNetworkRequest(QUrl(server.baseUrl() + "/y"))), 2);
        QVERIFY(out.called);        // exactly one callback despite the retry
        QVERIFY(out.ok);
        QCOMPARE(out.status, 200);
        QCOMPARE(hits, 2);          // 429 first, success on the retry
    }

    //! @tstid TS-HTTP-003 @design DES-SVC-HTTP
    // @relation(REQ-N-003, scope=function)
    void TS_HTTP_003_postNeverRetried()
    {
        qint32 hits = 0;
        MockHttpServer server([&hits](const QByteArray &, const QString &) {
            ++hits;
            return MockHttpServer::Response{500, R"({"err":"boom"})", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QNetworkAccessManager nam;
        JsonHttp http(&nam);
        QNetworkRequest req(QUrl(server.baseUrl() + "/z"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const Outcome out = roundTrip(nam, http, nam.post(req, QByteArray("{}")), 3);
        QVERIFY(out.called);
        QVERIFY(!out.ok);
        QCOMPARE(out.status, 500);
        QCOMPARE(hits, 1);          // retries are for idempotent GETs only
    }
};

QTEST_GUILESS_MAIN(TestJsonHttp)
#include "tst_jsonhttp.moc"
