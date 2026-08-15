#pragma once

#include <QString>
#include <QStringView>

#include <cstddef>

// Lightning secure update system — the compiled-in trusted signing keys.
//
// The remote side can NEVER introduce a key. Adding or rotating a key means
// shipping a new Lightning build: this table is the entire root of trust.
//
// Rotation works because several keys may be trusted at once — publish a
// release signed by key A while key B is already trusted, start signing with
// B, then mark A `retired` in a later build. A retired key is REJECTED; it
// stays in the table only as documentation of what was withdrawn.
//
// Only PUBLIC keys ever appear here. A private key must never be committed,
// embedded, or logged.
namespace lightning::update {

struct TrustedUpdateKey {
    const char *keyId;
    const char *base64Ed25519PublicKey; // raw 32-byte Ed25519 public key, base64
    bool retired;
};

// Lookup interface, so tests can supply a key generated at runtime without
// touching the compiled-in table.
class TrustStore
{
public:
    virtual ~TrustStore();
    // Base64 of the raw 32-byte public key for a USABLE key id. Returns a
    // null QString for unknown, retired, or not-compiled-in keys — the
    // caller must treat that as a verification failure, never as "skip".
    virtual QString publicKeyForId(QStringView keyId) const = 0;
};

// The compiled-in table.
class CompiledTrustStore final : public TrustStore
{
public:
    QString publicKeyForId(QStringView keyId) const override;
};

// Direct access to the table. Returns nullptr for an unknown key id, a
// retired key, or an entry whose public key was not compiled into this
// build (fail closed: with no key material, verification always fails).
const TrustedUpdateKey *findKey(QStringView keyId);

// Whole table, including retired and not-compiled-in entries (diagnostics).
const TrustedUpdateKey *trustedKeyTable(std::size_t *count);

// False when this build has no usable signing key at all. Update checks
// must then report an honest "updates are not available in this build"
// rather than attempting an unverifiable download.
bool hasUsableTrustedKey();

} // namespace lightning::update
