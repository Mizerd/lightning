#pragma once

#include "storage/SecretStore.h"

#include <QByteArray>
#include <QHash>
#include <QString>

// File-backed SecretStore for portable installations
// (lightning::portable::isPortable()).
//
// Security position: the key lives beside the ciphertext
// (<dataRoot>/secrets/secrets.key and secrets.dat) because a portable folder
// must work when copied to another machine. Possession of the complete
// directory therefore grants the saved Matrix session and device. This
// protects against casual inspection and indexing, not against theft of the
// folder, exactly like the SDK crypto store next to it.
//
// Do not bind the key to the machine (DPAPI, TPM, machine-id KDF, Credential
// Manager): that breaks the portable contract, and is what WinCredStore does.
// Stronger protection would need a user passphrase, as a separate feature.
// isSecure() is false, and nothing here may imply the folder is safe to share.
//
// Implementation:
//   * One AES-256-GCM sealed JSON document holds every (userId, key) pair, so
//     no user id appears in a file name.
//   * OpenSSL 3 EVP, the libcrypto the update verifier already requires. No
//     Matrix cryptography: Olm/Megolm stay in the Rust SDK's store.
//   * A fresh 96-bit nonce on every write, and a fixed context string as AAD.
//     The AAD holds no path or machine identifier, so the directory can be
//     moved or renamed. No absolute path is written to disk.
//   * Files and the secrets directory are owner-only where the filesystem
//     supports it; FAT32/exFAT cannot enforce this.
//
// Failure semantics (CLAUDE.md §6): an unreadable secret is never reported as
// absent. If the document exists but cannot be opened, parsed or
// authenticated, the store fails:
//   * isAvailable() is false and lastError() explains why;
//   * readSecret() returns empty with lastReadFailed() true, so
//     SettingsManager::secretBackendUnavailable() reports "cannot tell"
//     rather than "no saved sign-in";
//   * writes and deletes refuse rather than start a fresh document over the
//     damaged one, which would silently discard the old device.
// A missing document is an ordinary answer (a fresh folder). A zero-byte
// secrets.dat or secrets.key is truncation, not absence: minting a new key
// would orphan the sealed document forever. Both refuse and preserve the
// bytes so the folder can be repaired from a copy.
class PortableSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    // Directory holding the sealed document, normally dataRoot() + "/secrets".
    // Passed in so tests are hermetic and relocation is testable.
    explicit PortableSecretStore(const QString &secretsDir,
                                 QObject *parent = nullptr);
    ~PortableSecretStore() override;

    // Always false; see the security position above.
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

    // File names this store owns, for diagnostics and tests.
    static QString dataFileName();
    static QString keyFileName();
    QString directory() const { return m_dir; }

private:
    enum class State { Ok, Failed };

    // Reads and authenticates the document, or starts empty when none exists.
    void load();

    // Seals and atomically rewrites the document. Refuses while failed.
    bool persist();

    // Loads or mints the key file, never touching the data file on failure, so
    // the pair cannot become unopenable.
    bool ensureKey();

    static QString mapKey(const QString &userId, const QString &key);
    void setError(const QString &error) const { m_lastError = error; }

    const QString m_dir;
    State m_state = State::Ok;

    // Raw 32-byte AES-256 key; cleansed in the destructor.
    QByteArray m_key;

    // (userId \x1f key) -> value, as in InMemorySecretStore. Held decrypted for
    // the process lifetime like every backend; never logged or exposed to QML.
    QHash<QString, QString> m_secrets;

    mutable bool m_lastReadFailed = false;
    mutable QString m_lastError;
};
