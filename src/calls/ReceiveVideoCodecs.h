#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace lightning::calls {

struct ReceiveVideoCodec {
    QString encoding;
    /// Depayloader (named recvdepay) through decoder (named videodec).
    QString chain;
    /// VP8 and H.264 leave a codec header in the clear; livekit-client
    /// encrypts a VP9 or AV1 frame whole.
    bool encryptedWhole = false;
};

/// The video codecs this runtime can receive, by SDP encoding name. `has`
/// answers whether an element factory exists.
inline QList<ReceiveVideoCodec> receivableVideoCodecs(
    const std::function<bool(const QByteArray &)> &has)
{
    struct Spec {
        const char *encoding;
        const char *depay;
        const char *parse;
        QList<QByteArray> needs;
        QList<QByteArray> decoders;
        bool encryptedWhole;
    };
    // A frame decoded against a reference that never arrived is the speckled
    // picture of GitHub #68; freeze instead and ask the sender for a keyframe.
    static const char *const recovery =
        "request-keyframe=true wait-for-keyframe=true ";
    static const QList<Spec> specs{
        {"VP8", "rtpvp8depay", "", {}, {"vp8dec"}, false},
        {"H264", "rtph264depay",
         "! video/x-h264,stream-format=byte-stream,alignment=au ! h264parse ",
         {"h264parse"}, {"avdec_h264", "openh264dec"}, false},
        {"VP9", "rtpvp9depay", "", {}, {"vp9dec", "avdec_vp9"}, true},
        {"AV1", "rtpav1depay", "", {}, {"dav1ddec", "av1dec"}, true},
    };
    QList<ReceiveVideoCodec> out;
    for (const Spec &spec : specs) {
        bool usable = has(spec.depay);
        for (const QByteArray &need : spec.needs)
            usable = usable && has(need);
        QByteArray decoder;
        for (const QByteArray &candidate : spec.decoders) {
            if (decoder.isEmpty() && has(candidate))
                decoder = candidate;
        }
        if (!usable || decoder.isEmpty())
            continue;
        // rtpav1depay's keyframe properties vary by plugin release; the
        // engine sets them where they exist.
        const bool av1 = qstrcmp(spec.encoding, "AV1") == 0;
        out.append({QLatin1String(spec.encoding),
                    QStringLiteral("%1 name=recvdepay %2%3! %4 name=videodec ")
                        .arg(QLatin1String(spec.depay),
                             QLatin1String(av1 ? "" : recovery),
                             QLatin1String(spec.parse),
                             QLatin1String(decoder)),
                    spec.encryptedWhole});
    }
    return out;
}

/// The codec to name when none of a track's codecs can be received, else
/// empty. `mimes` are LiveKit's ("video/VP9").
inline QString unsupportedVideoCodec(const QStringList &mimes,
                                     const QStringList &receivable)
{
    QString first;
    for (const QString &mime : mimes) {
        const QString name = mime.section(QLatin1Char('/'), -1).toUpper();
        if (name.isEmpty())
            continue;
        if (receivable.contains(name))
            return {};
        if (first.isEmpty())
            first = name == QLatin1String("H264") ? QStringLiteral("H.264")
                                                  : name;
    }
    return first;
}

/// True when a VP9 RTP payload ends a spatial layer's frame (the E bit).
inline bool vp9PayloadEndsFrame(const unsigned char *payload, qsizetype size)
{
    return payload && size > 0 && (payload[0] & 0x04) != 0;
}

}
