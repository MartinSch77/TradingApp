// Integration tests for the Claude decision adapter (DES-SVC-AI) against an
// in-process mock of the Anthropic Messages API: the unconfigured short
// circuit (no key -> immediate error result, no HTTP), the exact request
// shape on the wire (model, max_tokens, evidence prompt, structured-output
// schema, x-api-key/anthropic-version headers), the decision parse from
// content[0].text, and the HTTP-error / unparsable-reply paths. The endpoint
// is redirected to the mock via setEndpointBaseForTesting() — no test
// touches the real network.

#include "MockHttpServer.h"
#include "services/AiAdvisor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

// Generous shared bound for spy waits: the mock answers in milliseconds, the
// margin only absorbs CI load.
constexpr qint32 kWaitMs = 15000;

// Anthropic Messages API success envelope: the decision JSON rides inside the
// first text block; a non-text block leads so the block scan is exercised.
QByteArray messagesBody(const QByteArray &decisionJson)
{
    const QJsonObject thinking{{QStringLiteral("type"), QStringLiteral("thinking")},
                               {QStringLiteral("thinking"), QStringLiteral("...")}};
    const QJsonObject text{{QStringLiteral("type"), QStringLiteral("text")},
                           {QStringLiteral("text"), QString::fromUtf8(decisionJson)}};
    return QJsonDocument(QJsonObject{{QStringLiteral("content"), QJsonArray{thinking, text}}})
        .toJson(QJsonDocument::Compact);
}

// Advisor pointed at the mock, with a key so the request actually goes out.
MockHttpServer::Handler messagesMock(const QByteArray &body, qint32 status = 200)
{
    return [body, status](const QByteArray &method, const QString &path) {
        if ((method == "POST") && path.contains(QStringLiteral("/v1/messages"))) {
            return MockHttpServer::Response{status, body, {}};
        }
        return MockHttpServer::Response{404, "{}", {}};
    };
}

// The one decisionReady payload of a request round trip ({} on timeout).
AiDecision decisionFor(AiAdvisor &advisor, const QString &prompt)
{
    QSignalSpy ready(&advisor, &AiAdvisor::decisionReady);
    advisor.requestDecision(prompt);
    if (!ready.wait(kWaitMs)) {
        return {};
    }
    return qvariant_cast<AiDecision>(ready.at(0).at(0));
}

} // namespace

class TestAiAdvisor : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-AI-001 @design DES-SVC-AI
    // @relation(REQ-F-008, scope=function)
    void TS_AI_001_unconfiguredReportsErrorWithoutHttp()
    {
        MockHttpServer server(messagesMock(messagesBody("{}")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        AiAdvisor advisor{QString()};  // no anthropicApiKey configured
        advisor.setEndpointBaseForTesting(server.baseUrl());
        QVERIFY(!advisor.isConfigured());
        QSignalSpy ready(&advisor, &AiAdvisor::decisionReady);
        advisor.requestDecision(QStringLiteral("evidence"));
        QCOMPARE(ready.count(), 1);  // answered synchronously, without a round trip
        const auto d = qvariant_cast<AiDecision>(ready.at(0).at(0));
        QVERIFY(!d.ok);
        QCOMPARE(d.error, QStringLiteral("No anthropicApiKey configured."));
        QTest::qWait(100);
        QVERIFY(server.requests().isEmpty());  // nothing left the process
    }

    //! @tstid TS-AI-002 @design DES-SVC-AI
    // @relation(REQ-N-005, scope=function)
    void TS_AI_002_requestShapeOnTheWire()
    {
        MockHttpServer server(messagesMock(messagesBody(
            R"({"symbol":"SPX500","action":"HOLD","confidence":1,"leverage":1,"rationale":"r"})")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        AiAdvisor advisor{QStringLiteral("test-key")};
        advisor.setEndpointBaseForTesting(server.baseUrl());
        const AiDecision d = decisionFor(advisor, QStringLiteral("the evidence"));
        QVERIFY(d.ok);

        // REQ-N-005: the advisor is advisory-only — its complete wire traffic
        // is this single Messages call; nothing order-like ever leaves it.
        QCOMPARE(server.requests().size(), 1);
        const MockHttpServer::Recorded req = server.requests().first();
        QCOMPARE(req.method, QByteArray("POST"));
        QCOMPARE(req.path, QStringLiteral("/v1/messages"));
        // Credentials and API version reach the wire as headers.
        QVERIFY(req.headers.toLower().contains("x-api-key: test-key"));
        QVERIFY(req.headers.toLower().contains("anthropic-version: 2023-06-01"));
        // Body: model, token cap, the evidence prompt, and the JSON schema
        // that makes the reply guaranteed-parseable.
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        QCOMPARE(body.value(QStringLiteral("model")).toString(),
                 QStringLiteral("claude-opus-4-8"));
        QCOMPARE(body.value(QStringLiteral("max_tokens")).toInt(), 1024);
        const QJsonObject msg0 =
            body.value(QStringLiteral("messages")).toArray().first().toObject();
        QCOMPARE(msg0.value(QStringLiteral("role")).toString(), QStringLiteral("user"));
        QCOMPARE(msg0.value(QStringLiteral("content")).toString(),
                 QStringLiteral("the evidence"));
        QCOMPARE(body.value(QStringLiteral("output_config"))
                     .toObject()
                     .value(QStringLiteral("format"))
                     .toObject()
                     .value(QStringLiteral("type"))
                     .toString(),
                 QStringLiteral("json_schema"));
    }

    //! @tstid TS-AI-003 @design DES-SVC-AI
    // @relation(REQ-F-008, scope=function)
    void TS_AI_003_decisionParsedFromTextBlock()
    {
        MockHttpServer server(messagesMock(messagesBody(
            R"({"symbol":"SPX500","action":"buy","confidence":72.5,"leverage":5,)"
            R"("rationale":"strong momentum"})")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        AiAdvisor advisor{QStringLiteral("test-key")};
        advisor.setEndpointBaseForTesting(server.baseUrl());
        const AiDecision d = decisionFor(advisor, QStringLiteral("evidence"));
        QVERIFY(d.ok);
        QCOMPARE(d.symbol, QStringLiteral("SPX500"));
        QCOMPARE(d.action, QStringLiteral("BUY"));  // normalised to upper case
        QCOMPARE(d.confidence, 72.5);
        QCOMPARE(d.leverage, 5);
        QCOMPARE(d.rationale, QStringLiteral("strong momentum"));
        QVERIFY(d.error.isEmpty());
    }

    //! @tstid TS-AI-004 @design DES-SVC-AI
    // @relation(REQ-F-008, scope=function)
    void TS_AI_004_httpErrorReportedAsFailedDecision()
    {
        MockHttpServer server(messagesMock(R"({"error":{"message":"boom"}})", 500));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        AiAdvisor advisor{QStringLiteral("test-key")};
        advisor.setEndpointBaseForTesting(server.baseUrl());
        const AiDecision d = decisionFor(advisor, QStringLiteral("evidence"));
        QVERIFY(!d.ok);
        QVERIFY(d.error.contains(QStringLiteral("HTTP 500")));
        QCOMPARE(server.requests().size(), 1);  // POSTs are never auto-retried
    }

    //! @tstid TS-AI-005 @design DES-SVC-AI
    // @relation(REQ-F-008, scope=function)
    void TS_AI_005_unparsableReplyReportedAsFailedDecision()
    {
        MockHttpServer server(messagesMock(messagesBody("this is not JSON")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        AiAdvisor advisor{QStringLiteral("test-key")};
        advisor.setEndpointBaseForTesting(server.baseUrl());
        const AiDecision d = decisionFor(advisor, QStringLiteral("evidence"));
        QVERIFY(!d.ok);
        QCOMPARE(d.error, QStringLiteral("Claude returned an unparsable response."));
    }
};

QTEST_GUILESS_MAIN(TestAiAdvisor)
#include "tst_aiadvisor.moc"
