#include "auth/OAuthCallbackServer.h"

#include <QLoggingCategory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QRandomGenerator>
#include <QUrlQuery>

namespace {
Q_LOGGING_CATEGORY(lcOAuthCb, "matrix.oauth")

constexpr auto kCallbackPath = "/callback";
} // namespace

OAuthCallbackServer::OAuthCallbackServer(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        // stop() first so a callback arriving during delivery is refused.
        stop();
        Q_EMIT timedOut();
    });
}

OAuthCallbackServer::~OAuthCallbackServer()
{
    stop();
}

bool OAuthCallbackServer::listen()
{
    if (m_server)
        return m_server->isListening();

    m_server = new QTcpServer(this);
    // Loopback only: an unauthenticated HTTP listener must not be reachable
    // from another host. Port 0 takes an ephemeral port.
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        qCWarning(lcOAuthCb) << "could not bind a loopback port for the sign-in callback";
        delete m_server;
        m_server = nullptr;
        return false;
    }

    // Per-attempt secret in the path. OAuth is protected by the SDK's `state`
    // check, but m.login.sso returns only `loginToken`, so without this any
    // local process or port-sweeping web page could deliver an attacker's
    // token and sign the user into the attacker's account (login CSRF).
    // It lives in the path, not the query, so it survives a homeserver that
    // reflects only the redirect URI.
    m_callbackNonce = QString::fromLatin1(
        QByteArray::number(QRandomGenerator::system()->generate64(), 16)
        + QByteArray::number(QRandomGenerator::system()->generate64(), 16));
    m_callbackPath = QLatin1String(kCallbackPath) + QLatin1Char('/')
        + m_callbackNonce;
    m_redirectUri = QStringLiteral("http://127.0.0.1:%1%2")
                        .arg(m_server->serverPort())
                        .arg(m_callbackPath);
    connect(m_server, &QTcpServer::newConnection, this, &OAuthCallbackServer::onConnection);
    m_timer.start(m_timeout);
    // The port is not secret; callback contents are never logged.
    qCInfo(lcOAuthCb) << "sign-in callback listening on loopback port"
                      << m_server->serverPort();
    return true;
}

bool OAuthCallbackServer::isListening() const
{
    return m_server && m_server->isListening();
}

QHostAddress OAuthCallbackServer::serverAddress() const
{
    return m_server ? m_server->serverAddress() : QHostAddress();
}

void OAuthCallbackServer::stop()
{
    m_timer.stop();
    if (m_active) {
        m_active->disconnectFromHost();
        m_active->deleteLater();
        m_active = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_redirectUri.clear();
    // A stale secret would let an abandoned sign-in's late answer be accepted.
    m_callbackPath.clear();
    m_callbackNonce.clear();
}

void OAuthCallbackServer::onConnection()
{
    if (!m_server)
        return;
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        // Single-shot: refuse everything once a callback is in progress.
        if (m_consumed || m_active) {
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }
        m_active = socket;
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void OAuthCallbackServer::onReadyRead(QTcpSocket *socket)
{
    if (m_consumed || socket != m_active)
        return;

    if (socket->bytesAvailable() > kMaxRequestBytes) {
        // Not a redirect callback; drop it unread.
        socket->disconnectFromHost();
        if (m_active == socket)
            m_active = nullptr;
        return;
    }

    // Only the request line is needed; wait until it is complete.
    if (!socket->canReadLine())
        return;

    const QByteArray line = socket->readLine(kMaxRequestBytes);
    const QList<QByteArray> parts = line.simplified().split(' ');
    if (parts.size() < 2 || parts.at(0) != "GET") {
        respond(socket, tr("Sign-in"), tr("This page is not part of the sign-in."));
        // respond() disconnects and the socket deletes itself. Release it so
        // the real callback is not refused; the single shot is not consumed.
        if (m_active == socket)
            m_active = nullptr;
        return;
    }

    finishWithSocket(socket, QString::fromLatin1(parts.at(1)));
}

void OAuthCallbackServer::finishWithSocket(QTcpSocket *socket, const QString &requestTarget)
{
    // Parse only enough to route; the SDK validates code and state.
    const QUrl target(requestTarget, QUrl::StrictMode);
    // The path must carry this attempt's secret.
    if (!target.isValid() || m_callbackPath.isEmpty()
        || target.path() != m_callbackPath) {
        // A favicon or stray request: answer it and keep waiting.
        respond(socket, tr("Sign-in"), tr("This page is not part of the sign-in."));
        if (m_active == socket)
            m_active = nullptr;
        return;
    }

    m_consumed = true;
    m_timer.stop();

    const QUrlQuery query(target);
    const QString error = query.queryItemValue(QStringLiteral("error"));

    if (!error.isEmpty()) {
        respond(socket,
                tr("Sign-in cancelled"),
                tr("You can close this window and return to Lightning."));
        // The error code is a fixed protocol token. error_description is
        // dropped: it is attacker-influenced remote text.
        Q_EMIT callbackFailed(error);
        stop();
        return;
    }

    // An empty credential counts as absent.
    const QString required = m_flow == Flow::Sso ? QStringLiteral("loginToken")
                                                 : QStringLiteral("code");
    const QString credential = query.queryItemValue(required);
    if (credential.isEmpty()) {
        respond(socket,
                tr("Sign-in failed"),
                tr("The response was incomplete. You can close this window and try again."));
        Q_EMIT callbackFailed(QStringLiteral("invalid_response"));
        stop();
        return;
    }

    respond(socket,
            tr("Signed in"),
            tr("You can close this window and return to Lightning."));

    // OAuth: the absolute redirect URL, which finish_login() parses and
    // validates. SSO: the login token alone. Both are credentials.
    const QString payload =
        m_flow == Flow::Sso
            ? credential
            : QStringLiteral("http://127.0.0.1:%1%2")
                  .arg(socket->localPort()).arg(requestTarget);

    // Emit before stop(), which deletes the socket.
    Q_EMIT callbackReceived(payload);
    stop();
}

void OAuthCallbackServer::respond(QTcpSocket *socket, const QString &title, const QString &body)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;

    // Self-contained, no scripts, and nothing reflected from the request.
    const QString html = QStringLiteral(
                             "<!doctype html><html><head><meta charset=\"utf-8\">"
                             "<title>%1</title></head><body>"
                             "<h1>%1</h1><p>%2</p></body></html>")
                             .arg(title.toHtmlEscaped(), body.toHtmlEscaped());
    const QByteArray payload = html.toUtf8();

    QByteArray response;
    response += "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/html; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "X-Frame-Options: DENY\r\n";
    response += "Content-Security-Policy: default-src 'none'\r\n";
    response += "Connection: close\r\n\r\n";
    response += payload;

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}
