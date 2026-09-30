#pragma once

#include <QByteArray>
#include <QString>

class SecretStore;

// The per-account key of the encrypted SDK media store (rust/src/mediastore.rs).
//
// 32 random bytes, kept through the account's SecretStore beside its tokens:
// the same backend, the same install scope, and so the same deletion.
// clearAccountSecrets() (sign-out, account removal) takes it with them.
// Stored as 64 hex characters. Never logged, never passed to QML, never in
// diagnostics.
//
// The rule that matters (CLAUDE.md §6): a key is created only when the record
// is PROVABLY absent: the read succeeded, returned nothing, and the store can
// vouch for its misses. A locked or unreachable keyring, a fallback standing
// in for one, or a malformed value all give no key, and the media store is
// kept in memory for the session. Replacing a key that exists but cannot be
// read would orphan the encrypted store for good; not having one for a
// session only costs a re-download.
//
// A second witness guards against a keyring that answers "nothing" when it
// means "locked" (a dismissed unlock prompt): the account's own access token
// must read back from the same store, in the same scope, before its missing
// media key counts as absent. A saved account always has one there; if it
// does not, the store is not answering for this account and no key is made.
namespace matrix::media_store_key {

inline constexpr char kSecretName[] = "mediaStoreKey";
inline constexpr int kKeyBytes = 32;
// SettingsManager's name for the access token (kSecretAccessToken).
inline constexpr char kAccessTokenName[] = "accessToken";

enum class Found { Present, Absent, Unreadable };

struct Resolution {
    Found found = Found::Unreadable;
    // True when this call created the key and the store confirmed it.
    bool created = false;
    // kKeyBytes bytes when usable, else empty. The caller scrubs it.
    QByteArray key;
};

// Read without writing anything.
Resolution read(const SecretStore *store, const QString &userId);

// Read, and create and store a key only when the record is provably absent
// and `mayCreate`. The caller passes false while SettingsManager reports the
// backend unavailable or its misses inconclusive (the keyring-outage contract),
// so an existing key is still used but none is ever made then. A key that
// could not be stored, or read back, is not returned.
//
// The store is a parameter, never kept: SettingsManager::setSecretStore() can
// replace it at runtime ("Try again" after an outage).
Resolution resolve(SecretStore *store, const QString &userId,
                   bool mayCreate = true);

// For a log line: "present", "created", "absent", "unreadable". Names the
// state only; the key has no representation here.
const char *describe(const Resolution &resolution);

} // namespace matrix::media_store_key
