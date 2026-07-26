#include "MockHttpServer.h"

// Out-of-line definitions: header-inline (comdat) methods get compiled into
// both the test TU and the automoc TU, and the coverage instrumentation can
// emit divergent records for them (llvm-cov: "functions have mismatched
// data"). One TU per binary keeps the coverage data unambiguous.

QString MockHttpServer::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
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
