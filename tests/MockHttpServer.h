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
    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

    qint32 requestCount() const { return m_requests; }

private:
    void serve(QTcpSocket *sock)
    {
        m_buffer[sock] += sock->readAll();
        const QByteArray &buf = m_buffer[sock];
        const qsizetype headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;  // request head not complete yet
        }
        // Wait for the full body when the client declared one (POST/PATCH).
        qsizetype contentLength = 0;
        const qsizetype clPos = buf.toLower().indexOf("content-length:");
        if ((clPos >= 0) && (clPos < headerEnd)) {
            const qsizetype eol = buf.indexOf("\r\n", clPos);
            contentLength = buf.mid(clPos + 15, eol - clPos - 15).trimmed().toLongLong();
        }
        if (buf.size() < (headerEnd + 4 + contentLength)) {
            return;
        }

        const QList<QByteArray> requestLine = buf.left(buf.indexOf("\r\n")).split(' ');
        const QByteArray method = requestLine.value(0);
        const QString path = QString::fromUtf8(requestLine.value(1));
        m_buffer.remove(sock);
        ++m_requests;

        const Response r = m_handler(method, path);
        QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\n";
        out += "Content-Type: application/json\r\n";
        for (const QByteArray &h : r.extraHeaders) {
            out += h + "\r\n";
        }
        out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += r.body;
        static_cast<void>(sock->write(out));
        sock->disconnectFromHost();
    }

    Handler m_handler;
    QHash<QTcpSocket *, QByteArray> m_buffer;
    qint32 m_requests = 0;
};

#endif // TRADINGAPP_TESTS_MOCKHTTPSERVER_H
