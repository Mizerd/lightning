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
        // The accounts list derives each row's isActive flag from the
        // active account, so an active-account change must also refresh
        // the LIST. Without this, the switcher kept pre-switch flags: the
        // previously active account's row still claimed isActive and its
        // click guard silently returned — the "cannot switch back to
        // account A" trap.
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
        list.append(record);
    }
    return list;
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
