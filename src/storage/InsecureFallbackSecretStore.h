#pragma once

#include "storage/SecretStore.h"

#include <QSettings>
#include <memory>

// QSettings-backed fallback used only when no native secure store is
// reachable. Stores secrets in plaintext under a dedicated group so it is
// trivial to audit ("secrets/*"). Always reports insecure so the UI can
// warn the user.
//
// SUBSTITUTED MODE, and why it exists. This store was also being handed back
// when a native backend WAS compiled in and merely failed to answer — a
// locked keyring, no session bus, a dismissed unlock prompt. It then reported
// isAvailable() true and lastReadFailed() false, so `secretBackendUnavailable()`
// said "I can answer" and every empty read was taken as fact. The tokens were
// still sitting in libsecret, so the app concluded the account had no saved
// sign-in, and §6's rule — "never treat 'no readable access token' as 'no
// account'" — was broken at the one place it matters: that conclusion arms
// the destructive local reset, whose cleanup then reports success having
// removed nothing.
//
// In substituted mode this store still READS AND WRITES normally, so the user
// is not locked out; it only stops claiming its answers are authoritative. It
// says a read could not be trusted, which is the honest answer when the store
// the secrets are actually in could not be opened, and it is what keeps the
// destructive path shut.
class InsecureFallbackSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    /// `substitutedForNative` = a native backend was compiled in and probed
    /// unavailable, so this store is standing in for one that may well hold
    /// the user's secrets. Never set it when no native backend exists: there
    /// this store IS the backend and its answers are authoritative.
    explicit InsecureFallbackSecretStore(QObject *parent = nullptr,
                                         bool substitutedForNative = false);

    bool isSecure() const override { return false; }
    bool isAvailable() const override { return true; }
    // Two independent reasons a read here cannot be trusted, and returning
    // only the first was a constant wearing a predicate's clothes.
    //
    // A substituted store cannot see what the native one holds, so every miss
    // is inconclusive rather than a fact. AND this store's own backing file
    // can be unreadable or malformed — a truncated INI, a permissions
    // change, a half-written config — in which case QSettings answers every
    // value() with an empty QVariant and says so only through status(),
    // which readSecret() never asked. An empty token then read as "no saved
    // sign-in", which is precisely the conflation SecretStore.h's own
    // comment exists to prevent, and it routes the user to a destructive
    // reset prompt for what is a config-file problem.
    bool lastReadFailed() const override
    {
        return m_substitutedForNative || m_lastReadFailed;
    }
    QString backendName() const override;

    bool storeSecret(const QString &userId,
                     const QString &key,
                     const QString &value) override;
    QString readSecret(const QString &userId,
                       const QString &key) const override;
    bool deleteSecret(const QString &userId, const QString &key) override;
    bool clearAccountSecrets(const QString &userId) override;

    QString lastError() const override { return m_lastError; }

private:
    QString settingsKey(const QString &userId, const QString &key) const;

    std::unique_ptr<QSettings> m_store;
    mutable QString m_lastError;
    // The outcome of the most recent readSecret(), taken from the backing
    // QSettings' own status rather than assumed. Mutable because reading is
    // const and the outcome of a read is exactly what this records.
    mutable bool m_lastReadFailed = false;
    bool m_substitutedForNative = false;
};
