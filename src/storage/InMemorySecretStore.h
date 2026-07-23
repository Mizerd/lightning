#pragma once

#include "storage/SecretStore.h"

#include <QHash>
#include <QString>

// Process-lifetime, RAM-only SecretStore used exclusively by the development
// screenshot/demo mode.
//
// It never touches libsecret, the Freedesktop Secret Service, the Windows
// Credential Manager, or any file: every secret lives in a QHash that dies with
// the process. `isSecure()` is deliberately false so the demo can assert it is
// NOT running on a production secure store (see AppController::beginScreenshotDemo).
//
// The screenshot demo does not store real tokens at all — its fictional accounts
// are registered as non-secret metadata only — but the SettingsManager wiring
// requires *some* SecretStore, and an in-memory one guarantees that even a stray
// storeSecret() can never reach a real keychain or the developer's real config.
// No Q_OBJECT: this adds no signals/slots/properties, so it needs no separate
// meta-object (it inherits SecretStore's). That keeps it header-only with no
// MOC/.cpp, and it is only ever used behind the SecretStore* interface.
class InMemorySecretStore : public SecretStore
{
public:
    explicit InMemorySecretStore(QObject *parent = nullptr) : SecretStore(parent) {}

    bool isSecure() const override { return false; }
    bool isAvailable() const override { return true; }
    QString backendName() const override
    { return QStringLiteral("in-memory (screenshot demo)"); }

    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_secrets.insert(mapKey(userId, key), value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    {
        return m_secrets.value(mapKey(userId, key));
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        return m_secrets.remove(mapKey(userId, key)) > 0;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        const QString prefix = userId + QLatin1Char('\x1f');
        for (auto it = m_secrets.begin(); it != m_secrets.end();) {
            if (it.key().startsWith(prefix))
                it = m_secrets.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }

private:
    static QString mapKey(const QString &userId, const QString &key)
    { return userId + QLatin1Char('\x1f') + key; }

    QHash<QString, QString> m_secrets;
};
