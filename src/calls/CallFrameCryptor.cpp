#include "calls/CallFrameCryptor.h"

#include <atomic>

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
    case FrameKind::VideoH264:  return 5; // the shortest start code + 2
    }
    return 1;
}

int CallFrameCryptor::h264ClearBytes(const char *frame, qsizetype size)
{
    if (!frame)
        return -1;
    const auto *data = reinterpret_cast<const unsigned char *>(frame);
    for (qsizetype i = 0; i + 3 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        const int type = data[i + 3] & 0x1f;
        if (type == 1 || type == 5)
            return i + 3 + 2 <= size ? static_cast<int>(i + 3 + 2) : -1;
    }
    return -1;
}

namespace {
QByteArray writeRbsp(const QByteArray &in);
QByteArray parseRbsp(const QByteArray &in);

struct BitReader {
    const QByteArray &data;
    qsizetype bit = 0;
    bool ok = true;
    unsigned u1()
    {
        if (bit >= data.size() * 8) {
            ok = false;
            return 0;
        }
        const unsigned value =
            (static_cast<unsigned char>(data.at(bit / 8)) >> (7 - bit % 8)) & 1;
        ++bit;
        return value;
    }
    unsigned ue()
    {
        int zeros = 0;
        while (ok && zeros < 32 && u1() == 0)
            ++zeros;
        unsigned value = 0;
        for (int i = 0; i < zeros; ++i)
            value = (value << 1) | u1();
        return (zeros < 32 ? (1u << zeros) - 1 : 0) + value;
    }
};

struct BitWriter {
    QByteArray out;
    int bits = 0;
    void u1(unsigned value)
    {
        if (bits % 8 == 0)
            out.append(char(0));
        if (value)
            out[out.size() - 1] = char(static_cast<unsigned char>(out.back())
                                       | (0x80 >> (bits % 8)));
        ++bits;
    }
    void ue(unsigned value)
    {
        const unsigned code = value + 1;
        int width = 0;
        while ((code >> (width + 1)) != 0)
            ++width;
        for (int i = 0; i < width; ++i)
            u1(0);
        for (int i = width; i >= 0; --i)
            u1((code >> i) & 1);
    }
};

/// `sps` (NAL header included) with its VUI replaced by the one libwebrtc would
/// write, or `sps` as it is when it cannot be read.
///
/// A receiving libwebrtc rewrites any SPS without a bitstream restriction, to
/// stop the decoder buffering, BEFORE the frame reaches the decryptor
/// (SpsVuiRewriter, Direction::kIncoming). The SPS is inside the authenticated
/// header, so the rewrite fails every tag. A browser's own sender has already
/// rewritten it (Direction::kOutgoing); this is that step. The other VUI fields
/// are dropped rather than parsed: timing means nothing to a call, and a
/// full-range signal would be lost, which the ladder's encoders do not send
/// for the limited-range NV12 and RGBA input they get.
QByteArray spsWithBitstreamRestriction(const QByteArray &sps)
{
    if (sps.size() < 4)
        return sps;
    const QByteArray rbsp = parseRbsp(sps.sliced(1));
    BitReader in{rbsp};
    in.bit = 24; // profile_idc, constraints, level_idc
    const int profile = static_cast<unsigned char>(rbsp.at(0));
    in.ue(); // seq_parameter_set_id
    // Only what a Constrained Baseline encoder can produce.
    if (profile != 66 && profile != 77)
        return sps;
    in.ue(); // log2_max_frame_num_minus4
    const unsigned pocType = in.ue();
    if (pocType == 0)
        in.ue();
    else if (pocType != 2)
        return sps;
    const unsigned maxRefFrames = in.ue();
    in.u1(); // gaps_in_frame_num_value_allowed_flag
    in.ue(); // pic_width_in_mbs_minus1
    in.ue(); // pic_height_in_map_units_minus1
    if (in.u1() == 0) // frame_mbs_only_flag
        in.u1();
    in.u1(); // direct_8x8_inference_flag
    if (in.u1()) { // frame_cropping_flag
        for (int i = 0; i < 4; ++i)
            in.ue();
    }
    if (!in.ok)
        return sps;
    BitWriter out;
    BitReader copy{rbsp};
    for (qsizetype i = 0; i < in.bit; ++i)
        out.u1(copy.u1());
    out.u1(1); // vui_parameters_present_flag
    for (int i = 0; i < 4; ++i)
        out.u1(0); // aspect ratio, overscan, video signal, chroma location
    for (int i = 0; i < 4; ++i)
        out.u1(0); // timing, NAL HRD, VCL HRD, pic_struct
    out.u1(1);     // bitstream_restriction_flag
    out.u1(1);     // motion_vectors_over_pic_boundaries_flag
    out.ue(0);     // max_bytes_per_pic_denom
    out.ue(0);     // max_bits_per_mb_denom
    out.ue(16);    // log2_max_mv_length_horizontal
    out.ue(16);    // log2_max_mv_length_vertical
    out.ue(0);     // max_num_reorder_frames
    out.ue(maxRefFrames); // max_dec_frame_buffering
    out.u1(1);     // rbsp_stop_one_bit
    return sps.first(1) + writeRbsp(out.out);
}

/// The frame as a receiver will rebuild it, up to `clear`: four-byte start
/// codes, because a depacketizer writes those, and an SPS it would not rewrite.
/// The clear header is authenticated, so any difference fails every tag.
QByteArray withReceiverHeader(const QByteArray &frame, int clear)
{
    const auto byteAt = [&frame](int at) {
        return static_cast<unsigned char>(frame.at(at));
    };
    QByteArray out;
    out.reserve(frame.size() + 32);
    int i = 0;
    while (i < clear) {
        // Any run of two or more zeros before a 1 is a start code; the extra
        // zeros are `trailing_zero_8bits`, which no packetizer carries.
        int one = i;
        while (one < clear && byteAt(one) == 0)
            ++one;
        if (one + 1 >= clear || one - i < 2 || byteAt(one) != 1) {
            out.append(frame.at(i));
            ++i;
            continue;
        }
        out.append(QByteArray::fromRawData("\0\0\0\1", 4));
        const int nal = one + 1;
        if ((byteAt(nal) & 0x1f) != 7) {
            i = nal;
            continue;
        }
        int end = nal;
        while (end + 2 < clear
               && !(byteAt(end) == 0 && byteAt(end + 1) == 0
                    && byteAt(end + 2) <= 1))
            ++end;
        const QByteArray sps = frame.sliced(nal, end - nal);
        const QByteArray rewritten = spsWithBitstreamRestriction(sps);
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (rewritten == sps && !warned.test_and_set()) {
            qWarning("call media: an H.264 SPS could not be rewritten; a "
                     "browser will fail to decrypt this stream's keyframes");
        }
        out.append(rewritten);
        i = end;
    }
    out.append(frame.constData() + clear, frame.size() - clear);
    return out;
}

// libwebrtc's and livekit-client's writeRbsp()/parseRbsp(), byte for byte.
QByteArray writeRbsp(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size() + in.size() / 64 + 4);
    int zeros = 0;
    for (const char c : in) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 3 && zeros >= 2) {
            out.append(char(3));
            zeros = 0;
        }
        out.append(c);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

QByteArray parseRbsp(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());
    const qsizetype size = in.size();
    for (qsizetype i = 0; i < size;) {
        if (size - i >= 3 && in.at(i) == 0 && in.at(i + 1) == 0
            && in.at(i + 2) == 3) {
            out.append(in.at(i));
            out.append(in.at(i + 1));
            i += 3;
        } else {
            out.append(in.at(i));
            ++i;
        }
    }
    return out;
}
} // namespace

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

QByteArray CallFrameCryptor::encryptFrame(const QByteArray &frame,
                                          FrameKind kind, quint32 ssrc,
                                          quint32 rtpTimestamp)
{
    // Held for the whole frame so the ring cannot rotate between choosing the
    // index and using the key; nested hasKey()/ivFor() re-enter the recursive
    // mutex.
    QMutexLocker lock(&m_mutex);
    const bool h264 = kind == FrameKind::VideoH264;
    const int clear = h264 ? h264ClearBytes(frame.constData(), frame.size())
                           : headerBytes(kind);
    if (clear < 0 || frame.size() < clear)
        return {}; // no slice, or too short for its header: refuse
    const QByteArray payload = h264 ? withReceiverHeader(frame, clear) : frame;
    const int header = h264 ? h264ClearBytes(payload.constData(), payload.size())
                            : clear;
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
    if (h264) {
        QByteArray body;
        body.reserve(cipher.size() + kTagBytes + kIvBytes + kTrailerBytes);
        body.append(cipher);
        body.append(tag);
        body.append(iv);
        body.append(static_cast<char>(kIvBytes));
        body.append(static_cast<char>(m_currentIndex));
        out.append(writeRbsp(body));
        return out;
    }
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

QByteArray CallFrameCryptor::decryptFrame(const QByteArray &received,
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
    const bool h264 = kind == FrameKind::VideoH264;
    const int header = h264
        ? h264ClearBytes(received.constData(), received.size())
        : headerBytes(kind);
    if (header < 0)
        return fail(DecryptFailure::ShortWire);
    // Everything below reads the frame as it was before RBSP escaping.
    const QByteArray wire = h264
        ? received.first(header) + parseRbsp(received.sliced(header))
        : received;
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
