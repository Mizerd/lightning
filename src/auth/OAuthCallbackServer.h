#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

class OAuthCallbackServer : public QObject
{
    Q_OBJECT

public:
    enum class Flow {
        OAuth,
        Sso,
    };

    // Connections allowed to wait for their request at the same time. A
    // browser opens up to four for one navigation; more are refused.
    static constexpr int kMaxPendingConnections = 16;

    explicit OAuthCallbackServer(QObject *parent = nullptr);
    ~OAuthCallbackServer() override;

    void setFlow(Flow flow) { m_flow = flow; }
    Flow flow() const { return m_flow; }

    bool listen();

    QString redirectUri() const { return m_redirectUri; }

    bool isListening() const;
    QHostAddress serverAddress() const;
    int pendingConnectionCount() const;

    void stop();

    void setTimeout(std::chrono::milliseconds timeout) { m_timeout = timeout; }
    void setRequestDeadline(std::chrono::milliseconds deadline)
    {
        m_requestDeadline = deadline;
    }

Q_SIGNALS:
    void callbackReceived(const QString &credential);
    void callbackFailed(const QString &error);
    void timedOut();

private:
    void onConnection();
    void onReadyRead(QTcpSocket *socket);
    bool isPending(QTcpSocket *socket) const;
    void finishWithSocket(QTcpSocket *socket, const QString &requestTarget);
    void answerStranger(QTcpSocket *socket, const char *why);
    void forget(QTcpSocket *socket);
    void respond(QTcpSocket *socket, const QString &title, const QString &body);

    static constexpr qint64 kMaxRequestBytes = 16 * 1024;

    QTcpServer *m_server = nullptr;
    // Accepted connections that have not sent a complete request yet.
    // Sockets delete themselves on disconnect, so raw pointers would dangle.
    QList<QPointer<QTcpSocket>> m_pending;
    QString m_redirectUri;
    QString m_callbackPath;
    QString m_callbackNonce;
    QTimer m_timer;
    std::chrono::milliseconds m_timeout{std::chrono::minutes(15)};
    std::chrono::milliseconds m_requestDeadline{std::chrono::seconds(10)};
    Flow m_flow = Flow::OAuth;
    bool m_consumed = false;

    const char *m_outcome = "stopped";
    int m_connections = 0;
    int m_strangers = 0;
    int m_refused = 0;
    int m_silentClosed = 0;
};
