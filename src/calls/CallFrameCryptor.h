// LiveKit-compatible media frame encryption: the SFU forwards our RTP without
// holding a key that can read it. A subtly wrong cryptor is worse than none,
// so this unit is small and heavily tested.
//
// ## The format is not invented here
//
// Every constant and byte position comes from livekit-client 2.22.0
// (`src/e2ee/`), the same format as libwebrtc's FrameCryptor and therefore
// Element Call. Do not "simplify" it.
//
//   key derivation : HKDF-SHA256(ikm = raw key (16 bytes from element-call,
//                    32 from livekit-client), salt = "LKFrameEncryptionKey",
//                    info = 128 zero bytes) -> 16 bytes (AES-128-GCM).
//                    PBKDF2 is livekit's passphrase path and is not used by
//                    MatrixRTC.
//
//   frame layout   : [ header (cleartext) ]
//                    [ AES-GCM ciphertext + 16-byte tag ]
//                    [ IV: 12 bytes ]
//                    [ trailer: 2 bytes = { 12, keyIndex } ]
//
//   header size    : audio (Opus TOC byte)      = 1
//                    video VP8 keyframe         = 10
//                    video VP8 delta frame      = 3
//                    Left in the clear so the SFU can route and detect
//                    keyframes without reading the media.
//
//   IV (12 bytes)  : [0..3]  ssrc               (uint32, big endian)
//                    [4..7]  rtp timestamp      (uint32, big endian)
//                    [8..11] timestamp - (sendCount % 0xffff)
//
// ## The IV rule
//
// Reusing a (key, IV) pair breaks AES-GCM entirely (it leaks the
// authentication key). The counter is per-SSRC and monotonic, seeded at a
// random offset as in the reference, and encryptFrame() refuses rather than
// reuse a counter. Receivers read the IV off the wire; nothing reconstructs
// one from remote input.
//
// Nothing here logs a key, a derived key, an IV or frame contents.
#pragma once

#include <cstdint>

#include <QByteArray>
#include <QRecursiveMutex>
#include <QHash>
#include <QList>

class CallFrameCryptor
{
public:
    /// Which unencrypted-header rule applies to a frame.
    enum class FrameKind {
        Audio,        ///< Opus: 1 cleartext byte (the TOC).
        VideoKey,     ///< VP8 keyframe: 10 cleartext bytes.
        VideoDelta,   ///< VP8 delta frame: 3 cleartext bytes.
    };

    /// Number of cleartext header bytes for a frame kind.
    static int headerBytes(FrameKind kind);

    /// Derive LiveKit's AES-128-GCM key from raw key material. Empty on
    /// failure; the caller must then refuse to send or receive rather than
    /// continue in the clear.
    static QByteArray deriveKey(const QByteArray &rawKey);

    CallFrameCryptor();

    /// Key indices a ring addresses: 256, the whole trailer byte. MatrixRTC
    /// rotates key ids modulo 256 (matrix-js-sdk
    /// `RTCEncryptionManager.nextKeyIndex`; element-call uses
    /// `keyringSize: 256`), so livekit-client's default of 16 would reject
    /// Element senders after sixteen rotations.
    ///
    /// The ring is sparse (a hash by index), and a re-sent index replaces, so
    /// it never exceeds 256 entries. Our own sender wraps at 16, which any
    /// ring of at least 16 accepts.
    static constexpr int kKeyRingSize = 256;
    /// At most this many keys per ring; the oldest is evicted. Senders only
    /// encrypt under their newest key, so 32 covers frames in flight across
    /// rotations while bounding what an index-spraying peer can make us hold.
    static constexpr int kMaxKeysPerRing = 32;

    /// Install a derived key at `index` (0..kKeyRingSize-1). The index travels
    /// in the frame trailer, so frames in flight across a rotation still
    /// decrypt.
    bool setKey(int index, const QByteArray &rawKey);
    /// Which index newly encrypted frames are stamped with.
    void setCurrentKeyIndex(int index);
    int currentKeyIndex() const { return m_currentIndex; }
    bool hasKey(int index) const;
    /// Does this ring hold any key? Lets the receive path report "no key"
    /// per sender instead of consulting an engine-wide flag.
    bool hasAnyKey() const;
    /// Forget every key; media keys must not outlive their call.
    void clearKeys();

    /// Encrypt one encoded frame, returning the wire form. Empty if there is
    /// no usable key or the payload is shorter than its header: never a
    /// partial frame and never the cleartext.
    QByteArray encryptFrame(const QByteArray &payload, FrameKind kind,
                            quint32 ssrc, quint32 rtpTimestamp);

    /// Why a frame did not decrypt. The causes point to different problems:
    /// a missing key is distribution, a tag mismatch is key agreement, a short
    /// wire is neither.
    enum class DecryptFailure {
        None,           ///< Decrypted.
        ShortWire,      ///< Fewer bytes than header + tag + iv + trailer.
        BadIvLength,    ///< The trailer's IV length is not the one we write.
        NoKeyForIndex,  ///< No key installed at the index the frame names.
        ShortBody,      ///< Body too short to hold the tag.
        CipherInit,     ///< OpenSSL refused to start.
        AuthTag,        ///< GCM tag mismatch: wrong key, or a tampered frame.
    };
    /// The failure, and the key index the frame named when one was readable.
    struct DecryptDiagnosis {
        DecryptFailure reason = DecryptFailure::None;
        int keyIndex = -1;
    };
    /// A one-word name for a failure, for a log line.
    static const char *decryptFailureName(DecryptFailure reason);

    /// Decrypt one wire-form frame. Empty means it could not be decrypted and
    /// must be dropped; there is no cleartext fallback. `why` reports which
    /// failure occurred.
    QByteArray decryptFrame(const QByteArray &wire, FrameKind kind,
                            DecryptDiagnosis *why = nullptr);

    /// Does this wire form carry our trailer? A structural test, not a proof.
    ///
    /// Used when a call is not encrypted for us and a frame passes through in
    /// the clear, to detect a peer that is in fact encrypting. Checks exactly
    /// what decryptFrame() checks before indexing (length, IV-length byte,
    /// key index in range), from the same constants.
    ///
    /// Never treat it as a decryption: a cleartext frame whose second-last
    /// byte is 12 passes (about 1 in 256 for uniform bytes, and real payloads
    /// are not uniform). Only a windowed rate over many frames is meaningful.
    static bool looksEncrypted(const char *wire, qsizetype size,
                               FrameKind kind);
    static bool looksEncrypted(const QByteArray &wire, FrameKind kind)
    { return looksEncrypted(wire.constData(), wire.size(), kind); }

    /// The longest server-injected-frame trailer this client arms.
    /// livekit-server's is base62 of 32 random bytes (~43 bytes); it comes
    /// from the SFU in `JoinResponse.sif_trailer`, so it is bounded (also
    /// enforced in rust/src/sfu.rs and SfuMediaEngine).
    static constexpr int kMaxServerTrailerBytes = 64;
    /// The shortest trailer that arms. A short one could match real frames.
    static constexpr int kMinServerTrailerBytes = 16;
    /// A trailer of LiveKit's shape: kMin..kMax bytes, all [0-9A-Za-z].
    static bool isUsableServerTrailer(const QByteArray &trailer);

    /// Does this frame end with the SFU's server-injected-frame trailer?
    ///
    /// livekit-server writes its own unencrypted blank frames into encrypted
    /// tracks (Opus silence on mute or track close, a VP8 8x8 keyframe) and
    /// marks them with a per-room trailer (`JoinResponse.sif_trailer`), which
    /// livekit-client checks before the crypto trailer
    /// (`isFrameServerInjected`).
    ///
    /// An exact match of the whole trailer; empty means not armed. A real
    /// encrypted frame cannot match: its second-last byte is 12, not base62.
    /// The SFU is outside the E2EE trust boundary, so a match only justifies
    /// dropping the frame without counting a decryption failure, never
    /// decoding it.
    static bool endsWithServerTrailer(const char *wire, qsizetype size,
                                      const QByteArray &trailer);
    static bool endsWithServerTrailer(const QByteArray &wire,
                                      const QByteArray &trailer)
    { return endsWithServerTrailer(wire.constData(), wire.size(), trailer); }

    /// Diagnostic only: does `wire` start with LiveKit's fixed 80-byte Opus
    /// silence frame (`f8 ff fe` then 77 zeros)? Labels a failed frame in a
    /// log line; never a decision.
    static bool startsWithOpusSilenceFrame(const char *wire, qsizetype size);

    /// Test seam: pin the per-SSRC counter so a known-answer test can assert
    /// an exact IV. Production seeds it randomly.
    void setSendCounterForTest(quint32 ssrc, quint32 value);
    /// The IV a given (ssrc, timestamp, counter) produces, so the construction
    /// itself can be asserted.
    static QByteArray makeIvForTest(quint32 ssrc, quint32 rtpTimestamp,
                                    quint32 sendCount);

private:
    QByteArray ivFor(quint32 ssrc, quint32 rtpTimestamp);

    /// Guards the key ring and the per-SSRC counters. Required: audio and
    /// video probes call in concurrently from their own streaming threads,
    /// and a racy counter could give two frames the same IV under the same
    /// key, a total AES-GCM break. Recursive so a nested caller cannot
    /// deadlock; sections are short and never block.
    mutable QRecursiveMutex m_mutex;
    /// Index -> derived key. Sparse; at most kKeyRingSize entries because
    /// every writer validates the index.
    QHash<int, QByteArray> m_keys;
    /// m_keys' indices, oldest first, for the kMaxKeysPerRing eviction.
    QList<int> m_keyOrder;
    int m_currentIndex = 0;
    QHash<quint32, quint32> m_sendCounts;
};
