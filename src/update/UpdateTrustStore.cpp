#include "update/UpdateTrustStore.h"

#include <QLatin1String>

// The release signing public key is supplied at build time. When it is not
// defined the entry stays EMPTY, which means "no key compiled in" — every
// signature verification then fails closed. There is deliberately no
// fallback, no bundled default, and no way for a downloaded file to add one.
#ifndef LIGHTNING_UPDATE_PUBKEY_2026A
#define LIGHTNING_UPDATE_PUBKEY_2026A ""
#endif

namespace lightning::update {
namespace {

constexpr TrustedUpdateKey kTrustedKeys[] = {
    { "lightning-release-2026a", LIGHTNING_UPDATE_PUBKEY_2026A, false },
};

} // namespace

TrustStore::~TrustStore() = default;

const TrustedUpdateKey *findKey(QStringView keyId)
{
    if (keyId.isEmpty())
        return nullptr;
    for (const TrustedUpdateKey &key : kTrustedKeys) {
        if (keyId != QLatin1String(key.keyId))
            continue;
        if (key.retired)
            return nullptr; // withdrawn: never usable again
        if (!key.base64Ed25519PublicKey || *key.base64Ed25519PublicKey == '\0')
            return nullptr; // not compiled into this build: fail closed
        return &key;
    }
    return nullptr;
}

const TrustedUpdateKey *trustedKeyTable(std::size_t *count)
{
    if (count)
        *count = sizeof(kTrustedKeys) / sizeof(kTrustedKeys[0]);
    return kTrustedKeys;
}

bool hasUsableTrustedKey()
{
    for (const TrustedUpdateKey &key : kTrustedKeys) {
        if (key.retired)
            continue;
        if (key.base64Ed25519PublicKey && *key.base64Ed25519PublicKey != '\0')
            return true;
    }
    return false;
}

QString CompiledTrustStore::publicKeyForId(QStringView keyId) const
{
    if (const TrustedUpdateKey *key = findKey(keyId))
        return QString::fromLatin1(key->base64Ed25519PublicKey);
    return {};
}

} // namespace lightning::update
