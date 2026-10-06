#pragma once

#include "storage/SecretStore.h"

// macOS Keychain backend (Security.framework SecItem* C API, generic
// passwords in the user's default keychain).
//
// Implemented only with HAVE_MAC_KEYCHAIN (set for APPLE in CMakeLists.txt);
// other platforms get a stub that reports unavailable.
//
// Items are kSecClassGenericPassword with service "Lightning/secret/<userId>"
// and account "<key>". The user id is public; the token lives only in the
// protected password data, never in the service or account names or in a log.
// Because the service names the account, clearAccountSecrets() is one
// SecItemDelete over that service.
//
// NOT live-tested on macOS: the dev environment has no macOS host. An ad-hoc
// signed bundle changes code identity on every release, so macOS may ask the
// user to allow access to items written by an earlier build; a refusal
// surfaces as lastReadFailed() ("cannot tell"), never as "no saved sign-in"
// (CLAUDE.md §6).
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
