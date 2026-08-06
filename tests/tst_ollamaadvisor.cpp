// Integration tests for the LOCAL model adapter (DES-SVC-OLLAMA) against an
// in-process mock of Ollama's HTTP API: the unconfigured short circuit, the
// availability probe over /api/tags, the exact request shape on the wire
// (/api/generate with stream=false and format=json), and — the part that
// actually matters for a small local model — the DEFENSIVE parse of whatever
// it answers: JSON wrapped in prose or fences, a word where a number belongs,
// "SELL (short)" where an enum belongs, no JSON at all.
//
// The endpoint is redirected to the mock via setEndpointBaseForTesting(), so no
// test needs a running Ollama and none touches the network.

#include "MockHttpServer.h"
#include "services/OllamaAdvisor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

// Generous shared bound for spy waits: the mock answers in milliseconds, the
// margin only absorbs CI load.
constexpr qint32 kWaitMs = 15000;

// Ollama's /api/generate envelope (stream=false): the model's text rides in
// "response", with the bookkeeping fields the daemon really sends alongside.
QByteArray generateBody(const QString &responseText)
{
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("model"), QStringLiteral("qwen2.5:1.5b")},
                             {QStringLiteral("response"), responseText},
                             {QStringLiteral("done"), true},
                             {QStringLiteral("done_reason"), QStringLiteral("stop")},
                             {QStringLiteral("total_duration"), 6611000000LL},
                         })
        .toJson(QJsonDocument::Compact);
}

// /api/tags envelope: the models the daemon serves.
QByteArray tagsBody(const QStringList &names)
{
    QJsonArray models;
    for (const QString &name : names) {
        models.append(QJsonObject{{QStringLiteral("name"), name},
                                  {QStringLiteral("size"), 986061892}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("models"), models}})
        .toJson(QJsonDocument::Compact);
}

MockHttpServer::Handler generateMock(const QByteArray &body, qint32 status = 200)
{
    return [body, status](const QByteArray &method, const QString &path) {
        if ((method == "POST") && path.contains(QStringLiteral("/api/generate"))) {
            return MockHttpServer::Response{status, body, {}};
        }
        return MockHttpServer::Response{404, "{}", {}};
    };
}

// One request round trip: the picks and the error the advisor reported.
struct Answer {
    QList<AiDecision> picks;
    QString error;
    [[nodiscard]] bool empty() const { return picks.isEmpty(); }
    [[nodiscard]] AiDecision first() const { return picks.isEmpty() ? AiDecision{} : picks.first(); }
};

Answer answerFor(OllamaAdvisor &advisor, const QString &prompt)
{
    QSignalSpy ready(&advisor, &OllamaAdvisor::proposalsReady);
    advisor.requestDecision(prompt);
    // The unconfigured and busy paths answer SYNCHRONOUSLY (no request goes out),
    // so only wait when nothing has arrived yet — wait() would otherwise sit out
    // its whole timeout waiting for a second signal that is never coming.
    if (ready.isEmpty() && !ready.wait(kWaitMs)) {
        return {};
    }
    Answer out;
    const QVariantList args = ready.at(0);
    out.picks = qvariant_cast<QList<AiDecision>>(args.at(0));
    out.error = args.at(1).toString();
    return out;
}

// The single pick of an answer, for the per-field parse cases below.
AiDecision firstPickOf(const QString &modelText);

// What a model's raw answer text parses into, over a fresh mock.
Answer answerFromText(const QString &modelText)
{
    MockHttpServer server(generateMock(generateBody(modelText)));
    if (!server.listen(QHostAddress::LocalHost)) {
        return {};
    }
    OllamaAdvisor advisor(QStringLiteral("http://localhost:1"), QStringLiteral("qwen2.5:1.5b"));
    advisor.setEndpointBaseForTesting(server.baseUrl());
    return answerFor(advisor, QStringLiteral("evidence"));
}

AiDecision firstPickOf(const QString &modelText)
{
    return answerFromText(modelText).first();
}

} // namespace

class TestOllamaAdvisor : public QObject
{
    Q_OBJECT;  // ";" closes the macro for tree-sitter so StrictDoc sees the first slot's @relation marker
private slots:
    //! @tstid TS-OLLAMA-001 @design DES-SVC-OLLAMA
    // @relation(REQ-F-030, scope=function)
    void TS_OLLAMA_001_unconfiguredNeverCallsOut()
    {
        MockHttpServer server(generateMock(generateBody(QStringLiteral("{}"))));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        // No model name = not configured: an immediate error result and NO request.
        OllamaAdvisor advisor(QStringLiteral("http://localhost:11434"), QString());
        advisor.setEndpointBaseForTesting(server.baseUrl());
        QVERIFY(!advisor.isConfigured());
        const Answer a = answerFor(advisor, QStringLiteral("evidence"));
        QVERIFY(a.empty());
        QVERIFY(a.error.contains(QStringLiteral("No Ollama model")));
        QVERIFY(server.requests().isEmpty());  // nothing left the process

        // …and the availability probe says so too, without a request.
        QSignalSpy avail(&advisor, &OllamaAdvisor::availability);
        advisor.checkAvailability();
        QVERIFY(avail.wait(kWaitMs) || (avail.count() > 0));
        QVERIFY(!avail.at(0).at(0).toBool());
        QVERIFY(avail.at(0).at(1).toString().contains(QStringLiteral("no model configured")));
        QVERIFY(server.requests().isEmpty());
    }

    //! @tstid TS-OLLAMA-002 @design DES-SVC-OLLAMA
    // @relation(REQ-F-030, scope=function)
    void TS_OLLAMA_002_requestShapeOnTheWire()
    {
        MockHttpServer server(generateMock(generateBody(QStringLiteral(
            R"({"symbol":"SPX500","action":"BUY","confidence":61,"leverage":5,)"
            R"("rationale":"trend and rating agree"})"))));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        OllamaAdvisor advisor(QStringLiteral("http://localhost:1"), QStringLiteral("qwen2.5:1.5b"));
        advisor.setEndpointBaseForTesting(server.baseUrl());
        const Answer a = answerFor(advisor, QStringLiteral("EVIDENCE-MARKER"));
        QCOMPARE(a.picks.size(), 1);
        const AiDecision d = a.first();
        QVERIFY(d.ok);

        // The body carries the configured model, the caller's evidence, and the two
        // switches that make a local model usable: no streaming, JSON format.
        const QJsonObject sent =
            QJsonDocument::fromJson(server.lastBodyFor(QStringLiteral("/api/generate"))).object();
        QCOMPARE(sent.value(QStringLiteral("model")).toString(), QStringLiteral("qwen2.5:1.5b"));
        QCOMPARE(sent.value(QStringLiteral("prompt")).toString(), QStringLiteral("EVIDENCE-MARKER"));
        QCOMPARE(sent.value(QStringLiteral("stream")).toBool(), false);
        QCOMPARE(sent.value(QStringLiteral("format")).toString(), QStringLiteral("json"));
        QVERIFY(sent.value(QStringLiteral("system")).toString().contains(QStringLiteral("JSON")));
        // A trading call should not be a dice roll.
        QVERIFY(sent.value(QStringLiteral("options")).toObject()
                    .value(QStringLiteral("temperature")).toDouble() <= 0.3);

        QCOMPARE(d.symbol, QStringLiteral("SPX500"));
        QCOMPARE(d.action, QStringLiteral("BUY"));
        QCOMPARE(d.confidence, 61.0);
        QCOMPARE(d.leverage, 5);
        QCOMPARE(d.rationale, QStringLiteral("trend and rating agree"));
    }

    //! @tstid TS-OLLAMA-003 @design DES-SVC-OLLAMA
    // @relation(REQ-F-030, scope=function)
    void TS_OLLAMA_003_availabilityProbeDiagnoses()
    {
        // The daemon serves the configured model -> ready.
        MockHttpServer server([](const QByteArray &method, const QString &path) {
            if ((method == "GET") && path.contains(QStringLiteral("/api/tags"))) {
                return MockHttpServer::Response{
                    200, tagsBody({QStringLiteral("qwen2.5:1.5b"), QStringLiteral("llama3.2:latest")}),
                    {}};
            }
            return MockHttpServer::Response{404, "{}", {}};
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        OllamaAdvisor ready(QStringLiteral("http://localhost:1"), QStringLiteral("qwen2.5:1.5b"));
        ready.setEndpointBaseForTesting(server.baseUrl());
        QSignalSpy readySpy(&ready, &OllamaAdvisor::availability);
        ready.checkAvailability();
        QVERIFY(readySpy.wait(kWaitMs));
        QVERIFY(readySpy.at(0).at(0).toBool());
        QVERIFY(readySpy.at(0).at(1).toString().contains(QStringLiteral("ready")));
        QCOMPARE(readySpy.at(0).at(2).toStringList().size(), 2);

        // An implicit tag counts: "llama3.2" is served as "llama3.2:latest".
        OllamaAdvisor implicit(QStringLiteral("http://localhost:1"), QStringLiteral("llama3.2"));
        implicit.setEndpointBaseForTesting(server.baseUrl());
        QSignalSpy implicitSpy(&implicit, &OllamaAdvisor::availability);
        implicit.checkAvailability();
        QVERIFY(implicitSpy.wait(kWaitMs));
        QVERIFY(implicitSpy.at(0).at(0).toBool());

        // Daemon up, model not installed -> not ok, and it says what to run.
        OllamaAdvisor missing(QStringLiteral("http://localhost:1"), QStringLiteral("mistral"));
        missing.setEndpointBaseForTesting(server.baseUrl());
        QSignalSpy missingSpy(&missing, &OllamaAdvisor::availability);
        missing.checkAvailability();
        QVERIFY(missingSpy.wait(kWaitMs));
        QVERIFY(!missingSpy.at(0).at(0).toBool());
        QVERIFY(missingSpy.at(0).at(1).toString().contains(QStringLiteral("ollama pull mistral")));
    }

    //! @tstid TS-OLLAMA-004 @design DES-SVC-OLLAMA
    // @relation(REQ-F-030, scope=function)
    void TS_OLLAMA_004_parsesWhatSmallModelsActuallyAnswer()
    {
        // JSON wrapped in prose and markdown fences (a weak instruction follower).
        const AiDecision fenced = firstPickOf(QStringLiteral(
            "Sure! Here is my call:\n```json\n"
            R"json({"symbol":"GER40","action":"sell","confidence":"high",)json"
            R"json("leverage":"x3","rationale":"momentum fading"})json"
            "\n```\nHope that helps!"));
        QVERIFY(fenced.ok);
        QCOMPARE(fenced.symbol, QStringLiteral("GER40"));
        QCOMPARE(fenced.action, QStringLiteral("SELL"));   // lower case accepted
        QCOMPARE(fenced.confidence, 75.0);                 // "high" -> a number
        QCOMPARE(fenced.leverage, 3);                      // "x3" -> 3

        // A fraction where a percentage belongs, and a decorated action. (The
        // custom json delimiter is required: the answer contains `)"`, which would
        // end a plain raw string in the middle of the payload.)
        const AiDecision fraction = firstPickOf(QStringLiteral(
            R"json({"symbol":"GOLD","action":"SELL (short)","confidence":0.62,)json"
            R"json("leverage":2,"rationale":"broke support"})json"));
        QVERIFY(fraction.ok);
        QCOMPARE(fraction.action, QStringLiteral("SELL"));
        QCOMPARE(fraction.confidence, 62.0);

        // An action the enum does not know reads as HOLD — never as a trade.
        const AiDecision unknown = firstPickOf(
            QStringLiteral(R"json({"symbol":"SPX500","action":"WAIT","confidence":40})json"));
        QVERIFY(unknown.ok);
        QCOMPARE(unknown.action, QStringLiteral("HOLD"));
        QCOMPARE(unknown.leverage, 0);       // unstated stays 0 (the bot then sizes it)
        QCOMPARE(unknown.confidence, 40.0);

        // No JSON at all: a reported failure, not a guess.
        const Answer prose = answerFromText(QStringLiteral("I would probably buy the S&P today."));
        QVERIFY(prose.empty());
        QVERIFY(prose.error.contains(QStringLiteral("named no tradable pick")));

        // EVERY confidence spelling a 1.5B model has been seen to use, including the
        // ones that must NOT become a trade. The parse is the feature here: a
        // mis-read confidence is a silent no-trade or, worse, a sized-up one.
        const auto confidenceOf = [this](const QString &spelling) {
            return firstPickOf(QStringLiteral(R"json({"symbol":"SPX500","action":"BUY",)json")
                               + QStringLiteral(R"json("confidence":%1})json").arg(spelling))
                .confidence;
        };
        QCOMPARE(confidenceOf(QStringLiteral("\"strong\"")), 75.0);
        QCOMPARE(confidenceOf(QStringLiteral("\"MEDIUM\"")), 50.0);
        QCOMPARE(confidenceOf(QStringLiteral("\"moderate conviction\"")), 50.0);
        QCOMPARE(confidenceOf(QStringLiteral("\"low\"")), 25.0);
        QCOMPARE(confidenceOf(QStringLiteral("\"weak\"")), 25.0);
        QCOMPARE(confidenceOf(QStringLiteral("\"no idea\"")), 0.0);   // unreadable = not actionable
        QCOMPARE(confidenceOf(QStringLiteral("\"0.8\"")), 80.0);      // numeric STRING, fraction
        QCOMPARE(confidenceOf(QStringLiteral("\"70\"")), 70.0);       // numeric string, percent
        QCOMPARE(confidenceOf(QStringLiteral("1.0")), 100.0);        // the fraction boundary
        QCOMPARE(confidenceOf(QStringLiteral("55")), 55.0);
        QCOMPARE(confidenceOf(QStringLiteral("0")), 0.0);            // zero stays zero
        QCOMPARE(confidenceOf(QStringLiteral("-5")), -5.0);          // nonsense passes through
                                                                     // to the floor, not fixed up

        // …and the action words, including the CLOSE-before-SHORT ordering that keeps
        // "close the short" from opening one.
        const auto actionOf = [this](const QString &word) {
            return firstPickOf(QStringLiteral(R"json({"symbol":"SPX500","action":"%1",)json")
                                   .arg(word)
                               + QStringLiteral(R"json("confidence":60})json"))
                .action;
        };
        QCOMPARE(actionOf(QStringLiteral("go long")), QStringLiteral("BUY"));
        QCOMPARE(actionOf(QStringLiteral("SHORT it")), QStringLiteral("SELL"));
        QCOMPARE(actionOf(QStringLiteral("close the short")), QStringLiteral("CLOSE"));
        QCOMPARE(actionOf(QStringLiteral("exit now")), QStringLiteral("CLOSE"));
        QCOMPARE(actionOf(QStringLiteral("go flat")), QStringLiteral("CLOSE"));
        QCOMPARE(actionOf(QStringLiteral("hold")), QStringLiteral("HOLD"));

        // Shapes that carry no usable pick: an entry that is not an object, a keyed
        // map whose key is the symbol, and a single object with no symbol at all.
        QVERIFY(answerFromText(QStringLiteral(R"json({"picks":["SPX500","GOLD"]})json")).empty());
        const Answer keyed = answerFromText(QStringLiteral(
            R"json({"picks":{"GOLD":{"action":"BUY","confidence":70},)json"
            R"json("NOTAPICK":"nonsense"}})json"));
        QCOMPARE(keyed.picks.size(), 1);
        QCOMPARE(keyed.picks.constFirst().symbol, QStringLiteral("GOLD"));
        QVERIFY(answerFromText(QStringLiteral(R"json({"action":"BUY","confidence":70})json"))
                    .empty());
        // A symbol the catalog does not know still PARSES: the adapter's job is to read
        // what the model said, and matching a name against the instruments actually on
        // offer is the gate's (matchProposalSymbol, one unambiguous match or nothing).
        // Dropping it here would hide a model that answers about the wrong market.
        const Answer unknownSymbol =
            answerFromText(QStringLiteral(R"json({"symbol":"NOSUCHTHING","action":"BUY"})json"));
        QCOMPARE(unknownSymbol.picks.size(), 1);
        QCOMPARE(unknownSymbol.picks.constFirst().symbol, QStringLiteral("NOSUCHTHING"));
        // Prose with a stray brace but no complete object: still a reported failure.
        QVERIFY(answerFromText(QStringLiteral("well { maybe buy")).empty());
        QVERIFY(answerFromText(QString()).empty());
    }

    //! @tstid TS-OLLAMA-007 @design DES-SVC-OLLAMA
    // @relation(REQ-F-030, scope=function)
    void TS_OLLAMA_007_manyPicksSoTheRiskBudgetDecidesNotTheProtocol()
    {
        // The requested shape: a ranked list. All of them come back, in order.
        const Answer many = answerFromText(QStringLiteral(
            R"json({"picks":[)json"
            R"json({"symbol":"SPX500","action":"BUY","confidence":71,"leverage":5,"rationale":"trend"},)json"
            R"json({"symbol":"GER40","action":"BUY","confidence":58,"leverage":3,"rationale":"breakout"},)json"
            R"json({"symbol":"GOLD","action":"SELL","confidence":44,"leverage":2,"rationale":"soft"}]})json"));
        QCOMPARE(many.picks.size(), 3);
        QCOMPARE(many.picks.at(0).symbol, QStringLiteral("SPX500"));
        QCOMPARE(many.picks.at(1).action, QStringLiteral("BUY"));
        QCOMPARE(many.picks.at(2).action, QStringLiteral("SELL"));
        QCOMPARE(many.picks.at(2).confidence, 44.0);
        QVERIFY(many.error.isEmpty());

        // A SYMBOL-KEYED MAP under "picks" — the shape qwen2.5:1.5b actually
        // answered with (captured verbatim 2026-08-04, including its "rationality"
        // spelling). Parsing this wrong is a silent no-trade, so it is pinned here.
        const Answer keyed = answerFromText(QStringLiteral(
            R"json({"picks": {"SPX500": {"action": "BUY", "confidence": 98, "leverage": 20,)json"
            R"json( "rationality": "Technical BUY signal with high TV rating."}}})json"));
        QCOMPARE(keyed.picks.size(), 1);
        QCOMPARE(keyed.picks.at(0).symbol, QStringLiteral("SPX500"));
        QCOMPARE(keyed.picks.at(0).action, QStringLiteral("BUY"));
        QCOMPARE(keyed.picks.at(0).confidence, 98.0);
        QCOMPARE(keyed.picks.at(0).leverage, 20);
        QVERIFY(keyed.picks.at(0).rationale.contains(QStringLiteral("Technical BUY")));

        // …the same map with several instruments, and without the wrapper.
        QCOMPARE(answerFromText(QStringLiteral(
                     R"json({"picks":{"SPX500":{"action":"BUY","confidence":70},)json"
                     R"json("GER40":{"action":"SELL","confidence":55}}})json")).picks.size(),
                 2);
        QCOMPARE(answerFromText(QStringLiteral(
                     R"json({"GOLD":{"action":"SELL","confidence":60}})json")).picks.size(), 1);

        // A bare array, and a differently-keyed object, are the same answer.
        QCOMPARE(answerFromText(QStringLiteral(
                     R"json([{"symbol":"GER40","action":"BUY","confidence":50}])json")).picks.size(),
                 1);
        QCOMPARE(answerFromText(QStringLiteral(
                     R"json({"trades":[{"symbol":"GOLD","action":"SELL","confidence":50}]})json")).picks.size(),
                 1);
        // A SINGLE pick object is still an answer (models regress to it).
        QCOMPARE(answerFromText(QStringLiteral(
                     R"json({"symbol":"SPX500","action":"BUY","confidence":50})json")).picks.size(),
                 1);
        // "Nothing worth trading" is an empty list, reported as such — not a trade.
        const Answer none = answerFromText(QStringLiteral(R"json({"picks":[]})json"));
        QVERIFY(none.empty());
        QVERIFY(none.error.contains(QStringLiteral("named no tradable pick")));

        // A runaway generation cannot flood the books: the answer is capped. The cap
        // is 16 because the bot now shows the model 14 instruments and asks for a
        // verdict on each — an instrument left out of the answer is one nobody can
        // read an opinion from (REQ-F-034).
        QString flood = QStringLiteral(R"json({"picks":[)json");
        for (int i = 0; i < 25; ++i) {
            flood += QStringLiteral(R"json({"symbol":"SYM%1","action":"BUY","confidence":50},)json")
                         .arg(i);
        }
        flood.chop(1);
        flood += QStringLiteral("]}");
        QCOMPARE(answerFromText(flood).picks.size(), 16);

        // The rationale key models really get wrong ("rationality") still reads.
        QCOMPARE(firstPickOf(QStringLiteral(
                     R"json({"picks":[{"symbol":"SPX500","action":"BUY","confidence":80,)json"
                     R"json("rationality":"bullish trend intact"}]})json")).rationale,
                 QStringLiteral("bullish trend intact"));
    }

    //! @tstid TS-OLLAMA-005 @design DES-SVC-OLLAMA
    // @relation(REQ-F-030, scope=function)
    void TS_OLLAMA_005_transportFailuresAreReportedNotSwallowed()
    {
        // The daemon answers an error status.
        MockHttpServer server(
            generateMock(R"({"error":"model requires more system memory"})", 500));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        OllamaAdvisor advisor(QStringLiteral("http://localhost:1"), QStringLiteral("qwen2.5:1.5b"));
        advisor.setEndpointBaseForTesting(server.baseUrl());
        const Answer failed = answerFor(advisor, QStringLiteral("evidence"));
        QVERIFY(failed.empty());
        QVERIFY(failed.error.contains(QStringLiteral("Ollama request failed")));

        // Nothing listening at all (a port with no server): still a clean report,
        // and the advisor is usable again afterwards.
        OllamaAdvisor dead(QStringLiteral("http://127.0.0.1:1"), QStringLiteral("qwen2.5:1.5b"));
        const Answer unreachable = answerFor(dead, QStringLiteral("evidence"));
        QVERIFY(unreachable.empty());
        QVERIFY(!unreachable.error.isEmpty());
        QVERIFY(!dead.busy());
    }

    //! @tstid TS-OLLAMA-006 @design DES-SVC-OLLAMA
    // @relation(REQ-F-030, scope=function)
    void TS_OLLAMA_006_oneRequestAtATimeAndHostNormalisation()
    {
        MockHttpServer server(generateMock(generateBody(QStringLiteral(
            R"({"symbol":"SPX500","action":"BUY","confidence":50,"leverage":2,"rationale":"ok"})"))));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        OllamaAdvisor advisor(QStringLiteral("http://localhost:1"), QStringLiteral("qwen2.5:1.5b"));
        advisor.setEndpointBaseForTesting(server.baseUrl());
        QSignalSpy ready(&advisor, &OllamaAdvisor::proposalsReady);
        advisor.requestDecision(QStringLiteral("first"));
        QVERIFY(advisor.busy());
        // A second call while one is in flight is refused rather than queued: a slow
        // model must not accumulate work on stale evidence.
        advisor.requestDecision(QStringLiteral("second"));
        QVERIFY(ready.count() >= 1);
        QVERIFY(qvariant_cast<QList<AiDecision>>(ready.at(0).at(0)).isEmpty());
        QVERIFY(ready.at(0).at(1).toString().contains(QStringLiteral("still answering")));
        QVERIFY(ready.wait(kWaitMs));   // …and the first one still completes
        QCOMPARE(qvariant_cast<QList<AiDecision>>(ready.at(1).at(0)).size(), 1);
        QVERIFY(!advisor.busy());
        QCOMPARE(server.requests().size(), 1);

        // Ollama's own OLLAMA_HOST is conventionally scheme-less; accept it.
        const OllamaAdvisor bare(QStringLiteral("127.0.0.1:11434/"), QStringLiteral("m"));
        QCOMPARE(bare.host(), QStringLiteral("http://127.0.0.1:11434"));
    }
};

QTEST_MAIN(TestOllamaAdvisor)
#include "tst_ollamaadvisor.moc"
