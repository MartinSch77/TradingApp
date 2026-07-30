#ifndef TRADINGAPP_TESTS_MOCKHTTPSERVER_H
#define TRADINGAPP_TESTS_MOCKHTTPSERVER_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>

// A minimal in-process HTTP/1.1 server for integration tests: each incoming
// request (method + path with query) is answered by the handler, which returns
// the status line/headers/body to send. Runs on 127.0.0.1 with an ephemeral
// port inside the test's event loop — no network access, no threads.
class MockHttpServer : public QTcpServer
{
    Q_OBJECT;  // ";" = the repo-wide tree-sitter anchor; keep it when copying as a template
public:
    struct Response {
        qint32 status = 200;
        QByteArray body;                    // JSON payload
        QList<QByteArray> extraHeaders;     // e.g. "Retry-After: 1"
        qint32 delayMs = 0;                 // hold the response back (race tests)
    };
    using Handler = std::function<Response(const QByteArray &method, const QString &pathAndQuery)>;

    explicit MockHttpServer(Handler handler, QObject *parent = nullptr)
        : QTcpServer(parent)
        , m_handler(std::move(handler))
    {
        static_cast<void>(connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *sock = nextPendingConnection()) {
                static_cast<void>(connect(sock, &QTcpSocket::readyRead, this,
                                          [this, sock] { serve(sock); }));
                static_cast<void>(connect(sock, &QTcpSocket::disconnected, sock,
                                          &QObject::deleteLater));
            }
        }));
    }

    // Hold every response whose path contains `pathFragment` until `until`
    // returns true, instead of for a wall-clock duration. An ordering test then
    // states the ORDER it needs ("answer the rates request only once both
    // searches were answered") rather than guessing a delay long enough for it —
    // guessing is what makes such a test pass on a developer machine and fail on
    // a loaded CI runner. Polled every 10 ms, given up on after 10 s, and the
    // response is sent anyway then, so a mistaken predicate surfaces as a test
    // failure rather than a hang. Deliberately NOT a Response field: that struct
    // is aggregate-initialised at a dozen call sites, and -Wextra rejects every
    // one of them the moment it grows a member.
    void holdUntil(const QString &pathFragment, std::function<bool()> until)
    {
        m_holds.append({pathFragment, std::move(until)});
    }

    // Every request served so far, in order. Kept separate from the handler (whose
    // signature stays method+path, so no existing handler has to grow a parameter):
    // asserting on a POST BODY — "this order really went out as orderType mit with
    // that triggerRate" — is the exception, not the rule.
    struct Recorded {
        QByteArray method;
        QString path;
        QByteArray body;   // empty for GET/DELETE
    };
    [[nodiscard]] QList<Recorded> requests() const;
    // The body of the last request whose path contains `pathFragment` ({} if none).
    [[nodiscard]] QByteArray lastBodyFor(const QString &pathFragment) const;

    // Base URL of the server, e.g. "http://127.0.0.1:54321".
    // Defined out-of-line (MockHttpServer.cpp) so exactly one TU per test
    // binary instruments it for coverage — duplicate comdat coverage records
    // made llvm-cov report "functions have mismatched data".
    [[nodiscard]] QString baseUrl() const;

private:
    void serve(QTcpSocket *sock);
    struct Hold {
        QString pathFragment;
        std::function<bool()> until;
    };
    QList<Hold> m_holds;
    QList<Recorded> m_requests;

    Handler m_handler;
    QHash<QTcpSocket *, QByteArray> m_buffer;
};

#endif // TRADINGAPP_TESTS_MOCKHTTPSERVER_H
