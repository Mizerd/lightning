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
// is not locked out; it only declines to vouch for what it CANNOT know. Three
// outcomes, not one — see lastReadFailed(): a value it just returned is
// trusted, a miss for an account it already holds secrets for is a fact about
// that account, and only a miss for an account it has never held anything for
// is reported as untrustworthy. That last case is the honest answer when the
// store the secrets are actually in could not be opened, and it is what keeps
// the destructive path shut.
//
// THE PRECONDITION THAT MAKES THE MIDDLE CASE SOUND, stated because nothing
// asserts it: an account's group is written and removed as a WHOLE.
// SettingsManager::saveSession writes all of its keys together (empty values
// included, and QSettings creates a key for an empty value), and
// clearAccountSecrets() removes the entire group. If a future caller ever
// deletes ONE key of a live account, a miss for that key would become
// "conclusive" while the account is still signed in.
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
    // Three states, and the middle one is the whole correction.
    //
    // A HIT IS NEVER INCONCLUSIVE. Substitution says the native store may
    // hold something this one cannot see — which makes a MISS ambiguous, and
    // says nothing whatever about a value already in hand. Returning
    // `m_substitutedForNative || m_lastReadFailed` made this store report
    // failure PERMANENTLY on any build with a native backend compiled in
    // whose daemon is not running: the Linux packages all carry
    // HAVE_LIBSECRET, so a machine with no session bus or no keyring — a
    // server, a minimal WM, a container — reads every token back perfectly
    // from this INI and was told, forever, that its sign-ins could not be
    // read. Two consequences, both live: SettingsManager::
    // secretBackendUnavailable() was stuck true, so anything reporting "we
    // can't read your saved sign-ins" said so on every launch of a machine
    // where everything worked; and AccountManager::needsSignIn() short-
    // circuits on it, so a genuinely EXPIRED sign-in could never be reported
    // as needing one.
    //
    // A MISS UNDER SUBSTITUTION IS INCONCLUSIVE ONLY WHILE THIS ACCOUNT IS A
    // STRANGER HERE. §6 is explicit that "no readable access token" is not
    // "no account" — that conflation is what once armed `requireLocalReset`
    // against a real crypto store — so a miss for a user this store has never
    // held anything for stays unknowable: the native store it stood in for
    // may well hold the secret.
    //
    // But if the INI already holds ANY other secret for that same user, this
    // store IS where that account's record lives, and a missing key is then a
    // FACT about it rather than a blind spot. Without that distinction a
    // genuinely expired sign-in on a keyring-less machine could never be
    // reported as expired — the user was told "unlock the keyring" forever,
    // advice that cannot be followed because there is no keyring, and the
    // password login it points at is bounced back as ExistingStoreNeedsRestore.
    //
    // AND THE BACKING FILE CAN BE BROKEN whatever the mode. A truncated INI,
    // a permissions change or a half-written config makes QSettings answer
    // every value() with an empty QVariant and report it only through
    // status(), which readSecret() asks. That outcome outranks everything.
    /// Structural, and unlike lastReadFailed() it never softens: whatever a
    /// given read concluded, a store standing in for a native backend it
    /// could not open may always be missing something that backend holds.
    bool missesAreInconclusive() const override
    {
        return m_substitutedForNative;
    }

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
    /// `secrets/<escaped user id>` — the QSettings group one account's secrets
    /// live under. Shared by the key builder, the record probe and the account
    /// wipe, which each had their own copy of the escaping.
    static QString accountGroup(const QString &userId);
    QString settingsKey(const QString &userId, const QString &key) const;
    /// True when this store holds at least one secret for `userId`.
    bool hasAnySecretFor(const QString &userId) const;

    std::unique_ptr<QSettings> m_store;
    mutable QString m_lastError;
    /// Whether the most recent readSecret() actually returned a value.
    ///
    /// Only consulted in substituted mode, where it is what separates "this
    /// store cannot see what the native one holds" from "this store just
    /// handed you the secret". False before any read: nothing has been
    /// proven yet, which is the honest default for a store standing in for
    /// one that failed.
    mutable bool m_lastReadFound = false;
    /// Whether the account of the most recent readSecret() has ANY secret in
    /// this store. Only a MISS consults it, and only in substituted mode: it
    /// is what separates "this store has never served that account" from
    /// "this store serves that account and does not have that key".
    mutable bool m_lastReadUserHasRecord = false;
    // The outcome of the most recent readSecret(), taken from the backing
    // QSettings' own status rather than assumed. Mutable because reading is
    // const and the outcome of a read is exactly what this records.
    mutable bool m_lastReadFailed = false;
    bool m_substitutedForNative = false;
};
