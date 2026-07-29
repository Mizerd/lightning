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
        setLoginStage(QStringLiteral("starting_sync"));
        Q_EMIT isLoggedInChanged();
        Q_EMIT loginSucceeded();
    });

    connect(m_client, &MatrixClient::loginFailed, this, [this](const QString &reason) {
        setLoggingIn(false);
        setLastError(reason);
        setLoginStage(QStringLiteral("idle"));
        Q_EMIT loginFailed(reason);
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

void AuthManager::beginSsoLogin(const QString &homeserver)
{
    Q_UNUSED(homeserver);
    // Not wired to any UI yet; kept so the surface exists. Honest, version-
    // agnostic copy with no internal references, in case it is ever shown.
    setLastError(tr("Single sign-on is not available yet. Sign in with your "
                    "Matrix user ID and password."));
    Q_EMIT loginFailed(m_lastError);
}

void AuthManager::beginOidcLogin(const QString &homeserver)
{
    Q_UNUSED(homeserver);
    setLastError(tr("OIDC / Matrix Authentication Service sign-in is not "
                    "available yet. Sign in with your Matrix user ID and "
                    "password."));
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

void AuthManager::setLoginStage(const QString &stage)
{
    if (m_loginStage == stage)
        return;
    m_loginStage = stage;
    Q_EMIT loginStageChanged();
}
