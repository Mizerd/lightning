#include "calls/CallFrameCryptor.h"

#include <QRandomGenerator>
#include <cstring>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

namespace {
/// LiveKit's salt string; changing it breaks interoperability.
constexpr char kSalt[] = "LKFrameEncryptionKey";
/// AES-128-GCM: 16-byte key, 12-byte IV, 16-byte tag.
constexpr int kKeyBytes = 16;
// Accepted raw key lengths: element-call mints 16 bytes (matrix-js-sdk
// RTCEncryptionManager), livekit-client 32; HKDF derives the same AES-128 key
// from either. A closed set, so a malformed key cannot derive and make
// encryptionActive() true under material no peer has.
constexpr int kRawKeyBytesElement = 16;
constexpr int kRawKeyBytesLivekit = 32;

bool isSupportedRawKeyLength(int size)
{
    return size == kRawKeyBytesElement || size == kRawKeyBytesLivekit;
}
constexpr int kIvBytes = 12;
constexpr int kTagBytes = 16;
/// { IV length, key index }.
constexpr int kTrailerBytes = 2;
/// HKDF `info` is 128 zero bytes in the reference (`new ArrayBuffer(128)`),
/// not empty; an empty info derives a different key.
constexpr int kInfoBytes = 128;

/// Best-effort zeroing before release (the allocator may already have
/// copied).
void scrub(QByteArray &key)
{
    if (key.isEmpty())
        return;
    volatile char *raw = key.data();
    for (int i = 0; i < key.size(); ++i)
        raw[i] = 0;
}

void writeBigEndian32(unsigned char *out, quint32 value)
{
    out[0] = static_cast<unsigned char>((value >> 24) & 0xFF);
    out[1] = static_cast<unsigned char>((value >> 16) & 0xFF);
    out[2] = static_cast<unsigned char>((value >> 8) & 0xFF);
    out[3] = static_cast<unsigned char>(value & 0xFF);
}
} // namespace

int CallFrameCryptor::headerBytes(FrameKind kind)
{
    switch (kind) {
    case FrameKind::Audio:      return 1;
    case FrameKind::VideoKey:   return 10;
    case FrameKind::VideoDelta: return 3;
    }
    return 1;
}

bool CallFrameCryptor::looksEncrypted(const char *wire, qsizetype size,
                                      FrameKind kind)
{
    if (!wire)
        return false;
    // The same checks decryptFrame() makes before indexing, derived from the
    // same constants.
    const qsizetype floorBytes =
        headerBytes(kind) + kTagBytes + kIvBytes + kTrailerBytes;
    if (size < floorBytes)
        return false;
    const int ivLength = static_cast<unsigned char>(wire[size - 2]);
    if (ivLength != kIvBytes)
        return false;
    // With a 256-entry ring every byte is a legal index, so the IV-length byte
    // is effectively the whole test; kept so reader and writer share one
    // constant.
    const int keyIndex = static_cast<unsigned char>(wire[size - 1]);
    return keyIndex >= 0 && keyIndex < kKeyRingSize;
}

bool CallFrameCryptor::endsWithServerTrailer(const char *wire, qsizetype size,
                                             const QByteArray &trailer)
{
    // Not armed, or armed with something no accepted SFU would send.
    if (!wire || trailer.isEmpty() || trailer.size() > kMaxServerTrailerBytes)
        return false;
    // Strictly longer than the trailer: a frame that is only the trailer
    // carries no payload.
    if (size <= trailer.size())
        return false;
    return std::memcmp(wire + size - trailer.size(), trailer.constData(),
                       static_cast<size_t>(trailer.size()))
        == 0;
}

bool CallFrameCryptor::isUsableServerTrailer(const QByteArray &trailer)
{
    if (trailer.size() < kMinServerTrailerBytes
        || trailer.size() > kMaxServerTrailerBytes)
        return false;
    for (const char c : trailer) {
        const bool base62 = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z');
        if (!base62)
            return false;
    }
    return true;
}

bool CallFrameCryptor::startsWithOpusSilenceFrame(const char *wire,
                                                  qsizetype size)
{
    // livekit-server `OpusSilenceFrame` (pkg/sfu/downtrack.go), as whitelisted
    // by livekit-client's sifPayload.ts: f8 ff fe, then 77 zeros.
    constexpr qsizetype kSilenceBytes = 80;
    if (!wire || size < kSilenceBytes)
        return false;
    const auto *b = reinterpret_cast<const unsigned char *>(wire);
    if (b[0] != 0xf8 || b[1] != 0xff || b[2] != 0xfe)
        return false;
    for (qsizetype i = 3; i < kSilenceBytes; ++i) {
        if (b[i] != 0)
            return false;
    }
    return true;
}

QByteArray CallFrameCryptor::deriveKey(const QByteArray &rawKey)
{
    if (rawKey.isEmpty())
        return {};

    EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (!kdf)
        return {};
    EVP_KDF_CTX *ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx)
        return {};

    // info is 128 zero bytes — see kInfoBytes.
    unsigned char info[kInfoBytes];
    std::memset(info, 0, sizeof(info));

    char digest[] = "SHA256";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_KEY,
            const_cast<char *>(rawKey.constData()),
            static_cast<size_t>(rawKey.size())),
        OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_SALT, const_cast<char *>(kSalt),
            sizeof(kSalt) - 1),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info,
                                          sizeof(info)),
        OSSL_PARAM_construct_end(),
    };

    QByteArray derived(kKeyBytes, Qt::Uninitialized);
    const int rc = EVP_KDF_derive(
        ctx, reinterpret_cast<unsigned char *>(derived.data()),
        static_cast<size_t>(derived.size()), params);
    EVP_KDF_CTX_free(ctx);
    if (rc <= 0)
        return {};
    return derived;
}

CallFrameCryptor::CallFrameCryptor() = default;

bool CallFrameCryptor::setKey(int index, const QByteArray &rawKey)
{
    QMutexLocker lock(&m_mutex);
    // Remote input (a received key names its index): bound it first.
    if (index < 0 || index >= kKeyRingSize)
        return false;
    // Validate the input size, not just the derived one: HKDF accepts any
    // length, so a bogus key would otherwise derive cleanly. See
    // isSupportedRawKeyLength().
    if (!isSupportedRawKeyLength(rawKey.size()))
        return false;
    const QByteArray derived = deriveKey(rawKey);
    if (derived.size() != kKeyBytes)
        return false;
    // Most-recent order, so the cap evicts the oldest.
    m_keyOrder.removeOne(index);
    m_keyOrder.append(index);
    m_keys.insert(index, derived);
    while (m_keys.size() > kMaxKeysPerRing) {
        // Never evict the key our own frames use.
        int victim = -1;
        for (const int candidate : std::as_const(m_keyOrder)) {
            if (candidate != m_currentIndex) {
                victim = candidate;
                break;
            }
        }
        if (victim < 0)
            break;
        m_keyOrder.removeOne(victim);
        QByteArray old = m_keys.take(victim);
        scrub(old);
    }
    return true;
}

bool CallFrameCryptor::hasAnyKey() const
{
    QMutexLocker lock(&m_mutex);
    // Only setKey() inserts, and only usable keys.
    return !m_keys.isEmpty();
}

void CallFrameCryptor::setCurrentKeyIndex(int index)
{
    QMutexLocker lock(&m_mutex);
    if (index >= 0 && index < kKeyRingSize)
        m_currentIndex = index;
}

bool CallFrameCryptor::hasKey(int index) const
{
    QMutexLocker lock(&m_mutex);
    if (index < 0 || index >= kKeyRingSize)
        return false;
    // constFind, never operator[]: a lookup must not insert an empty slot.
    const auto it = m_keys.constFind(index);
    return it != m_keys.cend() && it.value().size() == kKeyBytes;
}

void CallFrameCryptor::clearKeys()
{
    QMutexLocker lock(&m_mutex);
    for (QByteArray &key : m_keys)
        scrub(key);
    m_keys.clear();
    m_keyOrder.clear();
    m_sendCounts.clear();
    m_currentIndex = 0;
}

QByteArray CallFrameCryptor::makeIvForTest(quint32 ssrc, quint32 rtpTimestamp,
                                           quint32 sendCount)
{
    QByteArray iv(kIvBytes, Qt::Uninitialized);
    auto *out = reinterpret_cast<unsigned char *>(iv.data());
    writeBigEndian32(out, ssrc);
    writeBigEndian32(out + 4, rtpTimestamp);
    // The reference computes `timestamp - (sendCount % 0xffff)` in JS doubles
    // truncated to uint32; C++ unsigned wraparound gives the same 32 bits.
    writeBigEndian32(out + 8, rtpTimestamp - (sendCount % 0xffff));
    return iv;
}

QByteArray CallFrameCryptor::ivFor(quint32 ssrc, quint32 rtpTimestamp)
{
    QMutexLocker lock(&m_mutex);
    auto it = m_sendCounts.find(ssrc);
    if (it == m_sendCounts.end()) {
        // Random initial offset, as in the reference.
        it = m_sendCounts.insert(
            ssrc, QRandomGenerator::global()->bounded(0xffff));
    }
    const quint32 count = it.value();
    // Monotonic per SSRC and never reset while a key is in use: IV reuse
    // breaks AES-GCM.
    it.value() = count + 1;
    return makeIvForTest(ssrc, rtpTimestamp, count);
}

void CallFrameCryptor::setSendCounterForTest(quint32 ssrc, quint32 value)
{
    QMutexLocker lock(&m_mutex);
    m_sendCounts.insert(ssrc, value);
}

QByteArray CallFrameCryptor::encryptFrame(const QByteArray &payload,
                                          FrameKind kind, quint32 ssrc,
                                          quint32 rtpTimestamp)
{
    // Held for the whole frame so the ring cannot rotate between choosing the
    // index and using the key; nested hasKey()/ivFor() re-enter the recursive
    // mutex.
    QMutexLocker lock(&m_mutex);
    const int header = headerBytes(kind);
    if (payload.size() < header)
        return {}; // too short for its header: refuse
    if (!hasKey(m_currentIndex))
        return {}; // no key: the caller drops, never sends cleartext

    const QByteArray iv = ivFor(ssrc, rtpTimestamp);
    // A (shared) copy rather than a reference into the hash, which a later
    // insert could invalidate.
    const QByteArray key = m_keys.value(m_currentIndex);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};

    QByteArray out;
    const int plainLen = payload.size() - header;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr,
                                 nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvBytes,
                               nullptr) == 1
        && EVP_EncryptInit_ex(
               ctx, nullptr, nullptr,
               reinterpret_cast<const unsigned char *>(key.constData()),
               reinterpret_cast<const unsigned char *>(iv.constData()))
            == 1;

    if (ok) {
        // The cleartext header is authenticated (AAD) but not encrypted: the
        // SFU can route on it, and tampering fails the tag.
        int aadLen = 0;
        ok = EVP_EncryptUpdate(
                 ctx, nullptr, &aadLen,
                 reinterpret_cast<const unsigned char *>(payload.constData()),
                 header)
            == 1;
    }

    QByteArray cipher(plainLen, Qt::Uninitialized);
    int cipherLen = 0;
    if (ok && plainLen > 0) {
        ok = EVP_EncryptUpdate(
                 ctx, reinterpret_cast<unsigned char *>(cipher.data()),
                 &cipherLen,
                 reinterpret_cast<const unsigned char *>(payload.constData())
                     + header,
                 plainLen)
            == 1;
    }
    int finalLen = 0;
    if (ok) {
        ok = EVP_EncryptFinal_ex(
                 ctx,
                 reinterpret_cast<unsigned char *>(cipher.data()) + cipherLen,
                 &finalLen)
            == 1;
    }
    QByteArray tag(kTagBytes, Qt::Uninitialized);
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes,
                                 tag.data())
            == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    cipher.resize(cipherLen + finalLen);

    // [header][ciphertext][tag][iv][trailer]
    out.reserve(header + cipher.size() + kTagBytes + kIvBytes + kTrailerBytes);
    out.append(payload.constData(), header);
    out.append(cipher);
    out.append(tag);
    out.append(iv);
    out.append(static_cast<char>(kIvBytes));
    out.append(static_cast<char>(m_currentIndex));
    return out;
}

const char *CallFrameCryptor::decryptFailureName(DecryptFailure reason)
{
    switch (reason) {
    case DecryptFailure::None:          return "none";
    case DecryptFailure::ShortWire:     return "short-wire";
    case DecryptFailure::BadIvLength:   return "bad-iv-length";
    case DecryptFailure::NoKeyForIndex: return "no-key-for-index";
    case DecryptFailure::ShortBody:     return "short-body";
    case DecryptFailure::CipherInit:    return "cipher-init";
    case DecryptFailure::AuthTag:       return "auth-tag";
    }
    return "?";
}

QByteArray CallFrameCryptor::decryptFrame(const QByteArray &wire,
                                          FrameKind kind,
                                          DecryptDiagnosis *why)
{
    // Local diagnosis written to `why` on every exit; `fail` is the only
    // non-plaintext exit.
    DecryptDiagnosis diag;
    const auto fail = [&](DecryptFailure reason) {
        diag.reason = reason;
        if (why)
            *why = diag;
        return QByteArray();
    };
    if (why)
        *why = diag;

    QMutexLocker lock(&m_mutex);
    const int header = headerBytes(kind);
    if (wire.size() < header + kTagBytes + kIvBytes + kTrailerBytes)
        return fail(DecryptFailure::ShortWire);

    // IV length and key index come from the remote trailer; validate both
    // before using them as offsets.
    const int ivLength =
        static_cast<unsigned char>(wire.at(wire.size() - 2));
    const int keyIndex =
        static_cast<unsigned char>(wire.at(wire.size() - 1));
    diag.keyIndex = keyIndex;
    if (ivLength != kIvBytes)
        return fail(DecryptFailure::BadIvLength);
    if (!hasKey(keyIndex))
        // Nothing installed at the index this frame names (not a key
        // mismatch). Drop.
        return fail(DecryptFailure::NoKeyForIndex);

    const int suffix = ivLength + kTrailerBytes;
    const int bodyLen = wire.size() - header - suffix;
    if (bodyLen < kTagBytes)
        return fail(DecryptFailure::ShortBody);
    const int cipherLen = bodyLen - kTagBytes;

    const char *base = wire.constData();
    const unsigned char *iv = reinterpret_cast<const unsigned char *>(
        base + wire.size() - suffix);
    const unsigned char *tag = reinterpret_cast<const unsigned char *>(
        base + header + cipherLen);
    const QByteArray key = m_keys.value(keyIndex);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return fail(DecryptFailure::CipherInit);

    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr,
                                 nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvBytes,
                               nullptr) == 1
        && EVP_DecryptInit_ex(
               ctx, nullptr, nullptr,
               reinterpret_cast<const unsigned char *>(key.constData()), iv)
            == 1;
    if (ok) {
        int aadLen = 0;
        ok = EVP_DecryptUpdate(
                 ctx, nullptr, &aadLen,
                 reinterpret_cast<const unsigned char *>(base), header)
            == 1;
    }
    QByteArray plain(cipherLen > 0 ? cipherLen : 0, Qt::Uninitialized);
    int plainLen = 0;
    if (ok && cipherLen > 0) {
        ok = EVP_DecryptUpdate(
                 ctx, reinterpret_cast<unsigned char *>(plain.data()),
                 &plainLen,
                 reinterpret_cast<const unsigned char *>(base + header),
                 cipherLen)
            == 1;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes,
                                 const_cast<unsigned char *>(tag))
            == 1;
    }
    int finalLen = 0;
    if (ok) {
        // Tag verification happens here; a frame that fails is discarded.
        ok = EVP_DecryptFinal_ex(
                 ctx,
                 reinterpret_cast<unsigned char *>(plain.data()) + plainLen,
                 &finalLen)
            == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        // In practice a tag failure: the init calls only fail if OpenSSL
        // itself is broken.
        return fail(DecryptFailure::AuthTag);
    plain.resize(plainLen + finalLen);

    QByteArray out;
    out.reserve(header + plain.size());
    out.append(base, header);
    out.append(plain);
    return out;
}
