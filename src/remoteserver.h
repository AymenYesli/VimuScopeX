#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>

class QTcpServer;
class QTcpSocket;

// The Remote page's server: one port serving the phone page over HTTP and
// the live plot over a WebSocket (RFC 6455) on the same connection port.
// Written on QTcpServer (QtNetwork, part of qt6-base) rather than the
// optional QtWebSockets module, so nothing extra has to be installed on
// either platform. Only clients presenting the random key from the URL
// (the QR code carries it) are served; anyone else on the network gets 403.
class RemoteServer : public QObject
{
    Q_OBJECT
public:
    explicit RemoteServer(QObject *parent = nullptr);
    ~RemoteServer() override;

    bool start(quint16 port, QString *error);
    void stop();
    bool isRunning() const;
    quint16 port() const;

    QString key() const { return m_key; }
    void newKey();                  // revokes every phone that had the old URL
    int clientCount() const;

    void setPage(const QByteArray &html) { m_page = html; }
    // To every connected phone; skipped for one whose connection is backed
    // up (a slow phone gets fewer frames rather than ever-growing latency).
    void broadcast(const QByteArray &utf8Text);

signals:
    void command(const QJsonObject &command);
    void clientsChanged(int count);

private:
    struct Connection {
        QByteArray buffer;
        bool       webSocket = false;
    };
    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void handleHttp(QTcpSocket *socket, Connection &c);
    void handleFrames(QTcpSocket *socket, Connection &c);
    void sendFrame(QTcpSocket *socket, int opcode, const QByteArray &payload);
    void drop(QTcpSocket *socket);

    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, Connection> m_connections;
    QByteArray m_page;
    QString    m_key;
};
