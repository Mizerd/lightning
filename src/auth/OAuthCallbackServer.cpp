#include "auth/OAuthCallbackServer.h"

#include <QLoggingCategory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QRandomGenerator>
#include <QUrlQuery>

#include <utility>

namespace {
Q_LOGGING_CATEGORY(lcOAuthCb, "matrix.oauth")

constexpr auto kCallbackPath = "/callback";
constexpr auto kDeadlineName = "lightningRequestDeadline";

// Index just past the blank line that ends the request head, or -1.
qsizetype endOfRequestHead(const QByteArray &bytes)
{
    const qsizetype crlf = bytes.indexOf("\r\n\r\n");
    const qsizetype lf = bytes.indexOf("\n\n");
    if (crlf >= 0 && (lf < 0 || crlf < lf))
        return crlf + 4;
    return lf >= 0 ? lf + 2 : -1;
}

// An OAuth error code is a fixed protocol token, but it arrives in a URL
// anyone can send, so only a token-shaped value reaches the log.
QString loggableErrorCode(const QString &error)
{
    if (error.isEmpty() || error.size() > 40)
        return QStringLiteral("other");
    for (const QChar c : error) {
        if (!(c.isLower() && c.unicode() < 128) && c != QLatin1Char('_'))
            return QStringLiteral("other");
    }
    return error;
}
} // namespace

OAuthCallbackServer::OAuthCallbackServer(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_outcome = "timed-out";
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
    m_consumed = false;
    m_outcome = "stopped";
    m_connections = m_strangers = m_refused = m_silentClosed = 0;
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

int OAuthCallbackServer::pendingConnectionCount() const
{
    int count = 0;
    for (const QPointer<QTcpSocket> &socket : m_pending) {
        if (socket)
            ++count;
    }
    return count;
}

void OAuthCallbackServer::stop()
{
    m_timer.stop();
    const QList<QPointer<QTcpSocket>> pending = std::exchange(m_pending, {});
    for (const QPointer<QTcpSocket> &socket : pending) {
        if (!socket)
            continue;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    if (m_server) {
        // Whether the browser reached us at all, without anything it sent.
        qCInfo(lcOAuthCb) << "sign-in callback listener closed outcome=" << m_outcome
                          << "connections=" << m_connections
                          << "not-the-callback=" << m_strangers
                          << "refused=" << m_refused
                          << "closed-silent=" << m_silentClosed;
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_redirectUri.clear();
    // A stale secret would let an abandoned sign-in's late answer be accepted.
    m_callbackPath.clear();
    m_callbackNonce.clear();
}

void OAuthCallbackServer::forget(QTcpSocket *socket)
{
    m_pending.removeIf([socket](const QPointer<QTcpSocket> &p) {
        return p.isNull() || p.data() == socket;
    });
}

void OAuthCallbackServer::onConnection()
{
    if (!m_server)
        return;
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        ++m_connections;
        forget(nullptr);   // drop entries whose socket is gone
        // Every connection is served on its own: a browser may open a spare
        // connection that never speaks and send the request on another, and
        // serving only the first reset the real callback (Vivaldi, Chromium).
        if (m_consumed || m_pending.size() >= kMaxPendingConnections) {
            ++m_refused;
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_pending.append(socket);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        // A connection that never sends a request must not stay open for the
        // whole attempt.
        auto *deadline = new QTimer(socket);
        deadline->setObjectName(QLatin1String(kDeadlineName));
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, this, [this, socket] {
            ++m_silentClosed;
            forget(socket);
            socket->abort();
            socket->deleteLater();
        });
        deadline->start(m_requestDeadline);
    }
}

bool OAuthCallbackServer::isPending(QTcpSocket *socket) const
{
    for (const QPointer<QTcpSocket> &p : m_pending) {
        if (p && p.data() == socket)
            return true;
    }
    return false;
}

void OAuthCallbackServer::onReadyRead(QTcpSocket *socket)
{
    if (m_consumed || !isPending(socket))
        return;

    if (socket->bytesAvailable() > kMaxRequestBytes) {
        // Not a redirect callback; drop it unread.
        ++m_strangers;
        forget(socket);
        socket->abort();
        socket->deleteLater();
        return;
    }

    // Wait for the whole request head, so the answer is not followed by a
    // reset for unread bytes, which can cost the browser the page.
    const qsizetype headEnd = endOfRequestHead(socket->peek(kMaxRequestBytes));
    if (headEnd < 0)
        return;
    const QByteArray head = socket->read(headEnd);
    // It spoke in time; from here the answer decides what happens to it.
    if (auto *deadline = socket->findChild<QTimer *>(QLatin1String(kDeadlineName),
                                                     Qt::FindDirectChildrenOnly))
        deadline->stop();
    const qsizetype lineEnd = head.indexOf('\n');
    const QList<QByteArray> parts = head.left(lineEnd).simplified().split(' ');
    if (parts.size() < 2 || parts.at(0) != "GET") {
        answerStranger(socket, "not a GET");
        return;
    }

    finishWithSocket(socket, QString::fromLatin1(parts.at(1)));
}

void OAuthCallbackServer::answerStranger(QTcpSocket *socket, const char *why)
{
    // A favicon, a probe or a POST: answer it and keep waiting. It does not
    // consume the single shot.
    ++m_strangers;
    qCInfo(lcOAuthCb) << "sign-in callback: answered a request that is not the callback:"
                      << why;
    forget(socket);
    respond(socket, tr("Sign-in"), tr("This page is not part of the sign-in."));
}

void OAuthCallbackServer::finishWithSocket(QTcpSocket *socket, const QString &requestTarget)
{
    // Parse only enough to route; the SDK validates code and state.
    const QUrl target(requestTarget, QUrl::StrictMode);
    // The path must carry this attempt's secret.
    if (!target.isValid() || m_callbackPath.isEmpty()
        || target.path() != m_callbackPath) {
        answerStranger(socket, !target.isValid() ? "unparsable target"
                               : target.path().startsWith(QLatin1String(kCallbackPath))
                                   ? "wrong callback secret"
                                   : "other path");
        return;
    }

    m_consumed = true;
    m_timer.stop();
    forget(socket);

    const QUrlQuery query(target);
    const QString error = query.queryItemValue(QStringLiteral("error"));

    if (!error.isEmpty()) {
        m_outcome = "error-response";
        qCInfo(lcOAuthCb) << "sign-in callback carried an error code:"
                          << loggableErrorCode(error);
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
        m_outcome = "incomplete";
        respond(socket,
                tr("Sign-in failed"),
                tr("The response was incomplete. You can close this window and try again."));
        Q_EMIT callbackFailed(QStringLiteral("invalid_response"));
        stop();
        return;
    }

    m_outcome = "callback";
    // OAuth: the absolute redirect URL, which finish_login() parses and
    // validates. SSO: the login token alone. Both are credentials. Built
    // before answering: a closed socket no longer knows its port.
    const QString payload =
        m_flow == Flow::Sso
            ? credential
            : QStringLiteral("http://127.0.0.1:%1%2")
                  .arg(m_server ? m_server->serverPort() : socket->localPort())
                  .arg(requestTarget);

    // Nothing has been checked yet: the SDK still has to redeem the
    // credential, and that can fail. The page must not claim success.
    respond(socket,
            tr("Finishing sign-in"),
            tr("You can close this window and return to Lightning."));

    // Emit before stop(), which deletes the listener and its sockets.
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
