#pragma once

#include <QString>
#include <QStringView>

#include <cstddef>

namespace lightning::update {

struct TrustedUpdateKey {
    const char *keyId;
    const char *base64Ed25519PublicKey;
    bool retired;
};

class TrustStore
{
public:
    virtual ~TrustStore();
    // Base64 of the raw 32-byte public key for a USABLE key id. Returns a
    // null QString for unknown, retired, or not-compiled-in keys — the
    // caller must treat that as a verification failure, never as "skip".
    virtual QString publicKeyForId(QStringView keyId) const = 0;
};

class CompiledTrustStore final : public TrustStore
{
public:
    QString publicKeyForId(QStringView keyId) const override;
};

const TrustedUpdateKey *findKey(QStringView keyId);

const TrustedUpdateKey *trustedKeyTable(std::size_t *count);

bool hasUsableTrustedKey();

}
