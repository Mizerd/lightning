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
        record.insert(QStringLiteral("needsSignIn"), needsSignIn(uid));
        list.append(record);
    }
    return list;
}

bool AccountManager::needsSignIn(const QString &userId) const
{
    // Derived on demand, never persisted: an account row is only "needs sign
    // in" when there is nothing left that could restore it. Live crypto or
    // verification health for an INACTIVE account is deliberately not
    // reported — the SDK only exposes that for the account it is currently
    // attached to, so anything else here would be a stale guess.
    if (!m_settings)
        return false;
    if (!m_settings->hasSavedAccount(userId))
        return true;
    // An unreadable secret backend is NOT evidence that an account lost its
    // sign-in. A locked keyring or an unavailable session bus makes every
    // lookup come back empty, which would otherwise paint every row in the
    // switcher as broken — the same "no readable token means no account"
    // conflation that let the login path delete a real crypto store. When
    // the backend cannot answer, report nothing rather than a wrong answer.
    // Read FIRST, then ask whether the backend could answer: the unavailable
    // signal reflects the outcome of the most recent read, so checking it
    // beforehand would test a stale result.
    const bool tokenEmpty = m_settings->accessTokenFor(userId).isEmpty();
    if (secretBackendUnavailable())
        return false;
    return tokenEmpty;
}

bool AccountManager::secretBackendUnavailable() const
{
    // Delegates rather than re-deriving: the login path keys destructive
    // decisions on the same question, and two copies of "can the secret
    // backend answer?" would eventually disagree.
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
