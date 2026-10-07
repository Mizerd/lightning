#include "auth/AuthManager.h"

#include "app/UrlLauncher.h"
#include "matrix/MatrixClient.h"
#include "storage/AppDataPaths.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QHostAddress>
#include <QRegularExpression>
#include <QUrl>

#include <utility>

namespace {
Q_LOGGING_CATEGORY(lcAuthLogout, "lightning.auth.logout")
} // namespace

AuthManager::AuthManager(MatrixClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    Q_ASSERT(m_client);

    connect(m_client, &MatrixClient::loginSucceeded, this, [this](const QString &) {
        // Only a sign-in started through this manager was interactive; every
        // restore (launch, account switch, add-account rollback) reaches the
        // client directly and arrives here with m_loggingIn false.
        m_lastSignInInteractive = m_loggingIn;
        ++m_sessionGeneration;
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

    // Arrives just before the answer below, for the same server.
    connect(m_client, &MatrixClient::authDiscoveryDetails, this,
            [this](const QString &homeserver, const QVariantMap &details) {
        m_detailsFor = homeserver;
        m_details = details;
    });

    connect(m_client, &MatrixClient::authMethodsDiscovered, this,
            [this](const QString &homeserver, bool password, bool oauth, bool sso) {
        const bool haveDetails = std::exchange(m_detailsFor, QString()) == homeserver;
        const QVariantMap details = std::exchange(m_details, QVariantMap());
        // The field has since been cleared or changed to text that is not an
        // address, which asks nothing, so this answers an earlier server.
        if (std::exchange(m_dropNextDiscovery, false))
            return;
        m_discoveredHomeserver = homeserver;
        m_serverPassword = password;
        m_serverOauth = oauth && m_client->supportsOAuthLogin();
        // Gated on backend capability, as OAuth is.
        m_serverSso = sso && m_client->supportsSsoLogin();
        // Without details, a server that offers nothing is most likely one
        // that could not be asked.
        m_serverReachable = haveDetails
            ? details.value(QStringLiteral("reachable"), true).toBool()
            : (password || oauth || sso);
        m_serverInsecure = haveDetails
            && isInsecureRemoteUrl(details.value(QStringLiteral("resolvedHomeserver")).toString());
        m_serverCanCreate = haveDetails
            && details.value(QStringLiteral("oauthCanCreate")).toBool();
        // Opened in the browser: https with a host, or nothing.
        const QUrl account(details.value(QStringLiteral("accountManagementUrl")).toString());
        m_accountManagementUrl = haveDetails && account.scheme() == QLatin1String("https")
                && !account.host().isEmpty() && account.userInfo().isEmpty()
            ? account.toString(QUrl::FullyEncoded) : QString();
        // A previous server's provider list must not survive into this one.
        m_ssoProviders.clear();
        // A server with its own sign-in page is signed in to there, as Element
        // does (its Login.ts keeps only the OAuth flow when the server has
        // one), so its compatibility SSO providers are not asked for.
        if (m_serverSso && !m_serverOauth) {
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
        // Nothing this build can use is "failed"; discoveryProblem says whether
        // the server could not be reached or answered with nothing usable.
        m_discoveryState = (m_serverPassword || m_serverOauth || m_serverSso)
            ? QStringLiteral("done") : QStringLiteral("failed");
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
        ++m_sessionGeneration;
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

namespace {

// One pass of AuthManager::normalizedServerAddress(); see there.
QString normalizeServerAddressOnce(const QString &typed)
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

    // Without a scheme, the text must be a server name with at most slashes
    // or a pasted API path after it; checked on the path as typed and again
    // once "." and ".." are resolved.
    const auto nameOnlyPath = [](const QString &path) {
        return path.isEmpty() || path.indexOf(apiPath) == 0
            || path == QString(path.size(), QLatin1Char('/'));
    };
    QString scheme = QStringLiteral("https");
    bool schemeless = false;
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
        if (!name.hasMatch() || !nameOnlyPath(name.captured(3)))
            return {};
        schemeless = true;
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
    if (!url.isValid() || url.host().isEmpty())
        return {};
    // "." and ".." first, so every check below sees the path that will be
    // sent: "/a/..//evil.com" is "//evil.com", "/_matrix/../../evil" is
    // "/evil".
    url = url.adjusted(QUrl::NormalizePathSegments);
    if (secondHost.match(url.path()).hasMatch()
        || (schemeless && !nameOnlyPath(url.path())))
        return {};

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

} // namespace

QString AuthManager::normalizedServerAddress(const QString &typed)
{
    // Only a fixed point is an answer: an address that would normalise to
    // something else next time (an encoded "%2F" that decodes into a new path
    // separator) is not one address, so it is refused.
    const QString once = normalizeServerAddressOnce(typed);
    if (once.isEmpty() || normalizeServerAddressOnce(once) != once)
        return {};
    return once;
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
    if (m_logoutPending)
        return; // already leaving the call on the way out
    if (!m_beforeLogout) {
        m_client->logout();
        return;
    }
    // The session this sign-out is for. The client's logout() acts on
    // whatever session it holds when it runs, so a deferred one must check
    // that it is still this one.
    const QString userId = m_client->currentUserId();
    const quint64 generation = m_sessionGeneration;
    setLogoutPending(true);
    QPointer<AuthManager> self(this);
    m_beforeLogout([self, userId, generation] {
        if (!self || !self->m_logoutPending)
            return;
        self->setLogoutPending(false);
        MatrixClient *client = self->m_client;
        if (!client || generation != self->m_sessionGeneration
            || client->currentUserId() != userId) {
            qCWarning(lcAuthLogout)
                << "deferred sign-out dropped: the session it was for is no "
                   "longer the current one";
            Q_EMIT self->logoutAbandoned();
            return;
        }
        client->logout();
    });
}

void AuthManager::setLogoutPending(bool pending)
{
    if (m_logoutPending == pending)
        return;
    m_logoutPending = pending;
    Q_EMIT logoutPendingChanged();
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
    // The same server, answered or being asked: nothing changes, so the
    // sign-in choices do not blink when the field loses focus.
    if (!hs.isEmpty() && hs == m_discoveredHomeserver
        && (m_discoveryState == QLatin1String("done")
            || m_discoveryState == QLatin1String("probing")))
        return;
    // Asking nothing leaves a probe in flight answering for a server no
    // longer on screen; a new probe replaces it in the backend.
    if (!hs.isEmpty())
        m_dropNextDiscovery = false;
    else if (m_discoveryState == QLatin1String("probing"))
        m_dropNextDiscovery = true;
    // Another server: the last one's error no longer applies.
    if (hs != m_discoveredHomeserver)
        setLastError({});
    // Reset first so a previous server's results never show against this one.
    m_discoveredHomeserver = hs;
    m_serverPassword = false;
    m_serverOauth = false;
    m_serverSso = false;
    m_serverReachable = true;
    m_serverInsecure = false;
    m_serverCanCreate = false;
    m_accountManagementUrl.clear();
    // Text that is not an address is never sent to be probed.
    m_discoveryState = typed.isEmpty() ? QStringLiteral("idle")
                       : hs.isEmpty()  ? QStringLiteral("failed")
                                       : QStringLiteral("probing");
    // A backend that has no browser sign-in has nothing to ask either: it
    // signs in with a password (the mock and the HTTP backends).
    if (!hs.isEmpty() && !m_client->supportsOAuthLogin() && !m_client->supportsSsoLogin()) {
        m_serverPassword = true;
        m_discoveryState = QStringLiteral("done");
        Q_EMIT discoveryChanged();
        return;
    }
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
    if (!refuseBeforeTheBrowser(hs))
        return;
    setLoggingIn(true);
    setBrowserLoginInProgress(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->beginOAuthLogin(hs);
}

void AuthManager::beginBrowserSignUp(const QString &homeserver)
{
    if (!m_client || m_loggingIn || m_browserLoginInProgress)
        return;
    const QString hs = normalizedServerAddress(homeserver);
    // Only for the server discovery said can do it; the button is shown for
    // nothing else.
    if (hs.isEmpty() || hs != m_discoveredHomeserver || !serverCanCreateAccount()) {
        setLastError(tr("This server does not create accounts from Lightning."));
        Q_EMIT loginFailed(m_lastError);
        return;
    }
    if (!refuseBeforeTheBrowser(hs))
        return;
    setLoggingIn(true);
    setBrowserLoginInProgress(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->beginOAuthSignUp(hs);
}

bool AuthManager::openAccountPage()
{
    if (!serverOffersAccountPage())
        return false;
    return lightning::urls::openExternally(QUrl(m_accountManagementUrl));
}

QString AuthManager::discoveryProblem() const
{
    if (m_discoveryState != QLatin1String("failed"))
        return {};
    if (m_discoveredHomeserver.isEmpty())
        return QStringLiteral("not_an_address");
    return m_serverReachable ? QStringLiteral("unsupported")
                             : QStringLiteral("unreachable");
}

bool AuthManager::isInsecureRemoteUrl(const QString &url)
{
    const QUrl parsed(url);
    if (parsed.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) != 0)
        return false;
    const QString host = parsed.host().toLower();
    if (host.isEmpty())
        return false;
    if (host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost")))
        return false;
    const QHostAddress address(host);
    return address.isNull() || !address.isLoopback();
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
    if (!refuseBeforeTheBrowser(hs))
        return;
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

bool AuthManager::refuseBeforeTheBrowser(const QString &homeserver)
{
    // Empty goes on to the backend's own "a homeserver is required".
    if (homeserver.isEmpty())
        return true;
    // The browser round-trip ends with the account record, keyed on this
    // address (RustSdkMatrixClient::adoptBrowserSession). Whatever that step
    // would refuse is refused here, before the user signs in on the server's
    // page and the server issues a device nobody will use. The user id is
    // not known yet, so a stand-in checks the address and the data folder.
    matrix::app_data::AccountIdentity identity;
    if (matrix::app_data::resolveAccountIdentity(
            homeserver, QStringLiteral("@check:example.org"), &identity))
        return true;
    setLastError(tr("Lightning can't keep an account for this server on this "
                    "device, so it did not open your browser."));
    Q_EMIT loginFailed(m_lastError);
    return false;
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
