#include "calls/CallFrameCryptor.h"

#include <QSet>
#include <mutex>
#include <thread>

#include <QtTest/QtTest>

namespace {
QByteArray rawKey(char fill = 'k')
{
    return QByteArray(32, fill);
}
}

class CallFrameCryptorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void bothElementAndLivekitKeyLengthsAreAccepted()
    {
        CallFrameCryptor sixteen;
        QVERIFY2(sixteen.setKey(0, QByteArray(16, 'k')),
                 "a 16-byte element-call key was refused");
        CallFrameCryptor thirtyTwo;
        QVERIFY2(thirtyTwo.setKey(0, QByteArray(32, 'k')),
                 "a 32-byte livekit-client key was refused");
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
        QCOMPARE(CallFrameCryptor::headerBytes(
                     CallFrameCryptor::FrameKind::VideoWhole), 0);
    }

    void anH264FrameKeepsItsHeadersAndHidesItsSlice()
    {
        const QByteArray sps = QByteArray::fromHex("0000000167428028");
        const QByteArray pps = QByteArray::fromHex("0000000168ce3c80");
        QByteArray slice = QByteArray::fromHex("000000016588");
        slice += QByteArray(4000, char(0));
        const QByteArray frame = sps + pps + slice;
        const int clear = frame.indexOf(slice) + 6;
        QCOMPARE(CallFrameCryptor::h264ClearBytes(frame.constData(), frame.size()),
                 clear);

        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, QByteArray(16, 'k')));
        for (int i = 0; i < 2000; ++i) {
            const QByteArray wire = cryptor.encryptFrame(
                frame, CallFrameCryptor::FrameKind::VideoH264, 7, 1000 + i);
            QVERIFY(!wire.isEmpty());
            QCOMPARE(wire.first(clear), frame.first(clear));
            const QByteArray body = wire.sliced(clear);
            QVERIFY2(!body.contains(QByteArray::fromHex("000001")),
                     "a start code in the ciphertext would split the frame");
            QVERIFY(!body.contains(QByteArray::fromHex("000000")));
            QCOMPARE(CallFrameCryptor::h264ClearBytes(wire.constData(),
                                                      wire.size()),
                     clear);
            CallFrameCryptor::DecryptDiagnosis why;
            QCOMPARE(cryptor.decryptFrame(
                         wire, CallFrameCryptor::FrameKind::VideoH264, &why),
                     frame);
        }

        const QByteArray shortCodes = QByteArray::fromHex("00000167428028")
            + QByteArray::fromHex("00000168ce3c80")
            + QByteArray::fromHex("00000165880102030405060708");
        const QByteArray widened = cryptor.encryptFrame(
            shortCodes, CallFrameCryptor::FrameKind::VideoH264, 7, 9);
        QVERIFY(widened.startsWith(
            QByteArray::fromHex("0000000167428028" "0000000168ce3c80"
                                "000000016588")));

        const QByteArray rewritten = cryptor.encryptFrame(
            QByteArray::fromHex("00000001" "6742c01fda014016e840" "0000ea6021"
                                "000000000001" "68ce3c80"
                                "000000000001" "6588aabbccddeeff"),
            CallFrameCryptor::FrameKind::VideoH264, 7, 10);
        QVERIFY(rewritten.startsWith(QByteArray::fromHex(
            "00000001" "6742c01fda014016e807844235"
            "00000001" "68ce3c80" "00000001" "6588")));

        QByteArray wire = cryptor.encryptFrame(
            frame, CallFrameCryptor::FrameKind::VideoH264, 7, 5);
        wire[5] = char(wire.at(5) ^ 1);
        QVERIFY(cryptor
                    .decryptFrame(wire, CallFrameCryptor::FrameKind::VideoH264,
                                  nullptr)
                    .isEmpty());
        QVERIFY(cryptor
                    .encryptFrame(sps + pps,
                                  CallFrameCryptor::FrameKind::VideoH264, 7, 6)
                    .isEmpty());
        QVERIFY(cryptor
                    .decryptFrame(sps + pps,
                                  CallFrameCryptor::FrameKind::VideoH264, nullptr)
                    .isEmpty());
    }

    void keyDerivationIsStableAndKeyDependent()
    {
        const QByteArray a = CallFrameCryptor::deriveKey(rawKey('a'));
        const QByteArray b = CallFrameCryptor::deriveKey(rawKey('b'));
        QCOMPARE(a.size(), 16);
        QCOMPARE(b.size(), 16);
        QCOMPARE(CallFrameCryptor::deriveKey(rawKey('a')), a);
        QVERIFY(a != b);
        QVERIFY(CallFrameCryptor::deriveKey(QByteArray()).isEmpty());
    }

    void keyDerivationMatchesAnIndependentHkdf()
    {
        QCOMPARE(CallFrameCryptor::deriveKey(rawKey('k')).toHex(),
                 QByteArray("262178a9e5dabf73df9342ed5bae9fe1"));
    }

    void ivLayoutIsExactlyTheReferenceConstruction()
    {
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
        QCOMPARE(static_cast<quint8>(iv.at(8)), quint8(0x0A));
        QCOMPARE(static_cast<quint8>(iv.at(9)), quint8(0x0B));
        QCOMPARE(static_cast<quint8>(iv.at(10)), quint8(0x0C));
        QCOMPARE(static_cast<quint8>(iv.at(11)), quint8(0x08));
    }

    void ivSubtractionWrapsLikeTheReference()
    {
        const QByteArray iv = CallFrameCryptor::makeIvForTest(0, 1, 5);
        QCOMPARE(static_cast<quint8>(iv.at(8)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(9)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(10)), quint8(0xFF));
        QCOMPARE(static_cast<quint8>(iv.at(11)), quint8(0xFC));
    }

    void ivIsNeverReusedForTheSameSsrc()
    {
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));
        QSet<QByteArray> seen;
        for (int i = 0; i < 512; ++i) {
            const QByteArray wire = cryptor.encryptFrame(
                QByteArray("\x01payload"), CallFrameCryptor::FrameKind::Audio,
42,1000);
            QVERIFY(!wire.isEmpty());
            seen.insert(wire.mid(wire.size() - 14, 12));
        }
        QCOMPARE(seen.size(), 512);
    }

    void twoTracksSharingOneCryptorMustNotShareAnIv()
    {
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));
        QSet<QByteArray> seen;
        for (int i = 0; i < 256; ++i) {
            for (quint32 ssrc : {1u, 2u, 3u}) {
                const QByteArray wire = cryptor.encryptFrame(
                    QByteArray("\x01payload"),
                    CallFrameCryptor::FrameKind::Audio, ssrc,
9000);
                QVERIFY(!wire.isEmpty());
                seen.insert(wire.mid(wire.size() - 14, 12));
            }
        }
        QCOMPARE(seen.size(), 768);
    }

    void twoThreadsThroughOneCryptorStayCorrect()
    {
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));

        std::mutex guard;
        QSet<QByteArray> seen;
        const auto run = [&](quint32 ssrc) {
            for (int i = 0; i < 400; ++i) {
                const QByteArray wire = cryptor.encryptFrame(
                    QByteArray("\x01payload"),
                    CallFrameCryptor::FrameKind::Audio, ssrc,
7000);
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

        QCOMPARE(wire.size(), payload.size() + 16 + 12 + 2);
        QCOMPARE(wire.at(0), payload.at(0));
        QCOMPARE(static_cast<quint8>(wire.at(wire.size() - 2)), quint8(12));
        QCOMPARE(static_cast<quint8>(wire.at(wire.size() - 1)), quint8(3));
        QVERIFY(!wire.contains(QByteArray("hello world")));
    }

    void theTrailerSaysWhetherAFrameWasEncryptedWithoutDecryptingIt()
    {
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(5, rawKey()));
        cryptor.setCurrentKeyIndex(5);
        const QByteArray payload("\x01" "hello world");
        const QByteArray wire = cryptor.encryptFrame(
            payload, CallFrameCryptor::FrameKind::Audio, 7, 99);
        QVERIFY(!wire.isEmpty());

        QVERIFY2(CallFrameCryptor::looksEncrypted(
                     wire, CallFrameCryptor::FrameKind::Audio),
                 "a frame this class encrypted is not recognised as one");
        CallFrameCryptor empty;
        QVERIFY(!empty.hasAnyKey());

        QVERIFY2(!CallFrameCryptor::looksEncrypted(
                     payload, CallFrameCryptor::FrameKind::Audio),
                 "an ordinary Opus-shaped payload is reported as encrypted");
        QByteArray tiny(8, '\0');
        tiny[6] = char(12);
        tiny[7] = char(3);
        QVERIFY(!CallFrameCryptor::looksEncrypted(
            tiny, CallFrameCryptor::FrameKind::Audio));
        QByteArray justAudio(1 + 16 + 12 + 2, 'x');
        justAudio[justAudio.size() - 2] = char(12);
        justAudio[justAudio.size() - 1] = char(0);
        QVERIFY(CallFrameCryptor::looksEncrypted(
            justAudio, CallFrameCryptor::FrameKind::Audio));
        QVERIFY2(!CallFrameCryptor::looksEncrypted(
                     justAudio, CallFrameCryptor::FrameKind::VideoKey),
                 "the length floor does not follow the frame kind's header");

        QByteArray wrongIv = wire;
        wrongIv[wrongIv.size() - 2] = char(16);
        QVERIFY(!CallFrameCryptor::looksEncrypted(
            wrongIv, CallFrameCryptor::FrameKind::Audio));
        QByteArray highIndex = wire;
        highIndex[highIndex.size() - 1] = char(200);
        QVERIFY(CallFrameCryptor::looksEncrypted(
            highIndex, CallFrameCryptor::FrameKind::Audio));

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

        QByteArray header = wire;
        header[0] = static_cast<char>(header.at(0) ^ 0xFF);
        QVERIFY2(receiver.decryptFrame(
                     header, CallFrameCryptor::FrameKind::Audio).isEmpty(),
                 "the cleartext header is authenticated and must be covered");
    }

    void aRingSaysWhetherItHoldsAnyKeyAtAll()
    {
        CallFrameCryptor ring;
        QVERIFY(!ring.hasAnyKey());
        QVERIFY(ring.setKey(3, QByteArray(32, 'k')));
        QVERIFY(ring.hasAnyKey());
        CallFrameCryptor refused;
        QVERIFY(!refused.setKey(0, QByteArray(7, 'k')));
        QVERIFY(!refused.hasAnyKey());
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
        QVERIFY(receiver.setKey(0, rawKey()));
        const QByteArray wire = sender.encryptFrame(
            QByteArray("\x01secret"), CallFrameCryptor::FrameKind::Audio, 1, 2);
        QVERIFY(!wire.isEmpty());
        QVERIFY(receiver.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio)
                    .isEmpty());
    }

    void rotationKeepsInFlightFramesDecryptable()
    {
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
        CallFrameCryptor cryptor;
        QVERIFY(cryptor.setKey(0, rawKey()));
        for (int size : {0, 1, 5, 14, 20, 28}) {
            const QByteArray junk(size, '\x01');
            QVERIFY2(cryptor.decryptFrame(
                         junk, CallFrameCryptor::FrameKind::Audio).isEmpty(),
                     qPrintable(QStringLiteral("size %1 must be refused")
                                    .arg(size)));
        }
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
        cryptor.setCurrentKeyIndex(255);
        cryptor.setCurrentKeyIndex(256);
        QCOMPARE(cryptor.currentKeyIndex(), 255);
        cryptor.setCurrentKeyIndex(-1);
        QCOMPARE(cryptor.currentKeyIndex(), 255);
    }

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
        QCOMPARE(static_cast<unsigned char>(wire.at(wire.size() - 1)), 200);
        QCOMPARE(static_cast<unsigned char>(wire.at(wire.size() - 2)), 12);

        CallFrameCryptor::DecryptDiagnosis why;
        QCOMPARE(receiver.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio,
                                       &why),
                 payload);
        QCOMPARE(why.reason, CallFrameCryptor::DecryptFailure::None);

        CallFrameCryptor neighbour;
        QVERIFY(neighbour.setKey(199, rawKey('e')));
        QVERIFY(neighbour.decryptFrame(wire, CallFrameCryptor::FrameKind::Audio,
                                       &why)
                    .isEmpty());
        QCOMPARE(why.reason, CallFrameCryptor::DecryptFailure::NoKeyForIndex);
        QCOMPARE(why.keyIndex, 200);
    }

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
        QVERIFY(!ring.hasKey(1));
        QVERIFY(!ring.hasKey(11));
        QVERIFY(ring.hasKey(12));
        QVERIFY(ring.hasKey(cap + 10));
        QVERIFY(ring.setKey(12, rawKey('z')));
        QVERIFY(ring.setKey(cap + 11, rawKey()));
        QVERIFY(ring.hasKey(12));
        QVERIFY(!ring.hasKey(13));
    }

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
        QVERIFY(cryptor.setKey(200, rawKey()));
        QVERIFY(cryptor.hasAnyKey());
        cryptor.clearKeys();
        QVERIFY(!cryptor.hasAnyKey());
        QVERIFY(!cryptor.hasKey(200));
    }

    void aServerInjectedFrameIsRecognisedByItsExactTrailer()
    {
        QByteArray silence(80, '\0');
        silence[0] = char(0xf8);
        silence[1] = char(0xff);
        silence[2] = char(0xfe);
        QVERIFY(CallFrameCryptor::startsWithOpusSilenceFrame(
            silence.constData(), silence.size()));

        const QByteArray trailer(
            "k3P9dQ2mZ7xW4vB8nT1cY6hJ0fL5sG2aE9rU3oKqXiR");
        QCOMPARE(trailer.back(), 'R');
        const QByteArray injected = silence + trailer;

        QVERIFY2(CallFrameCryptor::endsWithServerTrailer(injected, trailer),
                 "a silence frame carrying the room's trailer was not "
                 "recognised");
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(injected,
                                                         QByteArray()));
        QByteArray other = trailer;
        other[5] = other[5] == 'a' ? 'b' : 'a';
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(injected, other));
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(
            injected, QByteArray("x") + trailer));
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(trailer, trailer));
        const QByteArray huge(CallFrameCryptor::kMaxServerTrailerBytes + 1,
                              'A');
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(
            silence + huge, huge));

        CallFrameCryptor keyed;
        QVERIFY(keyed.setKey(0, rawKey()));
        CallFrameCryptor::DecryptDiagnosis why;
        QVERIFY(keyed.decryptFrame(injected, CallFrameCryptor::FrameKind::Audio,
                                   &why)
                    .isEmpty());
        QCOMPARE(why.reason, CallFrameCryptor::DecryptFailure::BadIvLength);
        QCOMPARE(why.keyIndex, 82);
    }

    void aRealFrameWhoseTailIsAsciiIsNotMisclassified()
    {
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        QVERIFY(sender.setKey(82, rawKey('q')));
        sender.setCurrentKeyIndex(82);
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
            QCOMPARE(wire.back(), trailer.back());
            QCOMPARE(receiver.decryptFrame(wire,
                                           CallFrameCryptor::FrameKind::Audio),
                     payload);
        }
        const QByteArray asciiTail = QByteArray(40, 'x') + trailer.left(20);
        QVERIFY(!CallFrameCryptor::endsWithServerTrailer(asciiTail, trailer));
    }
};

QTEST_MAIN(CallFrameCryptorTest)
#include "CallFrameCryptorTest.moc"
