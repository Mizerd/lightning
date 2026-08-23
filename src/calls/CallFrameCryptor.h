// LiveKit-compatible media frame encryption (MatrixRTC phase 2).
//
// This is the piece that makes an encrypted Matrix room's CALL actually
// end-to-end encrypted: the SFU forwards our RTP without ever holding a key
// that can read it. It is deliberately a small, self-contained, heavily
// tested unit, because a frame cryptor that is subtly wrong is worse than
// none — it looks encrypted and either interoperates with nobody or, if the
// IV construction is wrong, is cryptographically broken.
//
// ## The format is NOT invented here
//
// Every constant and every byte position below was read out of LiveKit's own
// implementation (`livekit-client` 2.22.0, `src/e2ee/`), which is the same
// format libwebrtc's native FrameCryptor implements and therefore the same
// one Element Call speaks. Do not "simplify" any of it.
//
//   key derivation : HKDF-SHA256(ikm = raw 32-byte key,
//                                salt = "LKFrameEncryptionKey",
//                                info = 128 zero bytes) -> 16 bytes
//                    (AES-128-GCM). Raw keys take the HKDF path;
//                    PBKDF2 is livekit's passphrase path and is NOT used
//                    by MatrixRTC, which distributes raw key bytes.
//
//   frame layout   : [ header (cleartext) ]
//                    [ AES-GCM ciphertext + 16-byte tag ]
//                    [ IV: 12 bytes ]
//                    [ trailer: 2 bytes = { 12, keyIndex } ]
//
//   header size    : audio (Opus TOC byte)      = 1
//                    video VP8 keyframe         = 10
//                    video VP8 delta frame      = 3
//                    The header is left in the clear so the SFU can still
//                    route and detect keyframes without being able to read
//                    the media.
//
//   IV (12 bytes)  : [0..3]  ssrc               (uint32, big endian)
//                    [4..7]  rtp timestamp      (uint32, big endian)
//                    [8..11] timestamp - (sendCount % 0xffff)
//
// ## The IV rule, which is the whole ballgame
//
// AES-GCM catastrophically fails on IV REUSE — repeating an (key, IV) pair
// leaks the authentication key, not just one frame. The counter is therefore
// per-SSRC and monotonic, seeded at a random offset exactly as the reference
// does, and `encryptFrame` REFUSES rather than reusing a counter it cannot
// advance. Nothing here ever reconstructs an IV from remote input: the
// receiver reads the IV off the wire, which is what it is transmitted for.
//
// Nothing in this file logs a key, a derived key, an IV, or frame contents.
#pragma once

#include <cstdint>

#include <QByteArray>
#include <QHash>

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

    /// Derive the AES-128-GCM key LiveKit uses from raw key material.
    ///
    /// Returns an empty QByteArray on failure; a caller that gets one MUST
    /// refuse to send or receive rather than continue in the clear.
    static QByteArray deriveKey(const QByteArray &rawKey);

    CallFrameCryptor();

    /// Install a derived key at one of the 16 ring slots. `index` is what
    /// travels in the frame trailer, so a receiver can still decrypt frames
    /// that were in flight when the key rotated.
    bool setKey(int index, const QByteArray &rawKey);
    /// Which index newly encrypted frames are stamped with.
    void setCurrentKeyIndex(int index);
    int currentKeyIndex() const { return m_currentIndex; }
    bool hasKey(int index) const;
    /// Forget every key. Called on leave: media keys must not outlive the
    /// call that used them.
    void clearKeys();

    /// Encrypt one RTP payload in place-of, returning the wire form.
    ///
    /// Returns an empty QByteArray if there is no usable key or the payload
    /// is too short for its header — never a partially-processed frame, and
    /// never the cleartext.
    QByteArray encryptFrame(const QByteArray &payload, FrameKind kind,
                            quint32 ssrc, quint32 rtpTimestamp);

    /// Decrypt one wire-form payload. Empty means "could not decrypt": a
    /// wrong key, a truncated frame, or a failed authentication tag. The
    /// caller must DROP the frame; there is no cleartext fallback.
    QByteArray decryptFrame(const QByteArray &wire, FrameKind kind);

    /// Test seam: pin the per-SSRC counter so a known-answer test can assert
    /// an exact IV. Production seeds it randomly.
    void setSendCounterForTest(quint32 ssrc, quint32 value);
    /// The IV a given (ssrc, timestamp, counter) produces — exposed so the
    /// construction itself can be asserted rather than inferred.
    static QByteArray makeIvForTest(quint32 ssrc, quint32 rtpTimestamp,
                                    quint32 sendCount);

private:
    QByteArray ivFor(quint32 ssrc, quint32 rtpTimestamp);

    /// 16 slots, matching LiveKit's key ring size.
    QByteArray m_keys[16];
    int m_currentIndex = 0;
    QHash<quint32, quint32> m_sendCounts;
};
