#pragma once

#include "storage/SecretStore.h"

// Freedesktop Secret Service backend via libsecret. Only compiled when
// HAVE_LIBSECRET is defined (see CMakeLists.txt — CMake probes libsecret-1
// via pkg-config). At runtime, isAvailable() calls secret_service_get_sync
// to confirm a session bus + Secret Service is actually reachable — CI /
// container environments may have neither and must fall back cleanly.
class LibSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit LibSecretStore(QObject *parent = nullptr);
    ~LibSecretStore() override;

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

    QString lastError() const override { return m_lastError; }
    bool lastReadFailed() const override { return m_lastReadFailed; }

private:
    void probe();
    void setError(const QString &err) const { m_lastError = err; }

    bool m_available = false;
    // Set by readSecret() when the backend itself could not answer;
    // cleared on a read that completed, whether or not it found
    // anything. Mutable because readSecret() is const.
    mutable bool m_lastReadFailed = false;
    mutable QString m_lastError;
};
