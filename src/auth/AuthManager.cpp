#include "auth/AuthManager.h"

#include "matrix/MatrixClient.h"

AuthManager::AuthManager(MatrixClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    Q_ASSERT(m_client);

    connect(m_client, &MatrixClient::loginSucceeded, this, [this](const QString &) {
        setLoggingIn(false);
        setBrowserLoginInProgress(false);
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
    });

    connect(m_client, &MatrixClient::authMethodsDiscovered, this,
            [this](const QString &homeserver, bool password, bool oauth, bool sso) {
        m_discoveredHomeserver = homeserver;
        m_serverPassword = password;
        m_serverOauth = oauth && m_client->supportsOAuthLogin();
        // Gated on backend capability, as OAuth is.
        m_serverSso = sso && m_client->supportsSsoLogin();
        // A previous server's provider list must not survive into this one.
        m_ssoProviders.clear();
        if (m_serverSso) {
            // Until providers arrive the UI shows the generic single action.
            m_client->requestSsoProviders(homeserver);
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
    setLoggingIn(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->login(homeserver, user, password);
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
    const QString hs = homeserver.trimmed();
    // Reset first so a previous server's results never show against this one.
    m_discoveredHomeserver = hs;
    m_serverPassword = false;
    m_serverOauth = false;
    m_serverSso = false;
    m_discoveryState = hs.isEmpty() ? QStringLiteral("idle")
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
    setLoggingIn(true);
    setBrowserLoginInProgress(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->beginOAuthLogin(homeserver.trimmed());
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
    // Shares browserLoginInProgress with OAuth; cancelBrowserLogin() cancels
    // either.
    setLoggingIn(true);
    setBrowserLoginInProgress(true);
    setLastError({});
    setLoginStage(QStringLiteral("connecting"));
    m_client->beginSsoLogin(homeserver.trimmed(), idpId);
}

void AuthManager::beginOidcLogin(const QString &homeserver)
{
    // Retained name for the existing QML surface; OAuth/OIDC is the same flow.
    beginBrowserLogin(homeserver);
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
