#pragma once

#include "storage/SecretStore.h"

class MacKeychainStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit MacKeychainStore(QObject *parent = nullptr);
    ~MacKeychainStore() override;

    bool isSecure() const override { return true; }
    bool isAvailable() const override;
    QString backendName() const override;

    bool storeSecret(const QString &userId,
                     const QString &key,
                     const QString &value) override;
    QString readSecret(const QString &userId,
                       const QString &key) const override;
    bool deleteSecret(const QString &userId, const QString &key) override;
    bool clearAccountSecrets(const QString &userId) override;

    // True when the last read could not be COMPLETED (a denied prompt, a
    // locked keychain), as opposed to completing and finding nothing.
    bool lastReadFailed() const override { return m_lastReadFailed; }
    QString lastError() const override { return m_lastError; }

private:
    bool m_available = false;
    mutable QString m_lastError;
    mutable bool m_lastReadFailed = false;
};
