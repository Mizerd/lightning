#pragma once

#include <QObject>
#include <QString>
#include <memory>

// Abstract on-disk secret storage.
//
// Concrete backends (v0.4):
//   * LibSecretStore              — Freedesktop Secret Service via libsecret.
//                                   Available if HAVE_LIBSECRET is defined at
//                                   build time. `isAvailable()` verifies the
//                                   session bus can actually be reached at
//                                   runtime (headless CI, containers, etc.
//                                   report unavailable).
//   * InsecureFallbackSecretStore — plaintext QSettings storage under a
//                                   dedicated group. Loudly reports insecure
//                                   status; the Settings screen surfaces a
//                                   warning banner while it is active.
//
// Callers should never pick the concrete class themselves — use
// SecretStore::createDefault(), which picks the best available backend and
// falls back to the insecure one, and check `isAvailable()` /
// `isSecure()` to render an accurate UI.
class SecretStore : public QObject
{
    Q_OBJECT
public:
    explicit SecretStore(QObject *parent = nullptr) : QObject(parent) {}
    ~SecretStore() override = default;

    // Factory: returns the best available backend for the platform, falling
    // back to the insecure store. Never returns null.
    static std::unique_ptr<SecretStore> createDefault(QObject *parent = nullptr);

    // True if a native, OS-integrated secure store is reachable.
    // False for InsecureFallbackSecretStore.
    virtual bool isSecure() const = 0;

    // True if the backend is currently reachable (e.g. the DBus session bus
    // is available for libsecret). InsecureFallbackSecretStore always
    // returns true.
    virtual bool isAvailable() const = 0;

    // Human-readable backend identifier for logs / Settings UI.
    // Examples: "libsecret (Secret Service)", "insecure fallback (QSettings)".
    virtual QString backendName() const = 0;

    // Store / read / delete a secret keyed by (userId, key). userId scopes
    // secrets to an account so multi-account (v0.5) can later target a
    // single identity for clearAccountSecrets().
    virtual bool storeSecret(const QString &userId,
                             const QString &key,
                             const QString &value) = 0;
    virtual QString readSecret(const QString &userId,
                               const QString &key) const = 0;
    virtual bool deleteSecret(const QString &userId, const QString &key) = 0;

    // Remove every secret bound to a user id. Used by logout.
    virtual bool clearAccountSecrets(const QString &userId) = 0;

    // Last error string. Empty when no error.
    virtual QString lastError() const = 0;
};
