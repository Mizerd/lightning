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
