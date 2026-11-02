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
Q_LOGGING_CATEGORY(lcAuth, "lightning.auth")
Q_LOGGING_CATEGORY(lcAuthLogout, "lightning.auth.logout")
}

AuthManager::AuthManager(MatrixClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    Q_ASSERT(m_client);

    m_abandonedWaitNote.setSingleShot(true);
    connect(&m_abandonedWaitNote, &QTimer::timeout, this, [this] {
        if (!m_loggingIn && m_lastError == abandonedWaitText())
            setLastError({});
    });

    connect(m_client, &MatrixClient::loginSucceeded, this, [this](const QString &) {
        m_lastSignInInteractive = m_loggingIn;
        const bool keepError =
            std::exchange(m_errorSurvivesRestore, false) && !m_loggingIn;
        m_abandonedLogin.invalidate();
        m_browserLaunchFailed = false;
        ++m_sessionGeneration;
        setLoggingIn(false);
        setBrowserLoginInProgress(false);
        m_discoveryDeferred = false;
        m_deferredDiscovery.clear();
        if (!keepError)
            setLastError({});
        setLoginStage(QStringLiteral("starting_sync"));
        Q_EMIT isLoggedInChanged();
        Q_EMIT loginSucceeded();
    });

    connect(m_client, &MatrixClient::loginFailed, this, [this](const QString &reason) {
        m_errorSurvivesRestore = m_loggingIn;
        m_abandonedLogin.invalidate();
        const QString shown =
            std::exchange(m_browserLaunchFailed, false) ? browserLaunchFailureText()
                                                        : reason;
        setLoggingIn(false);
        setBrowserLoginInProgress(false);
        setLastError(shown);
        setLoginStage(QStringLiteral("idle"));
        Q_EMIT loginFailed(shown);
        runDeferredDiscovery();
    });

    connect(m_client, &MatrixClient::authDiscoveryDetails, this,
            [this](const QString &homeserver, const QVariantMap &details) {
        m_detailsFor = homeserver;
        m_details = details;
    });

    connect(m_client, &MatrixClient::authMethodsDiscovered, this,
            [this](const QString &homeserver, bool password, bool oauth, bool sso) {
        const bool haveDetails = std::exchange(m_detailsFor, QString()) == homeserver;
        const QVariantMap details = std::exchange(m_details, QVariantMap());
        if (std::exchange(m_dropNextDiscovery, false))
            return;
        m_discoveredHomeserver = homeserver;
        m_browserLoginUnavailable = false;
        m_serverPassword = password;
        m_serverOauth = oauth && m_client->supportsOAuthLogin();
        m_serverSso = sso && m_client->supportsSsoLogin();
        m_serverReachable = haveDetails
            ? details.value(QStringLiteral("reachable"), true).toBool()
            : (password || oauth || sso);
        m_serverInsecure = haveDetails
            && isInsecureRemoteUrl(details.value(QStringLiteral("resolvedHomeserver")).toString());
        m_serverCanCreate = haveDetails
            && details.value(QStringLiteral("oauthCanCreate")).toBool();
        const QUrl account(details.value(QStringLiteral("accountManagementUrl")).toString());
        m_accountManagementUrl = haveDetails && account.scheme() == QLatin1String("https")
                && !account.host().isEmpty() && account.userInfo().isEmpty()
            ? account.toString(QUrl::FullyEncoded) : QString();
        m_ssoProviders.clear();
        if (m_serverSso && !m_serverOauth) {
            QMetaObject::invokeMethod(this, [this, homeserver] {
                if (m_client && m_serverSso && !m_browserLoginInProgress
                    && homeserver == m_discoveredHomeserver)
                    m_client->requestSsoProviders(homeserver);
            }, Qt::QueuedConnection);
        }
        m_discoveryState = (m_serverPassword || m_serverOauth || m_serverSso)
            ? QStringLiteral("done") : QStringLiteral("failed");
        Q_EMIT discoveryChanged();
    });

    connect(m_client, &MatrixClient::ssoProvidersReceived, this,
            [this](const QString &homeserver, bool sso,
                   const QVariantList &providers) {
        if (!m_discoveredHomeserver.isEmpty() && homeserver != m_discoveredHomeserver)
            return;
        if (!sso)
            return;
        m_ssoProviders = providers;
        Q_EMIT discoveryChanged();
    });

    // The backend reports the page's URL right after a refused launch too, so
    // only an attempt still running, whose page did open, waits for the
    // browser. The URL is deliberately not stored or surfaced.
    connect(m_client, &MatrixClient::ssoBrowserUrlReady, this, [this](const QString &) {
        if (m_browserLoginInProgress && !m_browserLaunchFailed)
            setLoginStage(QStringLiteral("waiting_for_browser"));
    });

    connect(m_client, &MatrixClient::browserLaunchFailed, this, [this] {
        if (!m_browserLoginInProgress || m_browserLaunchFailed)
            return;
        m_browserLaunchFailed = true;
        m_browserLoginUnavailable = true;
        Q_EMIT discoveryChanged();
        QMetaObject::invokeMethod(this, [this] {
            if (!m_client || !m_browserLaunchFailed || !m_browserLoginInProgress)
                return;
            m_client->cancelOAuthLogin();
            m_client->cancelSsoLogin();
        }, Qt::QueuedConnection);
    });

    connect(m_client, &MatrixClient::oauthBrowserUrlReady, this, [this](const QString &) {
        if (m_browserLoginInProgress && !m_browserLaunchFailed)
            setLoginStage(QStringLiteral("waiting_for_browser"));
    });

    connect(m_client, &MatrixClient::loggedOut, this, [this] {
        m_abandonedLogin.invalidate();
        m_errorSurvivesRestore = false;
        ++m_sessionGeneration;
        setLoggingIn(false);
        setLastError({});
        setLoginStage(QStringLiteral("idle"));
        Q_EMIT isLoggedInChanged();
        Q_EMIT loggedOut();
    });

    connect(m_client, &MatrixClient::connectionStateChanged,
            this, [this](MatrixClient::ConnectionState state) {
        if (state == MatrixClient::Connecting && m_loggingIn)
            setLoginStage(QStringLiteral("authenticating"));
        else if (state == MatrixClient::Syncing)
            setLoginStage(QStringLiteral("ready"));
    });
}

namespace {

QString normalizeServerAddressOnce(const QString &typed)
{
    QString text = typed.trimmed();
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
    if (text.isEmpty() || text.contains(QLatin1Char('@')))
        return {};

    static const QRegularExpression schemeRe(
        QStringLiteral("^([A-Za-z][A-Za-z0-9+.-]*)://"));
    static const QRegularExpression anotherScheme(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:/"));
    static const QRegularExpression serverName(QStringLiteral(
        "^((?:[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)*)|\\[[0-9A-Fa-f:.]+\\])"
        "(:[0-9]{1,5})?(/.*)?$"));
    static const QRegularExpression apiPath(
        QStringLiteral("/(?:_matrix|\\.well-known)(?:/|$)"),
        QRegularExpression::CaseInsensitiveOption);

    const auto nameOnlyPath = [](const QString &path) {
        return path.isEmpty() || path.indexOf(apiPath) == 0
            || path == QString(path.size(), QLatin1Char('/'));
    };
    QString scheme = QStringLiteral("https");
    bool schemeless = false;
    const QRegularExpressionMatch match = schemeRe.match(text);
    if (match.hasMatch()) {
        scheme = match.captured(1).toLower();
        if (fromUserId
            || (scheme != QLatin1String("https") && scheme != QLatin1String("http")))
            return {};
        text = text.mid(match.capturedLength());
        if (anotherScheme.match(text).hasMatch())
            return {};
    } else {
        const QRegularExpressionMatch name = serverName.match(text);
        if (!name.hasMatch() || !nameOnlyPath(name.captured(3)))
            return {};
        schemeless = true;
        if (fromUserId && name.captured(2).isEmpty()
            && !name.captured(1).contains(QLatin1Char('.'))
            && !name.captured(1).startsWith(QLatin1Char('['))
            && name.captured(1).compare(QLatin1String("localhost"), Qt::CaseInsensitive) != 0)
            return {};
    }

    static const QRegularExpression secondHost(QStringLiteral("^//+[^/]"));
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

}

QString AuthManager::normalizedServerAddress(const QString &typed)
{
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
    if (m_loggingIn || refuseWhileAbandonedLoginRuns())
        return;
    const QString hs = normalizedServerAddress(homeserver);
    if (hs.isEmpty() && !homeserver.trimmed().isEmpty()) {
        refuseServerAddress();
        return;
    }
    m_errorSurvivesRestore = false;
    setLoggingIn(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->login(hs, user, password);
}

void AuthManager::logout()
{
    if (m_logoutPending)
        return;
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
    m_errorSurvivesRestore = false;
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

void AuthManager::discoverAuthMethods(const QString &homeserver, bool force)
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
    if (!hs.isEmpty() && hs == m_discoveredHomeserver
        && (m_discoveryState == QLatin1String("probing")
            || (m_discoveryState == QLatin1String("done") && !force)))
        return;
    if (!hs.isEmpty())
        m_dropNextDiscovery = false;
    else if (m_discoveryState == QLatin1String("probing"))
        m_dropNextDiscovery = true;
    if (hs != m_discoveredHomeserver)
        setLastError({});
    m_discoveredHomeserver = hs;
    m_serverPassword = false;
    m_serverOauth = false;
    m_serverSso = false;
    m_serverReachable = true;
    m_serverInsecure = false;
    m_serverCanCreate = false;
    m_accountManagementUrl.clear();
    m_browserLoginUnavailable = false;
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
    if (!m_client || m_loggingIn || m_browserLoginInProgress
        || refuseWhileAbandonedLoginRuns())
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
    m_browserLaunchFailed = false;
    m_errorSurvivesRestore = false;
    setLoggingIn(true);
    setBrowserLoginInProgress(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->beginOAuthLogin(hs);
}

void AuthManager::beginBrowserSignUp(const QString &homeserver)
{
    if (!m_client || m_loggingIn || m_browserLoginInProgress
        || refuseWhileAbandonedLoginRuns())
        return;
    const QString hs = normalizedServerAddress(homeserver);
    if (hs.isEmpty() || hs != m_discoveredHomeserver || !serverCanCreateAccount()) {
        setLastError(tr("This server does not create accounts from Lightning."));
        Q_EMIT loginFailed(m_lastError);
        return;
    }
    if (!refuseBeforeTheBrowser(hs))
        return;
    m_browserLaunchFailed = false;
    m_errorSurvivesRestore = false;
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
    m_client->cancelOAuthLogin();
    m_client->cancelSsoLogin();
}

QString AuthManager::abandonedWaitText()
{
    return tr("The server has not answered the cancelled sign-in yet. Try "
              "again in a moment.");
}

bool AuthManager::refuseWhileAbandonedLoginRuns()
{
    // A cancelled password sign-in's request still runs on the backend's
    // handle, and its answer reaches the same handlers as any sign-in's. A new
    // password sign-in would open the store it holds; a browser one runs on
    // another handle, so nothing replaces the first and its answer would land
    // in the middle of the second (ending its wait, or signing in as the
    // password account while the loopback page stays open). So every start
    // waits for that answer, bounded. Nothing is attempted, so nothing is
    // reported as a failure for AppController to roll back.
    if (!m_abandonedLogin.isValid())
        return false;
    if (!m_abandonedLogin.hasExpired(m_abandonedLoginBoundMs)) {
        setLastError(abandonedWaitText());
        const qint64 left =
            m_abandonedLoginBoundMs - m_abandonedLogin.elapsed();
        m_abandonedWaitNote.start(static_cast<int>(qMax<qint64>(left, 0) + 50));
        return true;
    }
    qCWarning(lcAuth) << "a cancelled password sign-in never answered; "
                         "starting the next one anyway";
    m_abandonedLogin.invalidate();
    return false;
}

void AuthManager::cancelLogin()
{
    if (!m_client || !m_loggingIn)
        return;
    if (m_browserLoginInProgress) {
        cancelBrowserLogin();
        return;
    }
    qCInfo(lcAuth) << "password sign-in cancelled; its request is abandoned";
    m_abandonedLogin.start();
    const QString reason = tr("Stopped waiting for the server. If it still "
                              "completes this sign-in, the account may open "
                              "on its own.");
    m_errorSurvivesRestore = true;
    setLoggingIn(false);
    setLastError(reason);
    setLoginStage(QStringLiteral("idle"));
    Q_EMIT loginFailed(reason);
}

QString AuthManager::browserLaunchFailureText() const
{
    return m_serverPassword
        ? tr("Lightning couldn't open this server's sign-in page in your "
             "browser. Sign in with your password instead.")
        : tr("Lightning couldn't open this server's sign-in page in your "
             "browser.");
}

void AuthManager::beginSsoLogin(const QString &homeserver, const QString &idpId)
{
    if (!m_client || m_loggingIn || m_browserLoginInProgress
        || refuseWhileAbandonedLoginRuns())
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
    m_browserLaunchFailed = false;
    m_errorSurvivesRestore = false;
    setLoggingIn(true);
    setBrowserLoginInProgress(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->beginSsoLogin(hs, idpId);
}

void AuthManager::beginOidcLogin(const QString &homeserver)
{
    beginBrowserLogin(homeserver);
}

void AuthManager::runDeferredDiscovery()
{
    if (!m_discoveryDeferred || m_browserLoginInProgress)
        return;
    m_discoveryDeferred = false;
    const QString typed = std::exchange(m_deferredDiscovery, QString());
    if (normalizedServerAddress(typed) == m_discoveredHomeserver
        && m_discoveryState == QLatin1String("done"))
        return;
    discoverAuthMethods(typed);
}

bool AuthManager::refuseBeforeTheBrowser(const QString &homeserver)
{
    if (homeserver.isEmpty())
        return true;
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
