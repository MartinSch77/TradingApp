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
    Q_OBJECT
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

    // Base URL of the server, e.g. "http://127.0.0.1:54321".
    // Defined out-of-line (MockHttpServer.cpp) so exactly one TU per test
    // binary instruments it for coverage — duplicate comdat coverage records
    // made llvm-cov report "functions have mismatched data".
    QString baseUrl() const;

    qint32 requestCount() const { return m_requests; }

private:
    void serve(QTcpSocket *sock);

    Handler m_handler;
    QHash<QTcpSocket *, QByteArray> m_buffer;
    qint32 m_requests = 0;
};

#endif // TRADINGAPP_TESTS_MOCKHTTPSERVER_H
