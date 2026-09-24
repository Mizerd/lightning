#pragma once

#include "storage/SecretStore.h"

#include <QSettings>
#include <memory>

// QSettings-backed fallback used only when no native secure store is
// reachable. Secrets are plaintext under the "secrets/*" group, and
// isSecure() is always false so the UI warns.
//
// Substituted mode: when a native backend is compiled in but did not answer
// (locked keyring, no session bus), this store stands in for it. It still
// reads and writes normally, but an empty read must not be taken as "no saved
// sign-in" (CLAUDE.md §6): the secrets may be in the native store, and that
// conclusion would arm the destructive local reset. See lastReadFailed().
//
// Precondition: an account's group is written and removed as a whole
// (SettingsManager::saveSession writes all keys; clearAccountSecrets()
// removes the group). Deleting a single key of a live account would make a
// miss for it look conclusive.
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
    /// Structural, and unlike lastReadFailed() it never softens: whatever a
    /// given read concluded, a store standing in for a native backend it
    /// could not open may always be missing something that backend holds.
    bool missesAreInconclusive() const override
    {
        return m_substitutedForNative;
    }

    // Three cases:
    //   - A hit is never inconclusive. (Reporting failure whenever substituted
    //     left keyring-less machines permanently "unable to read" sign-ins and
    //     hid genuinely expired ones.)
    //   - Under substitution, a miss for an account this store has never held
    //     anything for is inconclusive: the native store may hold it.
    //   - A miss for an account with other secrets here is a fact about it.
    // A broken backing file (QSettings status() error) outranks everything.
    bool lastReadFailed() const override
    {
        if (m_lastReadFailed)
            return true;              // the file itself could not be read
        if (!m_substitutedForNative)
            return false;             // this store IS the store; a miss is a fact
        if (m_lastReadFound)
            return false;             // a value in hand is never ambiguous
        return !m_lastReadUserHasRecord;   // a miss: conclusive only if we know the account
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
    /// `secrets/<escaped user id>`: one account's group. The single copy of the
    /// escaping.
    static QString accountGroup(const QString &userId);
    QString settingsKey(const QString &userId, const QString &key) const;
    /// True when this store holds at least one secret for `userId`.
    bool hasAnySecretFor(const QString &userId) const;

    std::unique_ptr<QSettings> m_store;
    mutable QString m_lastError;
    /// Whether the last readSecret() returned a value. Used in substituted mode
    /// only; false before any read.
    mutable bool m_lastReadFound = false;
    /// Whether the last read's account has any secret here. Consulted only for
    /// a miss in substituted mode.
    mutable bool m_lastReadUserHasRecord = false;
    // The last read's outcome from QSettings' own status.
    mutable bool m_lastReadFailed = false;
    bool m_substitutedForNative = false;
};
