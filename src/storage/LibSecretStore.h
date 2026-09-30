#pragma once

#include "storage/SecretStore.h"

// Freedesktop Secret Service backend via libsecret. Only compiled when
// HAVE_LIBSECRET is defined (see CMakeLists.txt — CMake probes libsecret-1
// via pkg-config). At runtime, isAvailable() calls secret_service_get_sync
// to confirm a session bus + Secret Service is actually reachable — CI /
// container environments may have neither and must fall back cleanly.
//
// One Secret Service serves every install in the desktop session (native,
// Flatpak via --talk-name=org.freedesktop.secrets, snap via
// password-manager-service), while each keeps its own account records and SDK
// stores. Items therefore carry the install's scope as an attribute, under a
// schema older builds never search: Secret Service lookups and clears match a
// SUBSET of attributes, so an extra attribute on the old schema would still be
// read, and cleared on sign-out, by every older build.
class LibSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit LibSecretStore(QObject *parent = nullptr);
    ~LibSecretStore() override;

    bool isSecure() const override { return true; }
    bool isAvailable() const override;
    QString backendName() const override;
    bool isSharedBetweenInstalls() const override { return true; }

    // All four act on this install's items only, and refuse (a read reports
    // lastReadFailed()) while no install scope is set.
    bool storeSecret(const QString &userId,
                     const QString &key,
                     const QString &value) override;
    QString readSecret(const QString &userId,
                       const QString &key) const override;
    bool deleteSecret(const QString &userId, const QString &key) override;
    bool clearAccountSecrets(const QString &userId) override;

    // The pre-scoping item (schema net.smetonis.matrixclient.Secret).
    QString readLegacySecret(const QString &userId,
                             const QString &key) const override;
    bool deleteLegacySecret(const QString &userId,
                            const QString &key) override;

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
