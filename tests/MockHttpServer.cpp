#include "MockHttpServer.h"

#include <QTimer>

#include <algorithm>

// Out-of-line definitions: header-inline (comdat) methods get compiled into
// both the test TU and the automoc TU, and the coverage instrumentation can
// emit divergent records for them (llvm-cov: "functions have mismatched
// data"). One TU per binary keeps the coverage data unambiguous.

QString MockHttpServer::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
}

QList<MockHttpServer::Recorded> MockHttpServer::requests() const
{
    return m_requests;
}

QByteArray MockHttpServer::lastBodyFor(const QString &pathFragment) const
{
    const auto hit = std::find_if(m_requests.crbegin(), m_requests.crend(),
                                  [&pathFragment](const Recorded &r) {
                                      return r.path.contains(pathFragment);
                                  });
    return (hit == m_requests.crend()) ? QByteArray{} : hit->body;
}

void MockHttpServer::serve(QTcpSocket *sock)
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
    m_requests.append({method, path, buf.mid(headerEnd + 4, contentLength), buf.left(headerEnd)});
    m_buffer.remove(sock);

    const Response r = m_handler(method, path);
    QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\n";
    out += "Content-Type: application/json\r\n";
    for (const QByteArray &h : r.extraHeaders) {
        out += h + "\r\n";
    }
    out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += r.body;
    // sock as timer context throughout: if the client gave up and the socket
    // died, a pending send is dropped with it instead of writing to a dangling
    // pointer.
    const auto hold = std::find_if(m_holds.cbegin(), m_holds.cend(), [&path](const Hold &h) {
        return path.contains(h.pathFragment);
    });
    const std::function<bool()> gate =
        (hold == m_holds.cend()) ? std::function<bool()>{} : hold->until;
    if (gate) {
        // Poll, never block: this server runs inside the test's own event loop,
        // so blocking here would stop the very requests the predicate waits for.
        const auto sendWhenReady = [sock, out](auto &&self, std::function<bool()> ready,
                                               qint32 waitedMs) -> void {
            constexpr qint32 kPollMs = 10;
            constexpr qint32 kGiveUpMs = 10000;
            if (!ready() && (waitedMs < kGiveUpMs)) {
                QTimer::singleShot(kPollMs, sock,
                                   [self, ready, waitedMs] { self(self, ready, waitedMs + kPollMs); });
                return;
            }
            static_cast<void>(sock->write(out));
            sock->disconnectFromHost();
        };
        sendWhenReady(sendWhenReady, gate, 0);
        return;
    }
    if (r.delayMs > 0) {
        QTimer::singleShot(r.delayMs, sock, [sock, out] {
            static_cast<void>(sock->write(out));
            sock->disconnectFromHost();
        });
        return;
    }
    static_cast<void>(sock->write(out));
    sock->disconnectFromHost();
}
