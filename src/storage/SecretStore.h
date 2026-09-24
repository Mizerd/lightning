#pragma once

#include <QObject>
#include <QString>
#include <memory>

// Abstract secret storage. Backends include:
//   * LibSecretStore: Secret Service via libsecret (HAVE_LIBSECRET);
//     isAvailable() checks that the session bus is actually reachable.
//   * WinCredStore: Windows Credential Manager (HAVE_WINCRED).
//   * PortableSecretStore: sealed file for portable installations.
//   * InsecureFallbackSecretStore: plaintext QSettings; reports insecure and
//     Settings shows a warning while it is active.
//
// Use SecretStore::createDefault(), and check isAvailable()/isSecure() for an
// accurate UI.
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

    // Store / read / delete a secret keyed by (userId, key); userId scopes
    // secrets to one account.
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

    // True when the last readSecret() failed because the backend could not
    // answer (locked collection, dropped session bus), as opposed to no such
    // secret. isAvailable() is only probed at construction, so this is the only
    // way to tell "token unreadable" from "no account" (CLAUDE.md §6). Default
    // false.
    virtual bool lastReadFailed() const { return false; }

    /// Whether a miss is inconclusive by construction: true for the whole life
    /// of a store standing in for a native backend it could not open.
    /// Destructive decisions key on this structural fact, not on
    /// lastReadFailed(), which can report individual reads as trustworthy.
    virtual bool missesAreInconclusive() const { return false; }
};
