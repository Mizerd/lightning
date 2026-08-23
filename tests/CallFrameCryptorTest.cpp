// LiveKit-compatible frame encryption (2026-08-23, MatrixRTC phase 2).
//
// This suite exists because a frame cryptor that is subtly wrong is worse
// than none: it looks encrypted, interoperates with nobody, and — if the IV
// construction is wrong — is cryptographically broken rather than merely
// incompatible. So the tests assert the FORMAT against LiveKit's own
// implementation (livekit-client 2.22.0, src/e2ee/), not merely that a
// round trip works.
#include "calls/CallFrameCryptor.h"

#include <QSet>
#include <QtTest/QtTest>

namespace {
QByteArray rawKey(char fill = 'k')
{
    // MatrixRTC distributes 32 raw bytes.
    return QByteArray(32, fill);
}
} // namespace

class CallFrameCryptorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void headerSizesMatchTheReference()
    {
        // Read from livekit-client's UNENCRYPTED_BYTES. The SFU relies on
        // these staying in the clear to route and to detect keyframes; a
        // wrong value corrupts every frame in one direction only, which is
        // exactly the kind of bug that looks like a network problem.
        QCOMPARE(CallFrameCryptor::headerBytes(
                     CallFrameCryptor::FrameKind::Audio), 1);
        QCOMPARE(CallFrameCryptor::headerBytes(
                     CallFrameCryptor::FrameKind::VideoKey), 10);
        QCOMPARE(CallFrameCryptor::headerBytes(
                     CallFrameCryptor::FrameKind::VideoDelta), 3);
    }

    void keyDerivationIsStableAndKeyDependent()
    {
        const QByteArray a = CallFrameCryptor::deriveKey(rawKey('a'));
        const QByteArray b = CallFrameCryptor::deriveKey(rawKey('b'));
        // AES-128-GCM.
        QCOMPARE(a.size(), 16);
        QCOMPARE(b.size(), 16);
        // Deterministic: two participants deriving from the same shared key
        // must reach the same bytes or nobody can decrypt anybody.
        QCOMPARE(CallFrameCryptor::deriveKey(rawKey('a')), a);
        QVERIFY(a != b);
        // An empty key must not silently produce a usable one.
        QVERIFY(CallFrameCryptor::deriveKey(QByteArray()).isEmpty());
    }

    void keyDerivationMatchesAnIndependentHkdf()
    {
        // KNOWN ANSWER, cross-checked against a from-scratch RFC 5869
        // HKDF-SHA256 (not against this implementation): ikm = 32 * 'k',
        // salt = "LKFrameEncryptionKey", info = 128 zero bytes, L = 16.
        //
        // This is what proves we derive the SAME key Element does. A test
        // that only round-trips against itself would pass just as happily
        // with the wrong salt, the wrong info length, or PBKDF2 — all of
        // which produce a perfectly self-consistent cryptor that no other
        // client can talk to.
        QCOMPARE(CallFrameCryptor::deriveKey(rawKey('k')).toHex(),
                 QByteArray("262178a9e5dabf73df9342ed5bae9fe1"));
    }

    void ivLayoutIsExactlyTheReferenceConstruction()
    {
        // [0..3] ssrc, [4..7] timestamp, [8..11] timestamp - (count % 0xffff),
        // all big endian. Pinned byte by byte because this is the field an
        // implementation is most likely to get "nearly" right.
        const QByteArray iv =
            CallFrameCryptor::makeIvForTest(0x01020304u, 0x0A0B0C0Du, 5);
        QCOMPARE(iv.size(), 12);
        QCOMPARE(static_cast<quint8>(iv.at(0)), quint8(0x01));
        QCOMPARE(static_cast<quint8>(iv.at(1)), quint8(0x02));
        QCOMPARE(static_cast<quint8>(iv.at(2)), quint8(0x03));
        QCOMPARE(static_cast<quint8>(iv.at(3)), quint8(0x04));
        QCOMPARE(static_cast<quint8>(iv.at(4)), quint8(0x0A));
        QCOMPARE(static_cast<quint8>(iv.at(5)), quint8(0x0B));
        QCOMPARE(static_cast<quint8>(iv.at(6)), quint8(0x0C));
        QCOMPARE(static_cast<quint8>(iv.at(7)), quint8(0x0D));
        // 0x0A0B0C0D - 5
        QCOMPARE(static_cast<quint8>(iv.at(8)), quint8(0x0A));
        QCOMPARE(static_cast<quint8>(iv.at(9)), quint8(0x0B));
        QCOMPARE(static_cast<quint8>(iv.at(10)), quint8(0x0C));
        QCOMPARE(static_cast<quint8>(iv.at(11)), quint8(0x08));
    }

    void ivSubtractionWrapsLikeTheReference()
    {
        // The reference computes this in JS and truncates through
        // DataView.setUint32, so a counter larger than the timestamp wraps
        // rather than clamping. Unsigned arithmetic must match.
        const QByteArray iv = CallFrameCryptor::makeIvForTest(0, 1, 5);
        QCOMPARE(static_cast<quint8>(iv.at(8)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(9)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(10)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(11)), quint8(0xFC));
    }

    void ivIsNeverReusedForTheSameSsrc()
    {
        // THE critical property. AES-GCM IV reuse leaks the authentication
        // key — it is a total break, not a single-frame problem. Encrypting
        // many frames on one SSRC at an unchanging timestamp (the worst
        // case) must still produce distinct IVs.
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));
        QSet<QByteArray> seen;
        for (int i = 0; i < 512; ++i) {
            const QByteArray wire = cryptor.encryptFrame(
                QByteArray("\x01payload"), CallFrameCryptor::FrameKind::Audio,
                /*ssrc=*/42, /*rtpTimestamp=*/1000);
            QVERIFY(!wire.isEmpty());
            // IV sits just before the 2-byte trailer.
            seen.insert(wire.mid(wire.size() - 14, 12));
        }
        QCOMPARE(seen.size(), 512);
    }

    void encryptedFrameHasTheReferenceWireLayout()
    {
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(3, rawKey()));
        cryptor.setCurrentKeyIndex(3);
        const QByteArray payload("\x01" "hello world");
        const QByteArray wire = cryptor.encryptFrame(
            payload, CallFrameCryptor::FrameKind::Audio, 7, 99);
        QVERIFY(!wire.isEmpty());

        // header(1) + ciphertext(payload-1) + tag(16) + iv(12) + trailer(2)
        QCOMPARE(wire.size(), payload.size() + 16 + 12 + 2);
        // The Opus TOC byte stays in the clear so the SFU can route it.
        QCOMPARE(wire.at(0), payload.at(0));
        // Trailer: { IV length, key index }.
        QCOMPARE(static_cast<quint8>(wire.at(wire.size() - 2)), quint8(12));
        QCOMPARE(static_cast<quint8>(wire.at(wire.size() - 1)), quint8(3));
        // The body must NOT be the cleartext.
        QVERIFY(!wire.contains(QByteArray("hello world")));
    }

    void roundTripRecoversTheExactPayload()
    {
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        QVERIFY(sender.setKey(0, rawKey()));
        QVERIFY(receiver.setKey(0, rawKey()));

        for (auto kind : {CallFrameCryptor::FrameKind::Audio,
                          CallFrameCryptor::FrameKind::VideoKey,
                          CallFrameCryptor::FrameKind::VideoDelta}) {
            QByteArray payload(64, 'x');
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = static_cast<char>(i);
            const QByteArray wire =
                sender.encryptFrame(payload, kind, 1, 2);
            QVERIFY(!wire.isEmpty());
            QCOMPARE(receiver.decryptFrame(wire, kind), payload);
        }
    }

    void aTamperedFrameFailsAuthentication()
    {
        // The whole point of GCM: the SFU forwards our bytes but cannot
        // alter them undetected.
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        QVERIFY(sender.setKey(0, rawKey()));
        QVERIFY(receiver.setKey(0, rawKey()));
        QByteArray wire = sender.encryptFrame(
            QByteArray("\x01secret"), CallFrameCryptor::FrameKind::Audio, 1, 2);
        QVERIFY(!wire.isEmpty());

        QByteArray flipped = wire;
        flipped[3] = static_cast<char>(flipped.at(3) ^ 0x01);
        QVERIFY2(receiver.decryptFrame(
                     flipped, CallFrameCryptor::FrameKind::Audio).isEmpty(),
                 "a flipped ciphertext byte must fail the tag");

        // Tampering with the CLEARTEXT header must fail too: it is
        // authenticated as AAD even though it is not encrypted.
        QByteArray header = wire;
        header[0] = static_cast<char>(header.at(0) ^ 0xFF);
        QVERIFY2(receiver.decryptFrame(
                     header, CallFrameCryptor::FrameKind::Audio).isEmpty(),
                 "the cleartext header is authenticated and must be covered");
    }

    void aWrongKeyDecryptsToNothing()
    {
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        QVERIFY(sender.setKey(0, rawKey('a')));
        QVERIFY(receiver.setKey(0, rawKey('b')));
        const QByteArray wire = sender.encryptFrame(
            QByteArray("\x01secret"), CallFrameCryptor::FrameKind::Audio, 1, 2);
        QVERIFY(!wire.isEmpty());
        QVERIFY(receiver.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio)
                    .isEmpty());
    }

    void anUnknownKeyIndexIsDroppedNotGuessed()
    {
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        QVERIFY(sender.setKey(5, rawKey()));
        sender.setCurrentKeyIndex(5);
        // The receiver holds the same key material, but at a DIFFERENT slot.
        QVERIFY(receiver.setKey(0, rawKey()));
        const QByteArray wire = sender.encryptFrame(
            QByteArray("\x01secret"), CallFrameCryptor::FrameKind::Audio, 1, 2);
        QVERIFY(!wire.isEmpty());
        // Falling back to "some key we have" would defeat rotation.
        QVERIFY(receiver.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio)
                    .isEmpty());
    }

    void rotationKeepsInFlightFramesDecryptable()
    {
        // A key ring exists precisely so frames already on the wire when the
        // key rotates are not lost.
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        QVERIFY(sender.setKey(0, rawKey('a')));
        QVERIFY(receiver.setKey(0, rawKey('a')));
        const QByteArray old = sender.encryptFrame(
            QByteArray("\x01old"), CallFrameCryptor::FrameKind::Audio, 1, 2);

        QVERIFY(sender.setKey(1, rawKey('b')));
        sender.setCurrentKeyIndex(1);
        QVERIFY(receiver.setKey(1, rawKey('b')));
        const QByteArray fresh = sender.encryptFrame(
            QByteArray("\x01new"), CallFrameCryptor::FrameKind::Audio, 1, 3);

        QCOMPARE(receiver.decryptFrame(old, CallFrameCryptor::FrameKind::Audio),
                 QByteArray("\x01old"));
        QCOMPARE(receiver.decryptFrame(fresh,
                                       CallFrameCryptor::FrameKind::Audio),
                 QByteArray("\x01new"));
    }

    void withoutAKeyNothingIsEmittedInTheClear()
    {
        // The safety property that matters most: no key means NO OUTPUT, not
        // a passthrough. A cleartext fallback would silently un-encrypt an
        // encrypted room's call.
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.encryptFrame(QByteArray("\x01secret"),
                                     CallFrameCryptor::FrameKind::Audio, 1, 2)
                    .isEmpty());
        QVERIFY(cryptor.decryptFrame(QByteArray(64, 'x'),
                                     CallFrameCryptor::FrameKind::Audio)
                    .isEmpty());
    }

    void clearingKeysStopsEncryptionImmediately()
    {
        // Media keys must not outlive the call that used them.
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));
        QVERIFY(!cryptor.encryptFrame(QByteArray("\x01x"),
                                      CallFrameCryptor::FrameKind::Audio, 1, 2)
                     .isEmpty());
        cryptor.clearKeys();
        QVERIFY(!cryptor.hasKey(0));
        QVERIFY(cryptor.encryptFrame(QByteArray("\x01x"),
                                     CallFrameCryptor::FrameKind::Audio, 1, 2)
                    .isEmpty());
    }

    void malformedFramesAreRefusedNotIndexedInto()
    {
        // Every length here is remote input used as an offset.
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));
        for (int size : {0, 1, 5, 14, 20, 28}) {
            const QByteArray junk(size, '\x01');
            QVERIFY2(cryptor.decryptFrame(
                         junk, CallFrameCryptor::FrameKind::Audio).isEmpty(),
                     qPrintable(QStringLiteral("size %1 must be refused")
                                    .arg(size)));
        }
        // A trailer claiming an IV length we do not implement.
        QByteArray wire = cryptor.encryptFrame(
            QByteArray("\x01payload"), CallFrameCryptor::FrameKind::Audio, 1, 2);
        QVERIFY(!wire.isEmpty());
        wire[wire.size() - 2] = static_cast<char>(99);
        QVERIFY(cryptor.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio)
                    .isEmpty());
    }

    void aFrameShorterThanItsHeaderIsRefused()
    {
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));
        // A VP8 keyframe claims 10 cleartext bytes; 4 cannot carry that.
        QVERIFY(cryptor.encryptFrame(QByteArray(4, 'x'),
                                     CallFrameCryptor::FrameKind::VideoKey,
                                     1, 2)
                    .isEmpty());
    }

    void keyIndexIsBoundedToTheRing()
    {
        CallFrameCryptor cryptor;
        QVERIFY(!cryptor.setKey(-1, rawKey()));
        QVERIFY(!cryptor.setKey(16, rawKey()));
        QVERIFY(!cryptor.setKey(9999, rawKey()));
        QVERIFY(cryptor.setKey(15, rawKey()));
    }
};

QTEST_MAIN(CallFrameCryptorTest)
#include "CallFrameCryptorTest.moc"
