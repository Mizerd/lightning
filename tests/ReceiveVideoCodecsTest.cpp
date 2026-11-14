#include <QtTest/QtTest>

#include "calls/ReceiveVideoCodecs.h"

using namespace lightning::calls;

class ReceiveVideoCodecsTest : public QObject
{
    Q_OBJECT

    static QStringList encodings(const QList<QByteArray> &present)
    {
        QStringList out;
        for (const ReceiveVideoCodec &codec : receivableVideoCodecs(
                 [&](const QByteArray &name) { return present.contains(name); }))
            out << codec.encoding;
        return out;
    }

private Q_SLOTS:
    void aCodecIsOfferedOnlyWhenItsWholeChainExists()
    {
        const QList<QByteArray> base{"rtpvp8depay", "vp8dec"};
        QCOMPARE(encodings(base), QStringList{"VP8"});
        QCOMPARE(encodings(base + QList<QByteArray>{"rtpvp9depay", "vp9dec"}),
                 (QStringList{"VP8", "VP9"}));
        QCOMPARE(encodings(base + QList<QByteArray>{"rtpvp9depay", "avdec_vp9"}),
                 (QStringList{"VP8", "VP9"}));
        QCOMPARE(encodings(base + QList<QByteArray>{"rtpav1depay", "dav1ddec"}),
                 (QStringList{"VP8", "AV1"}));
        QCOMPARE(encodings(base + QList<QByteArray>{"rtpav1depay", "av1dec"}),
                 (QStringList{"VP8", "AV1"}));
        QCOMPARE(encodings(base + QList<QByteArray>{"rtph264depay", "h264parse",
                                                    "openh264dec", "rtpvp9depay",
                                                    "vp9dec", "rtpav1depay",
                                                    "av1dec"}),
                 (QStringList{"VP8", "H264", "VP9", "AV1"}));

        QCOMPARE(encodings(base + QList<QByteArray>{"vp9dec", "av1dec",
                                                    "dav1ddec"}),
                 QStringList{"VP8"});
        QCOMPARE(encodings(base + QList<QByteArray>{"rtpvp9depay",
                                                    "rtpav1depay"}),
                 QStringList{"VP8"});
        QCOMPARE(encodings(base + QList<QByteArray>{"rtph264depay",
                                                    "avdec_h264"}),
                 QStringList{"VP8"});
    }

    void aChainNamesTheDecoderThatExists()
    {
        const auto codecs = receivableVideoCodecs([](const QByteArray &name) {
            return name == "rtpvp8depay" || name == "vp8dec"
                || name == "rtpvp9depay" || name == "avdec_vp9"
                || name == "rtpav1depay" || name == "av1dec";
        });
        QCOMPARE(codecs.size(), 3);
        for (const ReceiveVideoCodec &codec : codecs)
            QVERIFY2(codec.chain.startsWith(QLatin1String("rtp"))
                         && codec.chain.section(QLatin1Char(' '), 0, 0)
                                .endsWith(QLatin1String("depay")),
                     qPrintable(codec.chain));
        QCOMPARE(codecs[0].chain,
                 QStringLiteral("rtpvp8depay name=recvdepay request-keyframe=true "
                                "wait-for-keyframe=true ! vp8dec name=videodec "));
        QVERIFY(!codecs[0].encryptedWhole);
        QCOMPARE(codecs[1].chain,
                 QStringLiteral("rtpvp9depay name=recvdepay request-keyframe=true "
                                "wait-for-keyframe=true ! avdec_vp9 name=videodec "));
        QVERIFY(codecs[1].encryptedWhole);
        QCOMPARE(codecs[2].chain,
                 QStringLiteral("rtpav1depay name=recvdepay ! av1dec name=videodec "));
        QVERIFY(codecs[2].encryptedWhole);
    }

    void aTrackIsUnsupportedOnlyWhenNoneOfItsCodecsCanBeReceived()
    {
        const QStringList receivable{"VP8", "H264", "VP9"};
        QCOMPARE(unsupportedVideoCodec({"video/AV1"}, receivable),
                 QStringLiteral("AV1"));
        QCOMPARE(unsupportedVideoCodec({"video/av1"}, receivable),
                 QStringLiteral("AV1"));
        QCOMPARE(unsupportedVideoCodec({"video/H264"}, {"VP8"}),
                 QStringLiteral("H.264"));
        QCOMPARE(unsupportedVideoCodec({"video/VP9", "video/VP8"}, {"VP8"}),
                 QString());
        QCOMPARE(unsupportedVideoCodec({"video/VP9"}, receivable), QString());
        QCOMPARE(unsupportedVideoCodec({}, receivable), QString());
        QCOMPARE(unsupportedVideoCodec({QString()}, receivable), QString());
    }

    void onlyTheEBitEndsAVp9Frame()
    {
        const unsigned char end[] = {0x04};
        const unsigned char middle[] = {0xfb};
        QVERIFY(vp9PayloadEndsFrame(end, 1));
        QVERIFY(!vp9PayloadEndsFrame(middle, 1));
        QVERIFY(!vp9PayloadEndsFrame(end, 0));
        QVERIFY(!vp9PayloadEndsFrame(nullptr, 1));
    }
};

QTEST_APPLESS_MAIN(ReceiveVideoCodecsTest)
#include "ReceiveVideoCodecsTest.moc"
