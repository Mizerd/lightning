#pragma once

#include <cstdint>

#include <QByteArray>
#include <QRecursiveMutex>
#include <QHash>
#include <QList>

class CallFrameCryptor
{
public:
    enum class FrameKind {
        Audio,
        VideoKey,
        VideoDelta,
        VideoH264,
    };

    static int h264ClearBytes(const char *frame, qsizetype size);

    static int headerBytes(FrameKind kind);

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
    static constexpr int kMaxKeysPerRing = 32;

    bool setKey(int index, const QByteArray &rawKey);
    void setCurrentKeyIndex(int index);
    int currentKeyIndex() const { return m_currentIndex; }
    bool hasKey(int index) const;
    bool hasAnyKey() const;
    /// Forget every key; media keys must not outlive their call.
    void clearKeys();

    QByteArray encryptFrame(const QByteArray &payload, FrameKind kind,
                            quint32 ssrc, quint32 rtpTimestamp);

    enum class DecryptFailure {
        None,
        ShortWire,
        BadIvLength,
        NoKeyForIndex,
        ShortBody,
        CipherInit,
        AuthTag,
    };
    struct DecryptDiagnosis {
        DecryptFailure reason = DecryptFailure::None;
        int keyIndex = -1;
    };
    static const char *decryptFailureName(DecryptFailure reason);

    QByteArray decryptFrame(const QByteArray &wire, FrameKind kind,
                            DecryptDiagnosis *why = nullptr);

    static bool looksEncrypted(const char *wire, qsizetype size,
                               FrameKind kind);
    static bool looksEncrypted(const QByteArray &wire, FrameKind kind)
    { return looksEncrypted(wire.constData(), wire.size(), kind); }

    static constexpr int kMaxServerTrailerBytes = 64;
    static constexpr int kMinServerTrailerBytes = 16;
    static bool isUsableServerTrailer(const QByteArray &trailer);

    static bool endsWithServerTrailer(const char *wire, qsizetype size,
                                      const QByteArray &trailer);
    static bool endsWithServerTrailer(const QByteArray &wire,
                                      const QByteArray &trailer)
    { return endsWithServerTrailer(wire.constData(), wire.size(), trailer); }

    static bool startsWithOpusSilenceFrame(const char *wire, qsizetype size);

    void setSendCounterForTest(quint32 ssrc, quint32 value);
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
    QHash<int, QByteArray> m_keys;
    QList<int> m_keyOrder;
    int m_currentIndex = 0;
    QHash<quint32, quint32> m_sendCounts;
};
