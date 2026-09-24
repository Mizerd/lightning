// LiveKit-compatible frame encryption. A subtly wrong cryptor looks encrypted,
// interoperates with nobody, and with a bad IV is cryptographically broken, so
// these tests assert the format against livekit-client 2.22.0 (src/e2ee/),
// not merely a round trip.
#include "calls/CallFrameCryptor.h"

#include <QSet>
#include <mutex>
#include <thread>

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
    // element-call mints 16-byte media keys (matrix-js-sdk's
    // RTCEncryptionManager), livekit-client's createE2EEKey() mints 32. HKDF
    // derives the same 16-byte AES-128 key from either, so both are accepted;
    // rejecting 16 would drop every frame from Element peers.
    void bothElementAndLivekitKeyLengthsAreAccepted()
    {
        CallFrameCryptor sixteen;
        QVERIFY2(sixteen.setKey(0, QByteArray(16, 'k')),
                 "a 16-byte element-call key was refused");
        CallFrameCryptor thirtyTwo;
        QVERIFY2(thirtyTwo.setKey(0, QByteArray(32, 'k')),
                 "a 32-byte livekit-client key was refused");
        // Still a closed set: an unused length must not derive and light up
        // encryptionActive().
        CallFrameCryptor odd;
        QVERIFY2(!odd.setKey(0, QByteArray(7, 'k')), "a 7-byte key was accepted");
        QVERIFY2(!odd.setKey(0, QByteArray(24, 'k')), "a 24-byte key was accepted");
        QVERIFY2(!odd.setKey(0, QByteArray()), "an empty key was accepted");
    }

    void headerSizesMatchTheReference()
    {
        // livekit-client's UNENCRYPTED_BYTES: the SFU needs these in the clear
        // to route and detect keyframes. A wrong value breaks one direction
        // only and looks like a network problem.
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
        // Deterministic: participants deriving from one shared key must agree.
        QCOMPARE(CallFrameCryptor::deriveKey(rawKey('a')), a);
        QVERIFY(a != b);
        // An empty key must not produce a usable one.
        QVERIFY(CallFrameCryptor::deriveKey(QByteArray()).isEmpty());
    }

    void keyDerivationMatchesAnIndependentHkdf()
    {
        // Known answer from an independent RFC 5869 HKDF-SHA256: ikm = 32 * 'k',
        // salt = "LKFrameEncryptionKey", info = 128 zero bytes, L = 16. A
        // self round-trip would pass with the wrong salt, info or KDF.
        QCOMPARE(CallFrameCryptor::deriveKey(rawKey('k')).toHex(),
                 QByteArray("262178a9e5dabf73df9342ed5bae9fe1"));
    }

    void ivLayoutIsExactlyTheReferenceConstruction()
    {
        // [0..3] ssrc, [4..7] timestamp, [8..11] timestamp - (count % 0xffff),
        // all big endian, pinned byte by byte.
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
        // The reference truncates through DataView.setUint32, so a counter
        // larger than the timestamp wraps rather than clamping.
        const QByteArray iv = CallFrameCryptor::makeIvForTest(0, 1, 5);
        QCOMPARE(static_cast<quint8>(iv.at(8)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(9)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(10)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(11)), quint8(0xFC));
    }

    void ivIsNeverReusedForTheSameSsrc()
    {
        // AES-GCM IV reuse leaks the authentication key, a total break. Many
        // frames on one SSRC at an unchanging timestamp must still get
        // distinct IVs.
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

    void twoTracksSharingOneCryptorMustNotShareAnIv()
    {
        // Audio and video share one send cryptor on separate streaming threads,
        // and the counter is per SSRC, so different SSRCs must never collide
        // even at the same timestamp.
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));
        QSet<QByteArray> seen;
        for (int i = 0; i < 256; ++i) {
            for (quint32 ssrc : {1u, 2u, 3u}) {
                const QByteArray wire = cryptor.encryptFrame(
                    QByteArray("\x01payload"),
                    CallFrameCryptor::FrameKind::Audio, ssrc,
                    /*rtpTimestamp=*/9000);
                QVERIFY(!wire.isEmpty());
                seen.insert(wire.mid(wire.size() - 14, 12));
            }
        }
        // Every one of the 768 frames got its own IV.
        QCOMPARE(seen.size(), 768);
    }

    void twoThreadsThroughOneCryptorStayCorrect()
    {
        // Drives the engine's shape: audio and video probes reaching one
        // cryptor concurrently. A smoke test only: it does not fail without the
        // mutex (distinct QHash entries rarely race). The lock is justified by
        // the failure mode, a silently duplicated AES-GCM IV.
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));

        // std::thread rather than QtConcurrent: no new Qt module needed.
        std::mutex guard;
        QSet<QByteArray> seen;
        const auto run = [&](quint32 ssrc) {
            for (int i = 0; i < 400; ++i) {
                const QByteArray wire = cryptor.encryptFrame(
                    QByteArray("\x01payload"),
                    CallFrameCryptor::FrameKind::Audio, ssrc,
                    /*rtpTimestamp=*/7000);
                const std::lock_guard<std::mutex> lock(guard);
                seen.insert(wire.mid(wire.size() - 14, 12));
            }
        };
        std::thread a(run, 11u);
        std::thread b(run, 22u);
        a.join();
        b.join();
        QCOMPARE(seen.size(), 800);
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

    // The trailer tells ciphertext from cleartext without a key. With no key
    // installed and encryption not required, SfuMediaEngine's receive probe
    // passes frames through, so a peer encrypting into a call we believe is
    // clear would feed ciphertext to the decoder unnoticed; the trailer is
    // the only distinguishing signal. Its false-positive shape is asserted.
    void theTrailerSaysWhetherAFrameWasEncryptedWithoutDecryptingIt()
    {
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(5, rawKey()));
        cryptor.setCurrentKeyIndex(5);
        const QByteArray payload("\x01" "hello world");
        const QByteArray wire = cryptor.encryptFrame(
            payload, CallFrameCryptor::FrameKind::Audio, 7, 99);
        QVERIFY(!wire.isEmpty());

        // A frame this scheme wrote is recognised without a key.
        QVERIFY2(CallFrameCryptor::looksEncrypted(
                     wire, CallFrameCryptor::FrameKind::Audio),
                 "a frame this class encrypted is not recognised as one");
        CallFrameCryptor empty;
        QVERIFY(!empty.hasAnyKey());

        // Its cleartext is not.
        QVERIFY2(!CallFrameCryptor::looksEncrypted(
                     payload, CallFrameCryptor::FrameKind::Audio),
                 "an ordinary Opus-shaped payload is reported as encrypted");
        // Too short for header + tag + IV + trailer, whatever it ends with.
        QByteArray tiny(8, '\0');
        tiny[6] = char(12);
        tiny[7] = char(3);
        QVERIFY(!CallFrameCryptor::looksEncrypted(
            tiny, CallFrameCryptor::FrameKind::Audio));
        // A VP8 keyframe has a 10-byte cleartext header, so the floor depends
        // on frame kind.
        QByteArray justAudio(1 + 16 + 12 + 2, 'x');
        justAudio[justAudio.size() - 2] = char(12);
        justAudio[justAudio.size() - 1] = char(0);
        QVERIFY(CallFrameCryptor::looksEncrypted(
            justAudio, CallFrameCryptor::FrameKind::Audio));
        QVERIFY2(!CallFrameCryptor::looksEncrypted(
                     justAudio, CallFrameCryptor::FrameKind::VideoKey),
                 "the length floor does not follow the frame kind's header");

        // An IV length we never write is refused (decryptFrame uses it as an
        // offset).
        QByteArray wrongIv = wire;
        wrongIv[wrongIv.size() - 2] = char(16);
        QVERIFY(!CallFrameCryptor::looksEncrypted(
            wrongIv, CallFrameCryptor::FrameKind::Audio));
        // Key index 200 is inside the 256-slot ring (element-call's), so the
        // frame is shaped like ours.
        QByteArray highIndex = wire;
        highIndex[highIndex.size() - 1] = char(200);
        QVERIFY(CallFrameCryptor::looksEncrypted(
            highIndex, CallFrameCryptor::FrameKind::Audio));

        // A shape test, not a decryption: a long cleartext frame whose last two
        // bytes happen to fit passes. That is why the engine takes a windowed
        // verdict; this fails if someone adds a per-frame check that cannot be
        // sound without the key.
        QByteArray unluckyCleartext(64, 'a');
        unluckyCleartext[unluckyCleartext.size() - 2] = char(12);
        unluckyCleartext[unluckyCleartext.size() - 1] = char(4);
        QVERIFY2(CallFrameCryptor::looksEncrypted(
                     unluckyCleartext, CallFrameCryptor::FrameKind::Audio),
                 "this test's own premise is gone: if a per-frame answer is "
                 "now reliable, the engine's sliding window can be dropped "
                 "— and if it is not, this must still pass");
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
        // GCM: the SFU forwards our bytes but cannot alter them undetected.
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

        // The cleartext header is authenticated as AAD, so tampering fails too.
        QByteArray header = wire;
        header[0] = static_cast<char>(header.at(0) ^ 0xFF);
        QVERIFY2(receiver.decryptFrame(
                     header, CallFrameCryptor::FrameKind::Audio).isEmpty(),
                 "the cleartext header is authenticated and must be covered");
    }

    // A ring reports whether it holds any key. An engine-wide "some key
    // arrived" flag is true once any participant is keyed, which would
    // mislabel a missing key as a decryption failure.
    void aRingSaysWhetherItHoldsAnyKeyAtAll()
    {
        CallFrameCryptor ring;
        QVERIFY(!ring.hasAnyKey());
        QVERIFY(ring.setKey(3, QByteArray(32, 'k')));
        QVERIFY(ring.hasAnyKey());
        // A refused key does not make an empty ring claim to be keyed.
        CallFrameCryptor refused;
        QVERIFY(!refused.setKey(0, QByteArray(7, 'k')));
        QVERIFY(!refused.hasAnyKey());
        // clearKeys() forgets the claim: media keys must not outlive the call.
        ring.clearKeys();
        QVERIFY(!ring.hasAnyKey());
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
        // The receiver has the same key material at a different slot.
        QVERIFY(receiver.setKey(0, rawKey()));
        const QByteArray wire = sender.encryptFrame(
            QByteArray("\x01secret"), CallFrameCryptor::FrameKind::Audio, 1, 2);
        QVERIFY(!wire.isEmpty());
        // Falling back to any held key would defeat rotation.
        QVERIFY(receiver.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio)
                    .isEmpty());
    }

    void rotationKeepsInFlightFramesDecryptable()
    {
        // The ring keeps frames already in flight decryptable across rotation.
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
        // No key means no output, never a passthrough, which would silently
        // un-encrypt an encrypted room's call.
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
        QCOMPARE(CallFrameCryptor::kKeyRingSize, 256);
        QVERIFY(!cryptor.setKey(-1, rawKey()));
        QVERIFY(!cryptor.setKey(256, rawKey()));
        QVERIFY(!cryptor.setKey(9999, rawKey()));
        QVERIFY(!cryptor.hasKey(256));
        QVERIFY(!cryptor.hasKey(-1));
        QVERIFY(!cryptor.hasAnyKey());
        QVERIFY(cryptor.setKey(15, rawKey()));
        QVERIFY(cryptor.setKey(16, rawKey()));
        QVERIFY(cryptor.setKey(255, rawKey()));
        // An out-of-ring index cannot become the one frames are stamped with.
        cryptor.setCurrentKeyIndex(255);
        cryptor.setCurrentKeyIndex(256);
        QCOMPARE(cryptor.currentKeyIndex(), 255);
        cryptor.setCurrentKeyIndex(-1);
        QCOMPARE(cryptor.currentKeyIndex(), 255);
    }

    // Key indices above 15 install and decrypt: matrix-js-sdk rotates as
    // `(keyId + 1) % 256` and element-call's ring holds 256.
    void aKeyAtIndex200IsInstalledAndDecryptsThatSendersFrames()
    {
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        QVERIFY2(sender.setKey(200, rawKey('e')),
                 "a key at index 200 was refused by the sender's ring");
        sender.setCurrentKeyIndex(200);
        QCOMPARE(sender.currentKeyIndex(), 200);
        QVERIFY2(receiver.setKey(200, rawKey('e')),
                 "a key at index 200 was refused by the receiver's ring");
        QVERIFY(receiver.hasKey(200));
        QVERIFY(receiver.hasAnyKey());

        const QByteArray payload("\x01" "an element peer, rotated a lot");
        const QByteArray wire = sender.encryptFrame(
            payload, CallFrameCryptor::FrameKind::Audio, 11, 4242);
        QVERIFY(!wire.isEmpty());
        // The trailer names the index, as livekit-client writes it.
        QCOMPARE(static_cast<unsigned char>(wire.at(wire.size() - 1)), 200);
        QCOMPARE(static_cast<unsigned char>(wire.at(wire.size() - 2)), 12);

        CallFrameCryptor::DecryptDiagnosis why;
        QCOMPARE(receiver.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio,
                                       &why),
                 payload);
        QCOMPARE(why.reason, CallFrameCryptor::DecryptFailure::None);

        // A ring keyed only at 199 reports "no key at that index" for 200,
        // not a bad trailer.
        CallFrameCryptor neighbour;
        QVERIFY(neighbour.setKey(199, rawKey('e')));
        QVERIFY(neighbour.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio,
                                       &why)
                    .isEmpty());
        QCOMPARE(why.reason, CallFrameCryptor::DecryptFailure::NoKeyForIndex);
        QCOMPARE(why.keyIndex, 200);
    }

    // A ring holds at most kMaxKeysPerRing keys and evicts the oldest, never
    // the index our own frames are sent under.
    void aRingKeepsOnlyItsMostRecentKeys()
    {
        CallFrameCryptor ring;
        const int cap = CallFrameCryptor::kMaxKeysPerRing;
        QCOMPARE(cap, 32);
        QVERIFY(ring.setKey(0, rawKey()));
        ring.setCurrentKeyIndex(0);
        for (int index = 1; index <= cap + 10; ++index)
            QVERIFY(ring.setKey(index, rawKey()));
        QVERIFY2(ring.hasKey(0), "the current send index was evicted");
        int held = 0;
        for (int index = 0; index < CallFrameCryptor::kKeyRingSize; ++index)
            held += ring.hasKey(index) ? 1 : 0;
        QCOMPARE(held, cap);
        // The oldest non-current indices went; the newest stayed.
        QVERIFY(!ring.hasKey(1));
        QVERIFY(!ring.hasKey(11));
        QVERIFY(ring.hasKey(12));
        QVERIFY(ring.hasKey(cap + 10));
        // Re-installing an old index makes it the newest again.
        QVERIFY(ring.setKey(12, rawKey('z')));
        QVERIFY(ring.setKey(cap + 11, rawKey()));
        QVERIFY(ring.hasKey(12));
        QVERIFY(!ring.hasKey(13));
    }

    // A lookup must not create a slot: an `operator[]` read on the hash
    // inserts an empty value, and hasAnyKey() (asked per frame) would then be
    // true for an unkeyed ring.
    void askingAboutAnIndexDoesNotMakeTheRingClaimAKey()
    {
        CallFrameCryptor cryptor;
        QVERIFY(!cryptor.hasKey(7));
        QVERIFY(!cryptor.hasKey(200));
        CallFrameCryptor::DecryptDiagnosis why;
        QByteArray frame(1 + 16 + 12 + 2 + 8, 'z');
        frame[frame.size() - 2] = char(12);
        frame[frame.size() - 1] = char(9);
        QVERIFY(cryptor.decryptFrame(frame, CallFrameCryptor::FrameKind::Audio,
                                     &why)
                    .isEmpty());
        QCOMPARE(why.reason, CallFrameCryptor::DecryptFailure::NoKeyForIndex);
        QVERIFY2(!cryptor.hasAnyKey(),
                 "a lookup inserted an empty slot and the ring now claims a "
                 "key");
        // clearKeys() really empties a keyed ring.
        QVERIFY(cryptor.setKey(200, rawKey()));
        QVERIFY(cryptor.hasAnyKey());
        cryptor.clearKeys();
        QVERIFY(!cryptor.hasAnyKey());
        QVERIFY(!cryptor.hasKey(200));
    }

    // SFU-injected frames are recognised by their exact trailer only.
    // livekit-server injects unencrypted blank frames into encrypted tracks
    // (Opus silence on mute or close), each ending in the room's
    // `JoinResponse.sif_trailer`. The SFU is outside the trust boundary, so a
    // shape heuristic would misfile real frames.
    void aServerInjectedFrameIsRecognisedByItsExactTrailer()
    {
        // livekit-server's OpusSilenceFrame: f8 ff fe + 77 zeros.
        QByteArray silence(80, '\0');
        silence[0] = char(0xf8);
        silence[1] = char(0xff);
        silence[2] = char(0xfe);
        QVERIFY(CallFrameCryptor::startsWithOpusSilenceFrame(
            silence.constData(), silence.size()));

        // A livekit-shaped trailer: base62, about 43 bytes, ending in 'R' (82).
        const QByteArray trailer(
            "k3P9dQ2mZ7xW4vB8nT1cY6hJ0fL5sG2aE9rU3oKqXiR");
        QCOMPARE(trailer.back(), 'R');
        const QByteArray injected = silence + trailer;

        QVERIFY2(CallFrameCryptor::endsWithServerTrailer(injected, trailer),
                 "a silence frame carrying the room's trailer was not "
                 "recognised");
        // Control: not armed, nothing matches.
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(injected,
                                                         QByteArray()));
        // One byte different anywhere in the trailer: not ours.
        QByteArray other = trailer;
        other[5] = other[5] == 'a' ? 'b' : 'a';
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(injected, other));
        // A longer trailer that ends with the real one does not match.
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(
            injected, QByteArray("x") + trailer));
        // A frame that is only the trailer carries no payload.
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(trailer, trailer));
        // An over-long "trailer" never arms.
        const QByteArray huge(CallFrameCryptor::kMaxServerTrailerBytes + 1,
                              'A');
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(
            silence + huge, huge));

        // Unrecognised, that frame fails as bad-iv-length with keyIndex equal
        // to the trailer's last byte.
        CallFrameCryptor keyed;
        QVERIFY(keyed.setKey(0, rawKey()));
        CallFrameCryptor::DecryptDiagnosis why;
        QVERIFY(keyed.decryptFrame(injected, CallFrameCryptor::FrameKind::Audio,
                                   &why)
                    .isEmpty());
        QCOMPARE(why.reason, CallFrameCryptor::DecryptFailure::BadIvLength);
        QCOMPARE(why.keyIndex, 82);
    }

    // A real encrypted frame is never taken for an injected one, even when
    // its tail is printable ASCII: the match is the whole trailer.
    void aRealFrameWhoseTailIsAsciiIsNotMisclassified()
    {
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        QVERIFY(sender.setKey(82, rawKey('q')));
        sender.setCurrentKeyIndex(82);   // trailer byte 'R', as above
        QVERIFY(receiver.setKey(82, rawKey('q')));
        const QByteArray trailer(
            "k3P9dQ2mZ7xW4vB8nT1cY6hJ0fL5sG2aE9rU3oKqXiR");
        for (int i = 0; i < 200; ++i) {
            const QByteArray payload =
                QByteArray("\x01") + QByteArray::number(i).repeated(9);
            const QByteArray wire = sender.encryptFrame(
                payload, CallFrameCryptor::FrameKind::Audio, 5,
                static_cast<quint32>(1000 + i));
            QVERIFY(!wire.isEmpty());
            QVERIFY2(!CallFrameCryptor::endsWithServerTrailer(wire, trailer),
                     "a real encrypted frame was classified as injected");
            // Same last byte as the trailer, so only an exact match separates
            // them.
            QCOMPARE(wire.back(), trailer.back());
            QCOMPARE(receiver.decryptFrame(wire,
                                           CallFrameCryptor::FrameKind::Audio),
                     payload);
        }
        // A cleartext frame ending in printable ASCII but not the trailer is
        // not injected either.
        const QByteArray asciiTail = QByteArray(40, 'x') + trailer.left(20);
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(asciiTail, trailer));
    }
};

QTEST_MAIN(CallFrameCryptorTest)
#include "CallFrameCryptorTest.moc"
