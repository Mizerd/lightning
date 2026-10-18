#pragma once

#include "storage/TwoBackendSecretStore.h"

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
// read, and cleared on sign-out, by every older build. Inside a Flatpak or
// snap libsecret may keep them in a file keyring instead, decided per process
// (see TwoBackendSecretStore).
class LibSecretStore final : public TwoBackendSecretStore
{
    Q_OBJECT
public:
    explicit LibSecretStore(QObject *parent = nullptr);
    ~LibSecretStore() override;

    bool isSecure() const override { return true; }
    bool isAvailable() const override;
    QString backendName() const override;
    bool isSharedBetweenInstalls() const override { return true; }

    // Reads, writes and clears (TwoBackendSecretStore) act on this install's
    // items only, and refuse (a read reports lastReadFailed()) while no
    // install scope is set.

    // The pre-scoping item (schema net.smetonis.matrixclient.Secret).
    QString readLegacySecret(const QString &userId,
                             const QString &key) const override;
    bool deleteLegacySecret(const QString &userId,
                            const QString &key) override;

    QString backendKind() const override { return m_backendKind; }

protected:
    // Backend::Default is libsecret's password API, which goes wherever
    // libsecret chose for this process; Backend::Service is the Secret Service
    // API, which reaches the Secret Service whatever that choice was, with the
    // same schema, attributes and collection.
    QString readFrom(Backend backend, const QString &userId,
                     const QString &key) const override;
    bool storeIn(Backend backend, const QString &userId, const QString &key,
                 const QString &value) override;
    bool clearFrom(Backend backend, const QString &userId,
                   const QString *key) override;

private:
    void probe();
    void detectBackendKind();

    bool m_available = false;
    // "file" or "service" (see SecretStore::backendKind()); empty when it
    // could not be told.
    QString m_backendKind;
};
