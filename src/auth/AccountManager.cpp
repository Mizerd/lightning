#include "auth/AccountManager.h"

#include "app/SettingsManager.h"

AccountManager::AccountManager(SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    if (m_settings) {
        connect(m_settings, &SettingsManager::accountsChanged,
                this, &AccountManager::accountsChanged);
        connect(m_settings, &SettingsManager::sessionChanged,
                this, &AccountManager::activeUserIdChanged);
        // Each row's isActive derives from the active account, so the list
        // must refresh on a switch too.
        connect(m_settings, &SettingsManager::sessionChanged,
                this, &AccountManager::accountsChanged);
    }
}

QString AccountManager::activeUserId() const
{
    return m_settings ? m_settings->activeAccountUserId() : QString{};
}

QStringList AccountManager::knownUserIds() const
{
    return m_settings ? m_settings->savedAccountUserIds() : QStringList{};
}

QVariantList AccountManager::accounts() const
{
    QVariantList list;
    if (!m_settings)
        return list;
    const QString active = m_settings->activeAccountUserId();
    const QStringList ids = m_settings->savedAccountUserIds();
    for (const QString &uid : ids) {
        QVariantMap record = m_settings->accountRecord(uid);
        record.remove(QStringLiteral("deviceId"));
        record.remove(QStringLiteral("addedAt"));
        record.insert(QStringLiteral("isActive"), uid == active);
        record.insert(QStringLiteral("needsSignIn"), needsSignIn(uid));
        list.append(record);
    }
    return list;
}

bool AccountManager::needsSignIn(const QString &userId) const
{
    // Crypto health of inactive accounts is not reported: the SDK only knows
    // it for the attached account.
    if (!m_settings)
        return false;
    if (!m_settings->hasSavedAccount(userId))
        return true;
    // A locked keyring or missing session bus is not a lost sign-in. Read
    // first: the unavailable flag reflects the most recent read.
    const bool tokenEmpty = m_settings->accessTokenFor(userId).isEmpty();
    if (secretBackendUnavailable())
        return false;
    return tokenEmpty;
}

bool AccountManager::secretBackendUnavailable() const
{
    // Delegated so this and the login path's destructive checks agree.
    return !m_settings || m_settings->secretBackendUnavailable();
}

bool AccountManager::hasAccount(const QString &userId) const
{
    return m_settings && m_settings->hasSavedAccount(userId);
}

QVariantMap AccountManager::account(const QString &userId) const
{
    if (!m_settings)
        return {};
    QVariantMap record = m_settings->accountRecord(userId);
    record.remove(QStringLiteral("deviceId"));
    record.insert(QStringLiteral("needsSignIn"), needsSignIn(userId));
    return record;
}

void AccountManager::setActiveUser(const QString &userId)
{
    if (!m_settings)
        return;
    m_settings->setActiveAccountUserId(userId);
}

void AccountManager::clearActiveUser()
{
    if (!m_settings)
        return;
    m_settings->setActiveAccountUserId({});
}

void AccountManager::updateProfile(const QString &userId,
                                   const QString &displayName,
                                   const QString &avatarUrl)
{
    if (!m_settings)
        return;
    m_settings->updateAccountProfile(userId, displayName, avatarUrl);
}

bool AccountManager::removeAccount(const QString &userId)
{
    if (!m_settings)
        return false;
    return m_settings->clearSessionForAccount(userId);
}
