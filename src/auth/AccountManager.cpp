#include "auth/AccountManager.h"

AccountManager::AccountManager(QObject *parent)
    : QObject(parent)
{
}

void AccountManager::setActiveUser(const QString &userId)
{
    if (m_activeUserId == userId)
        return;
    m_activeUserId = userId;
    if (!userId.isEmpty() && !m_knownUserIds.contains(userId)) {
        m_knownUserIds.append(userId);
        Q_EMIT knownUserIdsChanged();
    }
    Q_EMIT activeUserIdChanged();
}

void AccountManager::clearActiveUser()
{
    if (m_activeUserId.isEmpty())
        return;
    m_activeUserId.clear();
    Q_EMIT activeUserIdChanged();
}

void AccountManager::addAccount(const QString &userId)
{
    if (userId.isEmpty() || m_knownUserIds.contains(userId))
        return;
    m_knownUserIds.append(userId);
    Q_EMIT knownUserIdsChanged();
}

void AccountManager::removeAccount(const QString &userId)
{
    if (!m_knownUserIds.removeOne(userId))
        return;
    Q_EMIT knownUserIdsChanged();
    if (m_activeUserId == userId)
        clearActiveUser();
}

void AccountManager::switchTo(const QString &userId)
{
    if (!m_knownUserIds.contains(userId))
        return;
    setActiveUser(userId);
}
