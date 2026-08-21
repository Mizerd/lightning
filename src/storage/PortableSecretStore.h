#pragma once

#include "storage/SecretStore.h"

#include <QByteArray>
#include <QHash>
#include <QString>

// File-backed SecretStore for PORTABLE installations (Windows portable ZIP,
// and any other build where lightning::portable::isPortable() is true).
//
// ─────────────────────────────────────────────────────────────────────────
//  THE SECURITY POSITION — read this before changing anything here
// ─────────────────────────────────────────────────────────────────────────
// The encryption key lives in the SAME directory as the ciphertext
// (<dataRoot>/secrets/secrets.key beside <dataRoot>/secrets/secrets.dat),
// because the entire point of a portable installation is that the folder is
// self-contained and can be copied to another machine and keep working.
//
// Therefore: **possession of the complete portable directory grants access to
// the saved Matrix session and device.** This is obfuscation against casual
// inspection — someone opening the folder, or a backup tool indexing it, does
// not read an access token in the clear — it is NOT protection against theft
// or copying of the folder. Anyone who can read the folder can read the
// secrets, exactly as they can read the Rust SDK's crypto store sitting next
// to it.
//
// Do NOT "fix" this by binding the key to the machine (DPAPI, TPM, a
// machine-id-derived KDF, the Credential Manager). Machine binding is exactly
// what WinCredStore already does and exactly why a copied portable folder asks
// for a new login — it would break the portable contract, which is the whole
// reason this class exists. If stronger at-rest protection is ever wanted the
// only honest route is a user-supplied passphrase, and that is a separate,
// explicitly-asked-for feature, not a silent change here.
//
// isSecure() is false for this reason, so every existing surface that warns
// about a non-OS-backed store keeps warning. Nothing in this class, its
// backendName(), or any user-facing string may imply that the folder is safe
// to hand to someone else.
//
// ─────────────────────────────────────────────────────────────────────────
//  What it actually does
// ─────────────────────────────────────────────────────────────────────────
//   * One AES-256-GCM sealed JSON document holds every (userId, key) pair, so
//     no Matrix user id appears in a file NAME (unlike WinCredStore's target
//     names, which are fine inside a per-user credential vault and would be
//     needless metadata leakage in a folder that travels).
//   * AES-256-GCM through OpenSSL 3's EVP API — the same libcrypto the update
//     signature verifier already uses, and already REQUIRED by CMake. No new
//     dependency, no hand-rolled construction, and no Matrix cryptography:
//     Megolm/Olm stay entirely inside the Rust SDK's own store.
//   * A fresh 96-bit nonce on EVERY write (nonce reuse under one key is the
//     one fatal mistake in GCM), and a fixed context string as AAD so the blob
//     cannot be replayed as some other Lightning file.
//   * The AAD deliberately contains NO path and NO machine identifier — that
//     is what lets the directory be renamed, moved to another drive letter, or
//     copied to another PC and still open. Nothing written to disk records an
//     absolute path.
//   * Files are created owner-only (QFileDevice::ReadOwner | WriteOwner), as
//     is the secrets directory. NOTE: FAT32/exFAT — the usual format for a USB
//     stick, which is a completely normal home for a portable install — has no
//     ownership or permission bits at all and CANNOT enforce this. The call is
//     made anyway because it costs nothing on NTFS/ext4, but it must not be
//     described as a guarantee.
//
// ─────────────────────────────────────────────────────────────────────────
//  Failure semantics (CLAUDE.md §6 — this is the load-bearing part)
// ─────────────────────────────────────────────────────────────────────────
// An UNREADABLE secret is never reported as an ABSENT one. If the sealed
// document exists but cannot be opened, parsed, or authenticated — truncated
// file, flipped byte, missing key file — the store enters a failed state in
// which:
//   * isAvailable() is false and lastError() explains what is wrong;
//   * readSecret() returns empty AND lastReadFailed() is true, so
//     SettingsManager::secretBackendUnavailable() reports "cannot tell"
//     rather than letting an empty token read as "this account has no saved
//     sign-in" — the conflation that lets destructive cleanup delete or
//     orphan the wrong crypto store;
//   * storeSecret()/deleteSecret()/clearAccountSecrets() REFUSE. They do not
//     start a fresh document over the top of the damaged one. Preserving the
//     unreadable bytes keeps recovery possible; overwriting them is
//     irreversible data loss, and doing it silently would look like a
//     successful sign-in that has quietly discarded the old device.
// A MISSING document, by contrast, is a real and ordinary answer: a freshly
// extracted portable folder has no secrets yet, and that is not a failure.
//
// ABSENT and EMPTY are therefore kept strictly apart, for both files, and that
// distinction is the sharpest edge in this class. A zero-byte secrets.dat or
// secrets.key is a TRUNCATION — an interrupted copy of the folder, a full
// disk, a filesystem that lost the tail — and reading either as "nothing
// stored yet" is catastrophic: the key file would be minted over, which
// permanently orphans the sealed document beside it, and the whole thing would
// look like an ordinary first run right up until the user notices their Matrix
// device is gone. Both cases refuse and preserve the bytes, so restoring the
// missing half from a copy of the folder still works.
class PortableSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    // `secretsDir` is the directory the sealed document lives in — normally
    // lightning::portable::dataRoot() + "/secrets". It is passed in rather
    // than resolved internally so the unit tests are hermetic and so the
    // relocation property (root A written, root B read) is directly testable.
    explicit PortableSecretStore(const QString &secretsDir,
                                 QObject *parent = nullptr);
    ~PortableSecretStore() override;

    // False, deliberately and permanently — see the security position above.
    bool isSecure() const override { return false; }

    // False only when the on-disk document exists and could not be read.
    bool isAvailable() const override { return m_state == State::Ok; }

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

    // The two files this store owns, for diagnostics and for tests. Neither
    // path is ever written INTO either file.
    static QString dataFileName();
    static QString keyFileName();
    QString directory() const { return m_dir; }

private:
    enum class State { Ok, Failed };

    // Reads and authenticates the sealed document (or establishes an empty
    // one when nothing is stored yet). Sets m_state / m_lastError.
    void load();

    // Seals and atomically rewrites the document from m_secrets. Refuses
    // while m_state is Failed. Returns false and sets m_lastError on failure.
    bool persist();

    // Loads the key file, or mints and writes one when none exists yet.
    // Returns false without touching the data file on any failure, so a
    // half-written pair can never leave the document unopenable.
    bool ensureKey();

    static QString mapKey(const QString &userId, const QString &key);
    void setError(const QString &error) const { m_lastError = error; }

    const QString m_dir;
    State m_state = State::Ok;

    // Raw 32-byte AES-256 key. Empty until a key file is read or minted.
    // Cleansed in the destructor.
    QByteArray m_key;

    // (userId \x1f key) -> value, mirroring InMemorySecretStore's mapping.
    // Held decrypted for the process lifetime, exactly as every other backend
    // holds whatever it last read; never logged, never exposed to QML.
    QHash<QString, QString> m_secrets;

    mutable bool m_lastReadFailed = false;
    mutable QString m_lastError;
};
