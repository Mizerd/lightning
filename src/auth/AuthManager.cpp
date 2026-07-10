#include "auth/AuthManager.h"

#include "matrix/MatrixClient.h"

AuthManager::AuthManager(MatrixClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    Q_ASSERT(m_client);

    connect(m_client, &MatrixClient::loginSucceeded, this, [this](const QString &) {
        setLoggingIn(false);
        setLastError({});
        Q_EMIT isLoggedInChanged();
        Q_EMIT loginSucceeded();
    });

    connect(m_client, &MatrixClient::loginFailed, this, [this](const QString &reason) {
        setLoggingIn(false);
        setLastError(reason);
        Q_EMIT loginFailed(reason);
    });

    connect(m_client, &MatrixClient::loggedOut, this, [this] {
        setLoggingIn(false);
        setLastError({});
        Q_EMIT isLoggedInChanged();
        Q_EMIT loggedOut();
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

void AuthManager::beginSsoLogin(const QString &homeserver)
{
    Q_UNUSED(homeserver);
    // v0.4.1 placeholder. The real flow will:
    //   1. GET /_matrix/client/v3/login → find m.login.sso.
    //   2. Redirect to /login/sso/redirect?redirectUrl=<local URL> via
    //      QDesktopServices::openUrl (system browser) — no QtWebEngine.
    //   3. Listen on a local loopback for the loginToken callback.
    //   4. POST /login with type=m.login.token to obtain access_token.
    // See docs/next-prompts.md for the exact task write-up.
    setLastError(tr("SSO login is not implemented in v0.4.1. See docs/next-prompts.md."));
    Q_EMIT loginFailed(m_lastError);
}

void AuthManager::beginOidcLogin(const QString &homeserver)
{
    Q_UNUSED(homeserver);
    setLastError(tr("OIDC / Matrix Authentication Service login is not implemented in v0.4.1. See docs/next-prompts.md."));
    Q_EMIT loginFailed(m_lastError);
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
