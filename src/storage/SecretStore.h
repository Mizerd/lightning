#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <memory>

// Abstract secret storage. Backends include:
//   * LibSecretStore: Secret Service via libsecret (HAVE_LIBSECRET);
//     isAvailable() checks that the session bus is actually reachable. Shared
//     between installs, so its items are scoped (see setInstallScope()).
//   * WinCredStore: Windows Credential Manager (HAVE_WINCRED).
//   * MacKeychainStore: macOS Keychain (HAVE_MAC_KEYCHAIN).
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
    // secrets to one account. A shared store also scopes them to this install.
    virtual bool storeSecret(const QString &userId,
                             const QString &key,
                             const QString &value) = 0;
    virtual QString readSecret(const QString &userId,
                               const QString &key) const = 0;
    virtual bool deleteSecret(const QString &userId, const QString &key) = 0;

    // Remove every secret bound to a user id (in a shared store, this
    // install's only). Used by logout.
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

    // Install scoping. The Secret Service is one per desktop session, but the
    // Flatpak, the snap and a native build each keep their own account records
    // and SDK stores, so each must keep its own tokens. A store shared between
    // installs keys every item by installScope() too; SettingsManager sets it
    // from the id it keeps beside the account records, before any other call.
    // Stores that live inside one install (Credential Manager, portable file,
    // QSettings fallback) ignore it.
    virtual bool isSharedBetweenInstalls() const { return false; }

    // Which of its storage backends answers in this process, for a store that
    // has more than one: libsecret keeps a Flatpak's or snap's items in a
    // sandbox-private FILE keyring whenever the Secret portal answers, and in
    // the Secret Service when it does not, and the choice is made per process.
    // "file" or "service"; empty for a store with one backend (every other
    // store), which is never recorded or compared. An account records the kind
    // its sign-in was last written under (backendKindFor()), and such a store
    // reads it there (TwoBackendSecretStore), so a launch landing on the other
    // backend never reads a saved sign-in as gone.
    virtual QString backendKind() const { return {}; }

    // Where this process reads and writes this account's items: backendKind(),
    // unless the store reaches the account's recorded backend past its own
    // (TwoBackendSecretStore). What an account records after a write.
    virtual QString backendKindFor(const QString &userId) const
    {
        Q_UNUSED(userId);
        return backendKind();
    }

    // The backendKind() each account's sign-in was last written under, as the
    // owner of the account records keeps it; empty when unknown. Asked on every
    // use, never cached, so a record written since is what the next read sees.
    using RecordedBackendLookup = std::function<QString(const QString &userId)>;
    void setRecordedBackendLookup(RecordedBackendLookup lookup)
    {
        m_recordedBackend = std::move(lookup);
    }
    QString recordedBackendKind(const QString &userId) const
    {
        return m_recordedBackend ? m_recordedBackend(userId) : QString();
    }
    void setInstallScope(const QString &scope) { m_installScope = scope; }
    QString installScope() const { return m_installScope; }

    // Items written before install scoping existed, which every install could
    // read and overwrite. Shared stores only. Another install may still be
    // using them: delete one only when it provably holds this install's own
    // token (SettingsManager::clearSessionForAccount). Same lastReadFailed()
    // contract as readSecret().
    virtual QString readLegacySecret(const QString &userId,
                                     const QString &key) const
    {
        Q_UNUSED(userId);
        Q_UNUSED(key);
        return {};
    }
    virtual bool deleteLegacySecret(const QString &userId, const QString &key)
    {
        Q_UNUSED(userId);
        Q_UNUSED(key);
        return true;
    }

private:
    QString m_installScope;
    RecordedBackendLookup m_recordedBackend;
};
