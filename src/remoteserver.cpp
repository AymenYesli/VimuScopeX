#include "remoteserver.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

namespace {
constexpr qint64 kMaxRequest = 16 * 1024;       // HTTP request head
constexpr qint64 kMaxFrame = 64 * 1024;         // a phone's command is tiny
constexpr qint64 kBackedUp = 512 * 1024;        // skip frames above this queue

QByteArray httpResponse(const QByteArray &status, const QByteArray &type, const QByteArray &body)
{
    return "HTTP/1.1 " + status + "\r\nContent-Type: " + type
           + "\r\nContent-Length: " + QByteArray::number(body.size())
           + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}
}

RemoteServer::RemoteServer(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this))
{
    newKey();
    connect(m_server, &QTcpServer::newConnection, this, &RemoteServer::onNewConnection);
}

RemoteServer::~RemoteServer()
{
    stop();
}

bool RemoteServer::start(quint16 port, QString *error)
{
    stop();
    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        if (error)
            *error = m_server->errorString();
        return false;
    }
    return true;
}

void RemoteServer::stop()
{
    m_server->close();
    const QList<QTcpSocket *> sockets = m_connections.keys();
    for (QTcpSocket *s : sockets)
        drop(s);
}

bool RemoteServer::isRunning() const { return m_server->isListening(); }
quint16 RemoteServer::port() const { return m_server->serverPort(); }

void RemoteServer::newKey()
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    QString key;
    for (int i = 0; i < 12; ++i)
        key += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(int(sizeof alphabet) - 1)]);
    m_key = key;
    // Phones holding the old key are disconnected: the new URL is the only way in.
    const QList<QTcpSocket *> sockets = m_connections.keys();
    for (QTcpSocket *s : sockets)
        drop(s);
}

int RemoteServer::clientCount() const
{
    int n = 0;
    for (const Connection &c : m_connections)
        n += c.webSocket ? 1 : 0;
    return n;
}

void RemoteServer::onNewConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        m_connections.insert(socket, Connection());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { drop(socket); });
    }
}

void RemoteServer::onReadyRead(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;
    it->buffer += socket->readAll();
    if (it->webSocket)
        handleFrames(socket, *it);
    else
        handleHttp(socket, *it);
}

void RemoteServer::handleHttp(QTcpSocket *socket, Connection &c)
{
    const int headEnd = c.buffer.indexOf("\r\n\r\n");
    if (headEnd < 0) {
        if (c.buffer.size() > kMaxRequest)
            drop(socket);
        return;
    }
    const QList<QByteArray> lines = c.buffer.left(headEnd).split('\n');
    c.buffer.remove(0, headEnd + 4);
    const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
    if (request.size() < 2 || request[0] != "GET") {
        socket->write(httpResponse("405 Method Not Allowed", "text/plain", "GET only\n"));
        socket->disconnectFromHost();
        return;
    }
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = lines[i].indexOf(':');
        if (colon > 0)
            headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
    }
    const QUrl url(QStringLiteral("http://host") + QString::fromUtf8(request[1]));
    const bool keyOk = QUrlQuery(url).queryItemValue(QStringLiteral("k")) == m_key;
    const QString path = url.path();

    if (path == QLatin1String("/ws") && headers.value("upgrade").toLower() == "websocket") {
        if (!keyOk) {
            socket->write(httpResponse("403 Forbidden", "text/plain", "Wrong key\n"));
            socket->disconnectFromHost();
            return;
        }
        // RFC 6455 handshake: SHA-1 of the client's key plus the fixed GUID.
        const QByteArray accept = QCryptographicHash::hash(
            headers.value("sec-websocket-key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
            QCryptographicHash::Sha1).toBase64();
        socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
        c.webSocket = true;
        emit clientsChanged(clientCount());
        if (!c.buffer.isEmpty())
            handleFrames(socket, c);
        return;
    }
    if (path == QLatin1String("/") && keyOk) {
        socket->write(httpResponse("200 OK", "text/html; charset=utf-8", m_page));
    } else if (path == QLatin1String("/")) {
        socket->write(httpResponse("403 Forbidden", "text/html; charset=utf-8",
            "<html><body style='font-family:sans-serif;background:#111;color:#ddd;padding:2em'>"
            "<h2>VimuScope X</h2><p>Scan the QR code on the Remote page to connect.</p>"
            "</body></html>"));
    } else {
        socket->write(httpResponse("404 Not Found", "text/plain", "Not found\n"));
    }
    socket->disconnectFromHost();
}

void RemoteServer::handleFrames(QTcpSocket *socket, Connection &c)
{
    for (;;) {
        QByteArray &b = c.buffer;
        if (b.size() < 2)
            return;
        const quint8 b0 = quint8(b[0]), b1 = quint8(b[1]);
        const int opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        qint64 length = b1 & 0x7F;
        int header = 2;
        if (length == 126) {
            if (b.size() < 4)
                return;
            length = (quint8(b[2]) << 8) | quint8(b[3]);
            header = 4;
        } else if (length == 127) {
            if (b.size() < 10)
                return;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) | quint8(b[2 + i]);
            header = 10;
        }
        if (length > kMaxFrame) {
            drop(socket);
            return;
        }
        const int maskAt = header;
        if (masked)
            header += 4;
        if (b.size() < header + length)
            return;
        QByteArray payload = b.mid(header, int(length));
        if (masked) {
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = char(payload[i] ^ b[maskAt + (i & 3)]);
        }
        b.remove(0, header + int(length));

        if (opcode == 0x1) {                              // text: a command
            const QJsonDocument doc = QJsonDocument::fromJson(payload);
            if (doc.isObject())
                emit command(doc.object());
            // A command can end this connection (a new key drops everyone):
            // `c` is gone then, so stop here.
            if (!m_connections.contains(socket))
                return;
        } else if (opcode == 0x8) {                       // close
            sendFrame(socket, 0x8, QByteArray());
            socket->disconnectFromHost();
            return;
        } else if (opcode == 0x9) {                       // ping
            sendFrame(socket, 0xA, payload);
        }
    }
}

void RemoteServer::sendFrame(QTcpSocket *socket, int opcode, const QByteArray &payload)
{
    QByteArray frame;
    frame.append(char(0x80 | opcode));
    const qint64 n = payload.size();
    if (n < 126) {
        frame.append(char(n));
    } else if (n < 65536) {
        frame.append(char(126));
        frame.append(char((n >> 8) & 0xFF));
        frame.append(char(n & 0xFF));
    } else {
        frame.append(char(127));
        for (int i = 7; i >= 0; --i)
            frame.append(char((n >> (8 * i)) & 0xFF));
    }
    frame.append(payload);
    socket->write(frame);
}

void RemoteServer::broadcast(const QByteArray &utf8Text)
{
    for (auto it = m_connections.begin(); it != m_connections.end(); ++it) {
        if (!it->webSocket || it.key()->bytesToWrite() > kBackedUp)
            continue;
        sendFrame(it.key(), 0x1, utf8Text);
    }
}

void RemoteServer::drop(QTcpSocket *socket)
{
    const auto it = m_connections.find(socket);
    if (it == m_connections.end())
        return;
    const bool wasClient = it->webSocket;
    m_connections.erase(it);
    socket->disconnect(this);
    socket->disconnectFromHost();
    socket->deleteLater();
    if (wasClient)
        emit clientsChanged(clientCount());
}
