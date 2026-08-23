#include "calls/CallFrameCryptor.h"

#include <QRandomGenerator>
#include <cstring>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

namespace {
/// LiveKit's own salt string. Changing it makes us incompatible with every
/// other client; it is not a tunable.
constexpr char kSalt[] = "LKFrameEncryptionKey";
/// AES-128-GCM: 16-byte key, 12-byte IV, 16-byte tag.
constexpr int kKeyBytes = 16;
/// The RAW key both ends of the protocol agree on, before HKDF.
constexpr int kRawKeyBytes = 32;
constexpr int kIvBytes = 12;
constexpr int kTagBytes = 16;
/// { IV length, key index }.
constexpr int kTrailerBytes = 2;
/// HKDF `info` is 128 ZERO bytes in the reference (`new ArrayBuffer(128)`),
/// not an empty info. An empty info derives a different key and would fail
/// to interoperate while looking perfectly reasonable.
constexpr int kInfoBytes = 128;

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
    if (index < 0 || index >= 16)
        return false;
    // The INPUT size, not just the derived one. HKDF turns any length into
    // 16 bytes, so validating only the output accepted a 7-byte key: it
    // derived cleanly, `encryptionActive()` then reported true, and every
    // frame went out under a key no other participant could possibly have.
    // Both ends of this protocol agree on 32 raw bytes — a different length
    // is a bug or a hostile sender, never a key to use.
    if (rawKey.size() != kRawKeyBytes)
        return false;
    const QByteArray derived = deriveKey(rawKey);
    if (derived.size() != kKeyBytes)
        return false;
    m_keys[index] = derived;
    return true;
}

void CallFrameCryptor::setCurrentKeyIndex(int index)
{
    QMutexLocker lock(&m_mutex);
    if (index >= 0 && index < 16)
        m_currentIndex = index;
}

bool CallFrameCryptor::hasKey(int index) const
{
    QMutexLocker lock(&m_mutex);
    return index >= 0 && index < 16 && m_keys[index].size() == kKeyBytes;
}

void CallFrameCryptor::clearKeys()
{
    QMutexLocker lock(&m_mutex);
    for (QByteArray &key : m_keys) {
        // Best-effort scrub before release. Not a guarantee (the allocator
        // may already have copied), but the same transit hygiene the UIA
        // password path applies.
        if (!key.isEmpty()) {
            volatile char *raw = key.data();
            for (int i = 0; i < key.size(); ++i)
                raw[i] = 0;
        }
        key.clear();
    }
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
    // The reference computes `timestamp - (sendCount % 0xffff)` in JS, where
    // the subtraction happens on doubles and is then truncated to uint32 by
    // DataView.setUint32. Unsigned wraparound in C++ produces the same 32-bit
    // result for every input, including when sendCount exceeds timestamp.
    writeBigEndian32(out + 8, rtpTimestamp - (sendCount % 0xffff));
    return iv;
}

QByteArray CallFrameCryptor::ivFor(quint32 ssrc, quint32 rtpTimestamp)
{
    QMutexLocker lock(&m_mutex);
    auto it = m_sendCounts.find(ssrc);
    if (it == m_sendCounts.end()) {
        // Seeded at a random offset, as the reference does, so two calls on
        // the same SSRC do not start from the same counter.
        it = m_sendCounts.insert(
            ssrc, QRandomGenerator::global()->bounded(0xffff));
    }
    const quint32 count = it.value();
    // Monotonic per SSRC. AES-GCM IV reuse is a total break, so this must
    // never be reset while a key is in use.
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
    // Held across the WHOLE frame: the key ring must not be rotated out
    // from under a frame between choosing the index and using the key, and
    // the nested hasKey()/ivFor() calls re-enter this same recursive mutex.
    QMutexLocker lock(&m_mutex);
    const int header = headerBytes(kind);
    if (payload.size() < header)
        return {}; // too short to carry its own header: refuse, never guess
    if (!hasKey(m_currentIndex))
        return {}; // no key: the caller must drop, never send cleartext

    const QByteArray iv = ivFor(ssrc, rtpTimestamp);
    const QByteArray &key = m_keys[m_currentIndex];

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
        // The cleartext header is AUTHENTICATED (AAD) but not encrypted, so
        // an SFU can still route on it while a tampered header fails the tag.
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

QByteArray CallFrameCryptor::decryptFrame(const QByteArray &wire,
                                          FrameKind kind)
{
    QMutexLocker lock(&m_mutex);
    const int header = headerBytes(kind);
    if (wire.size() < header + kTagBytes + kIvBytes + kTrailerBytes)
        return {};

    // The trailer names the IV length and the key index. Both are remote
    // input, so both are validated before being used as offsets.
    const int ivLength =
        static_cast<unsigned char>(wire.at(wire.size() - 2));
    const int keyIndex =
        static_cast<unsigned char>(wire.at(wire.size() - 1));
    if (ivLength != kIvBytes)
        return {};
    if (!hasKey(keyIndex))
        return {}; // unknown key: drop, never render cleartext

    const int suffix = ivLength + kTrailerBytes;
    const int bodyLen = wire.size() - header - suffix;
    if (bodyLen < kTagBytes)
        return {};
    const int cipherLen = bodyLen - kTagBytes;

    const char *base = wire.constData();
    const unsigned char *iv = reinterpret_cast<const unsigned char *>(
        base + wire.size() - suffix);
    const unsigned char *tag = reinterpret_cast<const unsigned char *>(
        base + header + cipherLen);
    const QByteArray &key = m_keys[keyIndex];

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};

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
        // A failed tag lands HERE. This is the whole point: a frame that
        // does not authenticate is discarded, never passed on.
        ok = EVP_DecryptFinal_ex(
                 ctx,
                 reinterpret_cast<unsigned char *>(plain.data()) + plainLen,
                 &finalLen)
            == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    plain.resize(plainLen + finalLen);

    QByteArray out;
    out.reserve(header + plain.size());
    out.append(base, header);
    out.append(plain);
    return out;
}
