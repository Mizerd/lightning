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

QString InsecureFallbackSecretStore::accountGroup(const QString &userId)
{
    // Escape the separators in Matrix user ids so QSettings groups don't break.
    QString safeUser = userId;
    safeUser.replace(QLatin1Char('/'), QLatin1Char('_'));
    safeUser.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return QStringLiteral("%1/%2").arg(QLatin1String(kGroup), safeUser);
}

QString InsecureFallbackSecretStore::settingsKey(const QString &userId,
                                                 const QString &key) const
{
    return QStringLiteral("%1/%2").arg(accountGroup(userId), key);
}

bool InsecureFallbackSecretStore::hasAnySecretFor(const QString &userId) const
{
    // WHETHER THIS ACCOUNT'S RECORD LIVES HERE, which is the only thing that
    // can make a miss under substitution conclusive. See lastReadFailed().
    m_store->beginGroup(accountGroup(userId));
    const bool any = !m_store->childKeys().isEmpty();
    m_store->endGroup();
    return any;
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
    m_lastReadFailed = false;
    m_lastReadFound = false;
    m_lastReadUserHasRecord = false;
    const QVariant value = m_store->value(settingsKey(userId, key));
    // ASK, do not assume. QSettings reports an unreadable or unparsable
    // backing file only through status(); value() itself answers an empty
    // QVariant and nothing else. status() records the FIRST error and keeps
    // it, which is the honest behaviour here: a corrupt config does not heal
    // itself between two reads, and every answer from it stays untrusted.
    const QSettings::Status status = m_store->status();
    if (status != QSettings::NoError) {
        m_lastReadFailed = true;
        // Never the file's contents and never the key — this string reaches
        // logs and the Settings screen.
        m_lastError = status == QSettings::AccessError
            ? QStringLiteral("the settings file could not be read")
            : QStringLiteral("the settings file is malformed");
        return {};
    }
    // A VALUE IN HAND is what lets a substituted store vouch for this read.
    // Empty is a MISS — not a failure, and not automatically unknowable
    // either: see lastReadFailed() for what makes a miss conclusive.
    const QString secret = value.toString();
    m_lastReadFound = !secret.isEmpty();
    // Only worth asking on a miss, and only the miss consults it.
    m_lastReadUserHasRecord = m_lastReadFound || hasAnySecretFor(userId);
    return secret;
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
    m_store->beginGroup(accountGroup(userId));
    m_store->remove(QString{}); // remove all keys in this group
    m_store->endGroup();
    m_store->sync();
    return true;
}
