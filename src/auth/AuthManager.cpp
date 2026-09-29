#include "auth/AuthManager.h"

#include "matrix/MatrixClient.h"

#include <QRegularExpression>
#include <QUrl>

#include <utility>

AuthManager::AuthManager(MatrixClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    Q_ASSERT(m_client);

    connect(m_client, &MatrixClient::loginSucceeded, this, [this](const QString &) {
        setLoggingIn(false);
        setBrowserLoginInProgress(false);
        // Signed in: a probe asked for during the attempt is moot.
        m_discoveryDeferred = false;
        m_deferredDiscovery.clear();
        setLastError({});
        setLoginStage(QStringLiteral("starting_sync"));
        Q_EMIT isLoggedInChanged();
        Q_EMIT loginSucceeded();
    });

    connect(m_client, &MatrixClient::loginFailed, this, [this](const QString &reason) {
        setLoggingIn(false);
        // Any terminal outcome ends the browser wait.
        setBrowserLoginInProgress(false);
        setLastError(reason);
        setLoginStage(QStringLiteral("idle"));
        Q_EMIT loginFailed(reason);
        runDeferredDiscovery();
    });

    connect(m_client, &MatrixClient::authMethodsDiscovered, this,
            [this](const QString &homeserver, bool password, bool oauth, bool sso) {
        // The field has since been cleared or changed to text that is not an
        // address, which asks nothing, so this answers an earlier server.
        if (std::exchange(m_dropNextDiscovery, false))
            return;
        m_discoveredHomeserver = homeserver;
        m_serverPassword = password;
        m_serverOauth = oauth && m_client->supportsOAuthLogin();
        // Gated on backend capability, as OAuth is.
        m_serverSso = sso && m_client->supportsSsoLogin();
        // A previous server's provider list must not survive into this one.
        m_ssoProviders.clear();
        if (m_serverSso) {
            // Until providers arrive the UI shows the generic single action.
            // Asked on the next turn of the event loop, never from inside this
            // signal: the backend releases the handle that answered discovery
            // right after emitting it, and a request started here went with it
            // (the new handle can even reuse the freed address), so providers
            // never arrived. Measured on matrix.debian.social ("Salsa").
            QMetaObject::invokeMethod(this, [this, homeserver] {
                // Still the server on screen, and no browser sign-in started
                // since: asking would replace the handle that sign-in uses.
                if (m_client && m_serverSso && !m_browserLoginInProgress
                    && homeserver == m_discoveredHomeserver)
                    m_client->requestSsoProviders(homeserver);
            }, Qt::QueuedConnection);
        }
        // A server offering nothing is indistinguishable from an unreachable
        // one here, so report "failed" and let the user retry.
        m_discoveryState = (password || oauth || sso) ? QStringLiteral("done")
                                                      : QStringLiteral("failed");
        Q_EMIT discoveryChanged();
    });

    connect(m_client, &MatrixClient::ssoProvidersReceived, this,
            [this](const QString &homeserver, bool sso,
                   const QVariantList &providers) {
        // Ignore a late answer for a homeserver the user has typed away from.
        if (!m_discoveredHomeserver.isEmpty() && homeserver != m_discoveredHomeserver)
            return;
        if (!sso)
            return;
        m_ssoProviders = providers;
        Q_EMIT discoveryChanged();
    });

    connect(m_client, &MatrixClient::ssoBrowserUrlReady, this, [this](const QString &) {
        setLoginStage(QStringLiteral("waiting_for_browser"));
    });

    // The flow stays alive (Cancel and the timeout still apply), but the
    // failure is shown instead of waiting silently for the timeout.
    connect(m_client, &MatrixClient::browserLaunchFailed, this, [this] {
        setLastError(tr("Couldn't open your web browser. "
                        "Cancel and try again, or sign in another way."));
    });

    connect(m_client, &MatrixClient::oauthBrowserUrlReady, this, [this](const QString &) {
        // The URL is deliberately not stored or surfaced.
        setLoginStage(QStringLiteral("waiting_for_browser"));
    });

    connect(m_client, &MatrixClient::loggedOut, this, [this] {
        setLoggingIn(false);
        setLastError({});
        setLoginStage(QStringLiteral("idle"));
        Q_EMIT isLoggedInChanged();
        Q_EMIT loggedOut();
    });

    // Connecting is reached only after the SDK handle and store are open, so
    // it marks the store-open -> credentials-in-flight boundary.
    connect(m_client, &MatrixClient::connectionStateChanged,
            this, [this](MatrixClient::ConnectionState state) {
        if (state == MatrixClient::Connecting && m_loggingIn)
            setLoginStage(QStringLiteral("authenticating"));
        else if (state == MatrixClient::Syncing)
            setLoginStage(QStringLiteral("ready"));
    });
}

QString AuthManager::normalizedServerAddress(const QString &typed)
{
    QString text = typed.trimmed();
    // No address is this long; a paste this size is not one.
    if (text.isEmpty() || text.size() > 2048)
        return {};
    // A pasted Matrix ID names its server after the first colon. The user part
    // is dropped, never moved into another field.
    const bool fromUserId = text.startsWith(QLatin1Char('@'));
    if (fromUserId) {
        const qsizetype colon = text.indexOf(QLatin1Char(':'));
        if (colon < 2)
            return {};
        text = text.mid(colon + 1);
    }
    // No '@' belongs in a server address: "you@server" or "name:password@host"
    // is refused rather than stripped, so it is never sent anywhere.
    if (text.isEmpty() || text.contains(QLatin1Char('@')))
        return {};

    static const QRegularExpression schemeRe(
        QStringLiteral("^([A-Za-z][A-Za-z0-9+.-]*)://"));
    static const QRegularExpression anotherScheme(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:/"));
    // A server name as Matrix writes one: ASCII labels or an [IPv6] literal,
    // an optional port, then anything from the first '/'.
    static const QRegularExpression serverName(QStringLiteral(
        "^((?:[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)*)|\\[[0-9A-Fa-f:.]+\\])"
        "(:[0-9]{1,5})?(/.*)?$"));
    // A pasted endpoint (/_matrix/client/..., /.well-known/matrix/client)
    // names its server before the API path.
    static const QRegularExpression apiPath(
        QStringLiteral("/(?:_matrix|\\.well-known)(?:/|$)"),
        QRegularExpression::CaseInsensitiveOption);

    QString scheme = QStringLiteral("https");
    const QRegularExpressionMatch match = schemeRe.match(text);
    if (match.hasMatch()) {
        // A typed http:// is kept, never upgraded or downgraded; any other
        // scheme is not a homeserver. A Matrix ID's server has none.
        scheme = match.captured(1).toLower();
        if (fromUserId
            || (scheme != QLatin1String("https") && scheme != QLatin1String("http")))
            return {};
        text = text.mid(match.capturedLength());
        // "https://https://host" would make the host "https".
        if (anotherScheme.match(text).hasMatch())
            return {};
    } else {
        // No scheme means https, as matrix-sdk's discovery assumes for a server
        // name. Without one, the text must be a server name, optionally with a
        // pasted API path or slashes: a password typed here by mistake
        // ("Summer2026?", "Winter/2026") is never looked up.
        const QRegularExpressionMatch name = serverName.match(text);
        if (!name.hasMatch())
            return {};
        const QString path = name.captured(3);
        if (!path.isEmpty() && path.indexOf(apiPath) != 0
            && path != QString(path.size(), QLatin1Char('/')))
            return {};
        // A Matrix ID's server with no dot, port or brackets ("@me:Secret99")
        // is likelier a password; localhost is the exception.
        if (fromUserId && name.captured(2).isEmpty()
            && !name.captured(1).contains(QLatin1Char('.'))
            && !name.captured(1).startsWith(QLatin1Char('['))
            && name.captured(1).compare(QLatin1String("localhost"), Qt::CaseInsensitive) != 0)
            return {};
    }

    // A path that opens with "//name" reads as a second host
    // ("https://http//matrix.org"); slashes alone are only a trailing slash.
    static const QRegularExpression secondHost(QStringLiteral("^//+[^/]"));
    // QUrl lowercases the host.
    QUrl url(scheme + QLatin1String("://") + text, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()
        || secondHost.match(url.path()).hasMatch())
        return {};
    // "/a/../_matrix" resolves before the API path is looked for.
    url = url.adjusted(QUrl::NormalizePathSegments);

    // Any other path is kept: a homeserver can live under one (typed with its
    // scheme), and discovery checks it.
    QString path = url.path();
    const qsizetype cut = path.indexOf(apiPath);
    if (cut >= 0)
        path.truncate(cut);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString(QUrl::FullyEncoded);
}

bool AuthManager::isLoggedIn() const
{
    return m_client && m_client->isLoggedIn();
}

QString AuthManager::currentUserId() const
{
    return m_client ? m_client->currentUserId() : QString{};
}

void AuthManager::login(const QString &homeserver,
                        const QString &user,
                        const QString &password)
{
    if (m_loggingIn)
        return;
    // Empty goes on as is, for the backend's own "enter your homeserver".
    const QString hs = normalizedServerAddress(homeserver);
    if (hs.isEmpty() && !homeserver.trimmed().isEmpty()) {
        refuseServerAddress();
        return;
    }
    setLoggingIn(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->login(hs, user, password);
}

void AuthManager::logout()
{
    m_client->logout();
}

void AuthManager::restoreSession()
{
    if (m_client->restoreSession()) {
        Q_EMIT isLoggedInChanged();
    }
}

void AuthManager::clearLastError()
{
    setLastError({});
}

bool AuthManager::supportsSsoLogin() const
{
    return m_client && m_client->supportsSsoLogin();
}

bool AuthManager::supportsOidcLogin() const
{
    return m_client && m_client->supportsOAuthLogin();
}

void AuthManager::discoverAuthMethods(const QString &homeserver)
{
    if (!m_client)
        return;
    const QString typed = homeserver.trimmed();
    // The backend does not probe while a browser sign-in holds its handle and
    // never answers, so resetting now would leave the page without its
    // browser buttons once the attempt ends. The homeserver field asks for a
    // probe whenever the browser takes focus. Probe when the attempt ends.
    if (m_browserLoginInProgress) {
        m_deferredDiscovery = typed;
        m_discoveryDeferred = true;
        return;
    }
    const QString hs = normalizedServerAddress(typed);
    // Asking nothing leaves a probe in flight answering for a server no
    // longer on screen; a new probe replaces it in the backend.
    if (!hs.isEmpty())
        m_dropNextDiscovery = false;
    else if (m_discoveryState == QLatin1String("probing"))
        m_dropNextDiscovery = true;
    // Reset first so a previous server's results never show against this one.
    m_discoveredHomeserver = hs;
    m_serverPassword = false;
    m_serverOauth = false;
    m_serverSso = false;
    // Text that is not an address is never sent to be probed.
    m_discoveryState = typed.isEmpty() ? QStringLiteral("idle")
                       : hs.isEmpty()  ? QStringLiteral("failed")
                                       : QStringLiteral("probing");
    Q_EMIT discoveryChanged();
    if (hs.isEmpty())
        return;
    m_client->discoverAuthMethods(hs);
}

void AuthManager::beginBrowserLogin(const QString &homeserver)
{
    if (!m_client || m_loggingIn || m_browserLoginInProgress)
        return;
    if (!m_client->supportsOAuthLogin()) {
        setLastError(tr("This build cannot perform browser sign-in."));
        Q_EMIT loginFailed(m_lastError);
        return;
    }
    const QString hs = normalizedServerAddress(homeserver);
    if (hs.isEmpty() && !homeserver.trimmed().isEmpty()) {
        refuseServerAddress();
        return;
    }
    setLoggingIn(true);
    setBrowserLoginInProgress(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->beginOAuthLogin(hs);
}

void AuthManager::cancelBrowserLogin()
{
    if (!m_client || !m_browserLoginInProgress)
        return;
    // The backend answers with loginFailed, which resets the UI. Cancel both
    // flows: each is a no-op when not running.
    m_client->cancelOAuthLogin();
    m_client->cancelSsoLogin();
}

void AuthManager::beginSsoLogin(const QString &homeserver, const QString &idpId)
{
    if (!m_client || m_loggingIn)
        return;
    if (!m_client->supportsSsoLogin()) {
        setLastError(tr("This build cannot perform single sign-on."));
        Q_EMIT loginFailed(m_lastError);
        return;
    }
    const QString hs = normalizedServerAddress(homeserver);
    if (hs.isEmpty() && !homeserver.trimmed().isEmpty()) {
        refuseServerAddress();
        return;
    }
    // Shares browserLoginInProgress with OAuth; cancelBrowserLogin() cancels
    // either.
    setLoggingIn(true);
    setBrowserLoginInProgress(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->beginSsoLogin(hs, idpId);
}

void AuthManager::beginOidcLogin(const QString &homeserver)
{
    // Retained name for the existing QML surface; OAuth/OIDC is the same flow.
    beginBrowserLogin(homeserver);
}

void AuthManager::runDeferredDiscovery()
{
    if (!m_discoveryDeferred || m_browserLoginInProgress)
        return;
    m_discoveryDeferred = false;
    const QString typed = std::exchange(m_deferredDiscovery, QString());
    // The server on screen already has its answer, however it was spelled.
    if (normalizedServerAddress(typed) == m_discoveredHomeserver
        && m_discoveryState == QLatin1String("done"))
        return;
    discoverAuthMethods(typed);
}

void AuthManager::refuseServerAddress()
{
    setLastError(tr("That is not a server address. Enter one like matrix.org."));
    Q_EMIT loginFailed(m_lastError);
}

void AuthManager::setBrowserLoginInProgress(bool v)
{
    if (m_browserLoginInProgress == v)
        return;
    m_browserLoginInProgress = v;
    Q_EMIT browserLoginInProgressChanged();
}

void AuthManager::setLoggingIn(bool v)
{
    if (m_loggingIn == v)
        return;
    m_loggingIn = v;
    Q_EMIT isLoggingInChanged();
}

void AuthManager::setLastError(const QString &err)
{
    if (m_lastError == err)
        return;
    m_lastError = err;
    Q_EMIT lastErrorChanged();
}

void AuthManager::setLoginStage(const QString &stage)
{
    if (m_loginStage == stage)
        return;
    m_loginStage = stage;
    Q_EMIT loginStageChanged();
}
