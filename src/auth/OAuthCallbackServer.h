#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

// Loopback redirect endpoint for OAuth 2.0 authorization-code sign-in and
// legacy `m.login.sso`.
//
// matrix-sdk's own redirect server needs the `axum` feature, which this
// offline build does not vendor. This class only receives the redirect; the
// SDK owns every protocol step (state/CSRF check, PKCE, code exchange).
//
//   * binds loopback only, on an ephemeral port;
//   * single-shot: the first valid callback wins and replays are refused;
//   * serves several connections at once, because Chromium-based browsers
//     open a spare connection and send the request on another one;
//   * bounded in size, connection count and time, so the UI never waits
//     forever.
//
// The received credential is never logged, put in an error string, or given
// to QML.
class OAuthCallbackServer : public QObject
{
    Q_OBJECT

public:
    // Selects the required query parameter and what `callbackReceived` carries.
    enum class Flow {
        OAuth,   // requires `code`; emits the full redirect URL for the SDK
        Sso,     // requires `loginToken`; emits that token alone
    };

    // Connections allowed to wait for their request at the same time. A
    // browser opens up to four for one navigation; more are refused.
    static constexpr int kMaxPendingConnections = 16;

    explicit OAuthCallbackServer(QObject *parent = nullptr);
    ~OAuthCallbackServer() override;

    // Must be set before listen() for anything but OAuth.
    void setFlow(Flow flow) { m_flow = flow; }
    Flow flow() const { return m_flow; }

    // Binds loopback on an ephemeral port. Returns false if the port could not
    // be taken, in which case redirectUri() stays empty.
    bool listen();

    // The redirect URI to register with the authorization server and pass to
    // mx_rust_oauth_begin(). Empty until listen() succeeds.
    QString redirectUri() const { return m_redirectUri; }

    bool isListening() const;
    /// The bound address, exposed so tests can assert it is loopback.
    QHostAddress serverAddress() const;
    /// Connections accepted and still waiting to send a request.
    int pendingConnectionCount() const;

    // Stop listening and drop every open connection. Safe to call twice;
    // called automatically after the first accepted callback and on timeout.
    void stop();

    // How long to wait for the browser to come back before giving up.
    void setTimeout(std::chrono::milliseconds timeout) { m_timeout = timeout; }
    // How long one connection may stay open without sending a complete
    // request. Browsers close their spare connections after a few seconds.
    void setRequestDeadline(std::chrono::milliseconds deadline)
    {
        m_requestDeadline = deadline;
    }

Q_SIGNALS:
    // A credential in both flows: never log, show, store or pass it to QML.
    //   Flow::OAuth  the full redirect URL, for mx_rust_oauth_finish();
    //   Flow::Sso    the bare `loginToken`, for mx_rust_sso_finish().
    void callbackReceived(const QString &credential);
    // The server returned an OAuth error code (a fixed protocol token, safe
    // to show) instead of a code.
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

    // A real redirect request is well under 8 KiB.
    static constexpr qint64 kMaxRequestBytes = 16 * 1024;

    QTcpServer *m_server = nullptr;
    // Accepted connections that have not sent a complete request yet.
    // Sockets delete themselves on disconnect, so raw pointers would dangle.
    QList<QPointer<QTcpSocket>> m_pending;
    QString m_redirectUri;
    // Per-attempt secret path, `/callback/<128 bits>`; see listen().
    QString m_callbackPath;
    QString m_callbackNonce;
    QTimer m_timer;
    std::chrono::milliseconds m_timeout{std::chrono::minutes(15)};
    std::chrono::milliseconds m_requestDeadline{std::chrono::seconds(10)};
    Flow m_flow = Flow::OAuth;
    // Set as soon as a callback is accepted, so a racing replay is refused.
    bool m_consumed = false;

    // One summary line per attempt (see stop()): whether the browser ever
    // reached us is the first question a failed sign-in raises.
    const char *m_outcome = "stopped";
    int m_connections = 0;
    int m_strangers = 0;
    int m_refused = 0;
    int m_silentClosed = 0;
};
