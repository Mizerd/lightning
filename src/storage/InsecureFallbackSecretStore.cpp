#include "storage/InsecureFallbackSecretStore.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcSecret, "matrix.secret")

namespace {
constexpr auto kGroup = "secrets";
}

InsecureFallbackSecretStore::InsecureFallbackSecretStore(QObject *parent,
                                                         bool substitutedForNative)
    : SecretStore(parent)
    , m_store(std::make_unique<QSettings>())
    , m_substitutedForNative(substitutedForNative)
{
    qCWarning(lcSecret)
        << "InsecureFallbackSecretStore active — access tokens will be "
           "written to QSettings in plaintext. This is documented in "
           "docs/threat-model.md and surfaced in the Settings screen.";
    if (substitutedForNative) {
        qCWarning(lcSecret)
            << "standing in for a native secure store that could not be "
               "opened: reads from it are reported as UNTRUSTED, so a missing "
               "secret is never taken as evidence that an account is absent.";
    }
}

QString InsecureFallbackSecretStore::backendName() const
{
    return QStringLiteral("insecure fallback (QSettings)");
}

QString InsecureFallbackSecretStore::settingsKey(const QString &userId,
                                                 const QString &key) const
{
    // Escape the ':' in Matrix user ids so QSettings groups don't break.
    QString safeUser = userId;
    safeUser.replace(QLatin1Char('/'), QLatin1Char('_'));
    safeUser.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return QStringLiteral("%1/%2/%3").arg(QLatin1String(kGroup), safeUser, key);
}

bool InsecureFallbackSecretStore::storeSecret(const QString &userId,
                                              const QString &key,
                                              const QString &value)
{
    m_lastError.clear();
    m_store->setValue(settingsKey(userId, key), value);
    m_store->sync();
    return true;
}

QString InsecureFallbackSecretStore::readSecret(const QString &userId,
                                                const QString &key) const
{
    m_lastError.clear();
    return m_store->value(settingsKey(userId, key)).toString();
}

bool InsecureFallbackSecretStore::deleteSecret(const QString &userId,
                                                const QString &key)
{
    m_lastError.clear();
    m_store->remove(settingsKey(userId, key));
    m_store->sync();
    return true;
}

bool InsecureFallbackSecretStore::clearAccountSecrets(const QString &userId)
{
    m_lastError.clear();
    QString safeUser = userId;
    safeUser.replace(QLatin1Char('/'), QLatin1Char('_'));
    safeUser.replace(QLatin1Char('\\'), QLatin1Char('_'));
    m_store->beginGroup(QStringLiteral("%1/%2").arg(QLatin1String(kGroup), safeUser));
    m_store->remove(QString{}); // remove all keys in this group
    m_store->endGroup();
    m_store->sync();
    return true;
}
