#pragma once

#include "storage/SecretStore.h"

class WinCredStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit WinCredStore(QObject *parent = nullptr);
    ~WinCredStore() override;

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

    // True when the last read could not be COMPLETED (as opposed to
    // completing and finding nothing). See readSecret().
    bool lastReadFailed() const override { return m_lastReadFailed; }
    QString lastError() const override { return m_lastError; }

private:
    void setError(const QString &err) const { m_lastError = err; }

    bool m_available = false;
    mutable QString m_lastError;
    mutable bool m_lastReadFailed = false;
};
