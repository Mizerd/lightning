#include "auth/OAuthCallbackServer.h"

#include <QLoggingCategory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

namespace {
Q_LOGGING_CATEGORY(lcOAuthCb, "matrix.oauth")

// The path the redirect URI advertises. A request for anything else is not our
// callback (browsers routinely ask for /favicon.ico on a rendered page).
constexpr auto kCallbackPath = "/callback";
} // namespace

OAuthCallbackServer::OAuthCallbackServer(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        // Resolve the wait rather than leaving the UI stuck. stop() first so a
        // callback arriving during signal delivery cannot also be accepted.
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
    // Loopback ONLY. QHostAddress::LocalHost is 127.0.0.1: the endpoint is
    // unreachable from any other host, which is what makes an unauthenticated
    // HTTP listener acceptable here at all. Port 0 asks the OS for an
    // ephemeral port.
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        qCWarning(lcOAuthCb) << "could not bind a loopback port for the sign-in callback";
        delete m_server;
        m_server = nullptr;
        return false;
    }

    m_redirectUri = QStringLiteral("http://127.0.0.1:%1%2")
                        .arg(m_server->serverPort())
                        .arg(QLatin1String(kCallbackPath));
    connect(m_server, &QTcpServer::newConnection, this, &OAuthCallbackServer::onConnection);
    m_timer.start(m_timeout);
    // The port is not a secret, and it is useful when diagnosing a browser
    // that never comes back. The callback CONTENTS are never logged.
    qCInfo(lcOAuthCb) << "sign-in callback listening on loopback port"
                      << m_server->serverPort();
    return true;
}

bool OAuthCallbackServer::isListening() const
{
    return m_server && m_server->isListening();
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
}

void OAuthCallbackServer::onConnection()
{
    if (!m_server)
        return;
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        // Single-shot: once a callback has been accepted, refuse everything
        // else outright instead of parsing it.
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
        // Not a redirect callback. Drop it without reading it into memory.
        socket->disconnectFromHost();
        if (m_active == socket)
            m_active = nullptr;
        return;
    }

    // We only need the request line: "GET /callback?... HTTP/1.1". Wait until
    // a full line is available rather than guessing at a partial read.
    if (!socket->canReadLine())
        return;

    const QByteArray line = socket->readLine(kMaxRequestBytes);
    const QList<QByteArray> parts = line.simplified().split(' ');
    if (parts.size() < 2 || parts.at(0) != "GET") {
        respond(socket, tr("Sign-in"), tr("This page is not part of the sign-in."));
        // respond() disconnects, and the disconnected->deleteLater connection
        // then destroys this socket. Releasing m_active is therefore
        // mandatory: leaving it set would both dangle (stop() would touch a
        // freed socket) and permanently block the real callback, because
        // onConnection() refuses every later connection while m_active is
        // non-null. The single shot is NOT consumed here — this was not our
        // callback.
        if (m_active == socket)
            m_active = nullptr;
        return;
    }

    finishWithSocket(socket, QString::fromLatin1(parts.at(1)));
}

void OAuthCallbackServer::finishWithSocket(QTcpSocket *socket, const QString &requestTarget)
{
    // Parse only enough to route: is this the callback, and did the server
    // report an error rather than a code? The code and state are NOT read
    // here — the whole URL goes to the SDK, which owns their validation.
    const QUrl target(requestTarget, QUrl::StrictMode);
    if (!target.isValid() || target.path() != QLatin1String(kCallbackPath)) {
        // A favicon or stray request. Answer it and keep waiting for the real
        // callback; do NOT consume the single shot. Clearing m_active is what
        // makes "keep waiting" actually true — see onReadyRead.
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
        // The OAuth error code is a fixed protocol token (access_denied,
        // invalid_request, …), not user data, so it is safe to pass on. The
        // human-readable error_description is deliberately dropped: it is
        // attacker-influenceable text from a remote server.
        Q_EMIT callbackFailed(error);
        stop();
        return;
    }

    if (!query.hasQueryItem(QStringLiteral("code"))) {
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

    // The absolute redirect URL, reassembled from the endpoint we advertised
    // and the target the browser asked for. Carries the authorization code:
    // never logged, never shown.
    const QString absolute =
        QStringLiteral("http://127.0.0.1:%1%2").arg(socket->localPort()).arg(requestTarget);

    // Emit BEFORE stop(): stop() deletes the socket and clears state, and the
    // consumer only needs the string.
    Q_EMIT callbackReceived(absolute);
    stop();
}

void OAuthCallbackServer::respond(QTcpSocket *socket, const QString &title, const QString &body)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;

    // A minimal self-contained page. No external resources, no scripts, and
    // nothing echoed back from the request — the response must never reflect
    // attacker-supplied query content.
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
    // The page is a dead end; make sure nothing is cached or framed.
    response += "Cache-Control: no-store\r\n";
    response += "X-Frame-Options: DENY\r\n";
    response += "Content-Security-Policy: default-src 'none'\r\n";
    response += "Connection: close\r\n\r\n";
    response += payload;

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}
