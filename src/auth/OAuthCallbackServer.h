#pragma once

#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

// The loopback redirect endpoint for an OAuth 2.0 authorization-code sign-in.
//
// matrix-sdk 0.18 ships its own local redirect server, but only behind the
// `sso-login`/`local-server` features, whose `axum` dependency is not vendored
// in this offline `--locked` build. So Lightning provides the listener while
// the SDK keeps every protocol primitive: this class never parses `code`,
// never validates `state`, and never touches PKCE. It receives one HTTP
// request and hands the raw redirect URL straight to
// `mx_rust_oauth_finish()`, which is where the SDK checks the CSRF state
// against the authorization request it built and performs the code exchange.
//
// Deliberate properties:
//   * binds QHostAddress::LocalHost ONLY, never 0.0.0.0 — no other host may
//     reach the endpoint;
//   * takes an ephemeral port from the OS (port 0), so nothing is predictable
//     across sign-ins and no fixed port has to be reserved;
//   * single-shot: the first well-formed callback wins, the listener closes
//     immediately, and any later or replayed request is refused. A second
//     delivery of the same code therefore cannot start a second exchange;
//   * bounded: a request larger than kMaxRequestBytes, or one that never
//     completes, is dropped rather than buffered;
//   * times out, so a user who closes the browser or never finishes leaves the
//     UI in a resolved failure state instead of "Signing in" forever.
//
// The received URL carries the authorization code. It is never logged, never
// placed in an error string, and never given to QML.
class OAuthCallbackServer : public QObject
{
    Q_OBJECT

public:
    explicit OAuthCallbackServer(QObject *parent = nullptr);
    ~OAuthCallbackServer() override;

    // Binds loopback on an ephemeral port. Returns false if the port could not
    // be taken, in which case redirectUri() stays empty.
    bool listen();

    // The redirect URI to register with the authorization server and pass to
    // mx_rust_oauth_begin(). Empty until listen() succeeds.
    QString redirectUri() const { return m_redirectUri; }

    bool isListening() const;

    // Stop listening and drop any half-read connection. Safe to call twice;
    // called automatically after the first accepted callback and on timeout.
    void stop();

    // How long to wait for the browser to come back before giving up.
    void setTimeout(std::chrono::milliseconds timeout) { m_timeout = timeout; }

Q_SIGNALS:
    // The full redirect URL, for mx_rust_oauth_finish(). CONTAINS THE
    // AUTHORIZATION CODE — do not log it, do not show it, do not store it.
    void callbackReceived(const QString &redirectUrl);
    // The authorization server reported a failure instead of a code (the user
    // denied consent, the request expired). `error` is the OAuth error code,
    // which is a fixed protocol token and safe to show.
    void callbackFailed(const QString &error);
    // Nothing arrived in time.
    void timedOut();

private:
    void onConnection();
    void onReadyRead(QTcpSocket *socket);
    void finishWithSocket(QTcpSocket *socket, const QString &requestTarget);
    void respond(QTcpSocket *socket, const QString &title, const QString &body);

    // A browser request line plus headers for a redirect of this shape is well
    // under 8 KiB. Anything larger is not our callback.
    static constexpr qint64 kMaxRequestBytes = 16 * 1024;

    QTcpServer *m_server = nullptr;
    // QPointer, not a raw pointer: every accepted socket carries a
    // disconnected -> deleteLater connection, so it can be destroyed behind
    // our back. A raw pointer here dangles and stop() then touches freed
    // memory. QPointer null-clears itself on destruction.
    QPointer<QTcpSocket> m_active;
    QString m_redirectUri;
    QTimer m_timer;
    std::chrono::milliseconds m_timeout{std::chrono::minutes(5)};
    // Set the moment a callback is accepted, so a replay that races the
    // teardown is still refused.
    bool m_consumed = false;
};
