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
//   key derivation : HKDF-SHA256(ikm = raw key, 16 or 32 bytes — element-call
//                    sends 16, livekit-client 32; see CallFrameCryptor.cpp,
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
#include <QRecursiveMutex>
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
    /// Does this ring hold ANY key?
    ///
    /// Exists because the receive path used to ask an engine-wide flag —
    /// "some sender's key has arrived" — which is true for every track the
    /// moment one participant is keyed. A joiner whose key never arrived was
    /// then routed down the "decryption failed" branch instead of the "no
    /// key" one, and the log said the wrong thing about the one failure it
    /// most needed to name. Same drop either way; a truthful reason.
    bool hasAnyKey() const;
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

    /// WHY A FRAME DID NOT DECRYPT, because "empty" is six different faults.
    ///
    /// An empty return used to be the whole answer, and the six causes below
    /// send whoever reads the log to six different places: a key that never
    /// arrived is a DISTRIBUTION problem, a tag mismatch is a key AGREEMENT
    /// problem, and a short wire is neither. A live call was diagnosed as
    /// "frames in flight across a key rotation" on nothing more than an empty
    /// return, which is a hypothesis wearing a measurement's clothes.
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

    /// Decrypt one wire-form payload. Empty means "could not decrypt": a
    /// wrong key, a truncated frame, or a failed authentication tag. The
    /// caller must DROP the frame; there is no cleartext fallback. Pass
    /// `why` to find out which of the six it was.
    QByteArray decryptFrame(const QByteArray &wire, FrameKind kind,
                            DecryptDiagnosis *why = nullptr);

    /// DOES THIS WIRE FORM CARRY OUR TRAILER? A STRUCTURAL TEST, NOT A PROOF.
    ///
    /// Exists for one question the receive probe could not previously ask:
    /// when a call is NOT encrypted for us and no key has been installed, a
    /// frame is passed through in the clear — and a peer that IS encrypting
    /// then feeds ciphertext straight into the decoder while every counter in
    /// the engine reports healthy media. Cleartext and ciphertext are the
    /// same bytes to everything downstream; the only thing that separates
    /// them here is the two-byte trailer this class writes.
    ///
    /// It checks exactly what `decryptFrame` checks before it indexes:
    /// enough bytes for header + tag + IV + trailer, an IV-length byte equal
    /// to the one we write, and a key index inside the ring. Derived from the
    /// same constants as the writer so the two cannot drift.
    ///
    /// IT IS NOT A DECRYPTION AND IT MUST NEVER BE READ AS ONE. A cleartext
    /// frame whose last two bytes happen to be 12 and a value under 16
    /// passes — roughly 1 frame in 4096 for uniformly distributed bytes, and
    /// VP8/Opus payloads are not uniform, so the real rate is unknown and
    /// could be much higher for a given encoder. The ONLY sound use is a
    /// WINDOWED verdict over many frames: a peer that is encrypting produces
    /// 100%, a peer that is not produces a trickle. A per-frame branch on
    /// this would be a coin flip presented as a fact.
    static bool looksEncrypted(const char *wire, qsizetype size,
                               FrameKind kind);
    static bool looksEncrypted(const QByteArray &wire, FrameKind kind)
    { return looksEncrypted(wire.constData(), wire.size(), kind); }

    /// Test seam: pin the per-SSRC counter so a known-answer test can assert
    /// an exact IV. Production seeds it randomly.
    void setSendCounterForTest(quint32 ssrc, quint32 value);
    /// The IV a given (ssrc, timestamp, counter) produces — exposed so the
    /// construction itself can be asserted rather than inferred.
    static QByteArray makeIvForTest(quint32 ssrc, quint32 rtpTimestamp,
                                    quint32 sendCount);

private:
    QByteArray ivFor(quint32 ssrc, quint32 rtpTimestamp);

    /// Guards the key ring AND the per-SSRC counters.
    ///
    /// Not optional. One cryptor serves every track in a direction, and
    /// GStreamer runs each track on its OWN streaming thread: the audio and
    /// video pad probes call in concurrently. Unsynchronised, the counter
    /// QHash is a data race, and a torn read of it could hand two frames
    /// the same IV under the same key — which for AES-GCM is not a
    /// degradation, it is a total break.
    ///
    /// Recursive so a future caller inside a locked section cannot deadlock
    /// on itself; the sections are a few hundred bytes of AES and never
    /// block.
    mutable QRecursiveMutex m_mutex;
    /// 16 slots, matching LiveKit's key ring size.
    QByteArray m_keys[16];
    int m_currentIndex = 0;
    QHash<quint32, quint32> m_sendCounts;
};
