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
        // Any terminal outcome ends the browser wait. Without this the UI
        // could sit in "Waiting for your browser" after the attempt had
        // already failed.
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
        m_serverSso = sso;
        // "failed" only when the server told us nothing at all. A server that
        // genuinely offers neither is a valid, if unusual, answer — but it is
        // indistinguishable here from an unreachable one, so report the
        // conservative state and let the user retry.
        m_discoveryState = (password || oauth || sso) ? QStringLiteral("done")
                                                      : QStringLiteral("failed");
        Q_EMIT discoveryChanged();
    });

    connect(m_client, &MatrixClient::oauthBrowserUrlReady, this, [this](const QString &) {
        // The backend has opened the system browser. The URL itself is
        // deliberately not stored or surfaced here.
        setLoginStage(QStringLiteral("waiting_for_browser"));
    });

    connect(m_client, &MatrixClient::loggedOut, this, [this] {
        setLoggingIn(false);
        setLastError({});
        setLoginStage(QStringLiteral("idle"));
        Q_EMIT isLoggedInChanged();
        Q_EMIT loggedOut();
    });

    // The backend reaches Connecting only AFTER the SDK handle and the local
    // store have been opened (RustSdkMatrixClient::login() calls
    // ensureRustHandleForUser() first, then setState(Connecting)), so this
    // transition is the honest store-open → credentials-in-flight boundary.
    // Syncing is only reported once a real sync loop is running.
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

bool AuthManager::supportsOidcLogin() const
{
    return m_client && m_client->supportsOAuthLogin();
}

void AuthManager::discoverAuthMethods(const QString &homeserver)
{
    if (!m_client)
        return;
    const QString hs = homeserver.trimmed();
    // Reset first: stale results from a previously typed server must never be
    // shown against a new one.
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
    // The backend answers with loginFailed("Sign-in was cancelled."), which
    // clears the in-progress flag and the stage through the normal path — so
    // the UI can never be left stuck in a waiting state.
    m_client->cancelOAuthLogin();
}

void AuthManager::beginSsoLogin(const QString &homeserver)
{
    Q_UNUSED(homeserver);
    // Legacy Matrix SSO is not implemented, and this is a deliberate scope
    // decision rather than unfinished work: the SDK's login_sso helper needs
    // the sso-login/local-server features, whose axum dependency is not
    // vendored in this offline --locked build. Discovery still reports
    // whether a server offers SSO so the UI can say this honestly.
    setLastError(tr("This server's single sign-on is not supported by "
                    "Lightning. If the server also offers browser sign-in or a "
                    "password, use one of those instead."));
    Q_EMIT loginFailed(m_lastError);
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
