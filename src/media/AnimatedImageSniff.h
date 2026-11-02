#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>

#include <cstring>

namespace lightning::animsniff {

inline bool isGif(const QByteArray &bytes)
{
    return bytes.size() >= 13
        && (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"));
}

inline bool isAnimatedWebp(const QByteArray &bytes)
{
    return bytes.size() >= 30 && bytes.startsWith("RIFF")
        && std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0
        && std::memcmp(bytes.constData() + 12, "VP8X", 4) == 0
        && (static_cast<unsigned char>(bytes.at(20)) & 0x02u) != 0;
}

inline int gifFrameCount(const QByteArray &bytes, int stopAt = 2)
{
    if (!isGif(bytes))
        return 0;
    const qsizetype n = bytes.size();
    const auto u8 = [&bytes](qsizetype i) {
        return static_cast<unsigned char>(bytes.at(i));
    };
    qsizetype p = 13;
    const unsigned char screenFlags = u8(10);
    if (screenFlags & 0x80u)
        p += 3 * (qsizetype(1) << ((screenFlags & 0x07u) + 1));
    const auto skipSubBlocks = [&](qsizetype &q) {
        while (q < n) {
            const int len = u8(q);
            ++q;
            if (len == 0)
                return true;
            q += len;
        }
        return false;
    };
    int frames = 0;
    while (p < n) {
        const unsigned char tag = u8(p++);
        if (tag == 0x3B)
            break;
        if (tag == 0x21) {
            ++p;
            if (!skipSubBlocks(p))
                break;
        } else if (tag == 0x2C) {
            if (p + 9 > n)
                break;
            const unsigned char imageFlags = u8(p + 8);
            p += 9;
            if (imageFlags & 0x80u)
                p += 3 * (qsizetype(1) << ((imageFlags & 0x07u) + 1));
            ++p;
            if (p > n || !skipSubBlocks(p))
                break;
            if (++frames >= stopAt)
                return frames;
        } else {
            break;
        }
    }
    return frames;
}

// How fast an animation is paced, read from its own block structure (the GIF
// graphic-control extension, the WebP ANMF duration), never from a decoder:
// Qt's GIF handler rewrites a 0 or 1 cs delay to 100 ms, the way browsers do,
// so what it reports cannot tell very fast pacing from an ordinary file.
//   * GIF: a delay of 0 or 1 cs (0..10 ms) plays at 100 ms everywhere, so it
//     is NOT fast; 2 to 4 cs (20..40 ms) really is 25-50 fps.
//   * WebP: no such rule is shown for its handler, so 0 to 49 ms all count.
// This catches very fast pacing; it is not a photosensitivity test (content
// can flash at a slow rate). The protections for that are reduced motion and
// the autoplay setting, both of which stop the animation.
struct AnimationTiming {
    int frames = 0;
    int counted = 0;
    int flashing = 0;
};
constexpr int kTimingStop = 100000;
constexpr int kMaxSaneFrames = 300;
constexpr int kFlashingBelowMs = 50;

inline bool timingTooBusy(const AnimationTiming &t)
{
    return t.frames > kMaxSaneFrames
        || (t.counted > 0 && t.flashing * 2 > t.counted);
}

inline AnimationTiming animationTiming(const QByteArray &bytes)
{
    AnimationTiming t;
    const qsizetype n = bytes.size();
    const auto u8 = [&bytes](qsizetype i) {
        return static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(i)));
    };
    if (isGif(bytes) && n >= 13) {
        qsizetype p = 13;
        if (u8(10) & 0x80u)
            p += 3 * (qsizetype(1) << ((u8(10) & 0x07u) + 1));
        const auto skipSubBlocks = [&](qsizetype &q) {
            while (q < n) {
                const int len = int(u8(q));
                ++q;
                if (len == 0)
                    return true;
                q += len;
            }
            return false;
        };
        // A decoder applies the LAST graphic-control extension before each
        // image, so that delay is held until the image descriptor arrives. A
        // frame with none of its own inherits the previous one's (100 ms
        // before any), and an extension after the last frame is never used.
        int delayMs = 100;
        while (p < n && t.frames < kTimingStop) {
            const unsigned int tag = u8(p++);
            if (tag == 0x3B)
                break;
            if (tag == 0x21) {
                if (p >= n)
                    break;
                const unsigned int label = u8(p++);
                if (label == 0xF9 && p + 4 <= n && u8(p) >= 4)
                    delayMs = int(u8(p + 2) | (u8(p + 3) << 8)) * 10;
                if (!skipSubBlocks(p))
                    break;
            } else if (tag == 0x2C) {
                if (p + 9 > n)
                    break;
                const unsigned int flags = u8(p + 8);
                p += 9;
                if (flags & 0x80u)
                    p += 3 * (qsizetype(1) << ((flags & 0x07u) + 1));
                ++p;
                if (p > n || !skipSubBlocks(p))
                    break;
                ++t.frames;
                ++t.counted;
                if (delayMs > 10 && delayMs < kFlashingBelowMs)
                    ++t.flashing;
            } else {
                break;
            }
        }
    } else if (isAnimatedWebp(bytes)) {
        const qsizetype riff = qsizetype(u8(4) | (u8(5) << 8) | (u8(6) << 16)
                                         | (quint32(u8(7)) << 24));
        const qsizetype end = qMin(n, qsizetype(8) + riff);
        qsizetype p = 12;
        while (p + 8 <= end && t.frames < kTimingStop) {
            const quint32 size = u8(p + 4) | (u8(p + 5) << 8) | (u8(p + 6) << 16)
                | (quint32(u8(p + 7)) << 24);
            if (std::memcmp(bytes.constData() + p, "ANMF", 4) == 0) {
                ++t.frames;
                if (size >= 16 && p + 8 + 15 <= end) {
                    const int ms = int(u8(p + 8 + 12) | (u8(p + 8 + 13) << 8)
                                       | (u8(p + 8 + 14) << 16));
                    ++t.counted;
                    if (ms < kFlashingBelowMs)
                        ++t.flashing;
                }
            }
            p += 8 + qsizetype(size) + (size & 1u);
        }
    }
    return t;
}

inline bool isAnimation(const QByteArray &bytes)
{
    return isAnimatedWebp(bytes) || gifFrameCount(bytes, 2) >= 2;
}

inline QString animationSuffix(const QByteArray &bytes)
{
    if (isAnimatedWebp(bytes))
        return QStringLiteral("webp");
    if (gifFrameCount(bytes, 2) >= 2)
        return QStringLiteral("gif");
    return {};
}

inline QSize canvasSize(const QByteArray &bytes)
{
    const auto u8 = [&bytes](qsizetype i) {
        return static_cast<unsigned int>(
            static_cast<unsigned char>(bytes.at(i)));
    };
    if (isGif(bytes))
        return QSize(int(u8(6) | (u8(7) << 8)), int(u8(8) | (u8(9) << 8)));
    if (bytes.size() >= 30 && bytes.startsWith("RIFF")
        && std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0
        && std::memcmp(bytes.constData() + 12, "VP8X", 4) == 0) {
        const int w = int(u8(24) | (u8(25) << 8) | (u8(26) << 16)) + 1;
        const int h = int(u8(27) | (u8(28) << 8) | (u8(29) << 16)) + 1;
        return QSize(w, h);
    }
    return {};
}

constexpr qint64 kAvatarMaxPixels = 512LL * 512;
constexpr qint64 kBannerMaxPixels = 4LL * 1024 * 1024;

inline bool canvasWithin(const QByteArray &bytes, qint64 maxPixels)
{
    const QSize s = canvasSize(bytes);
    return s.isValid() && s.width() > 0 && s.height() > 0
        && qint64(s.width()) * s.height() <= maxPixels;
}

// ---- Metadata stripping (uploads) ----
//
// A kept animation is uploaded as its original bytes, and avatars and banners
// are public to every room the account is in. The still path re-encodes and so
// drops EXIF/GPS; this does the same for an animation without touching its
// frames. What goes:
//   * GIF: comment extensions (0xFE), plain-text extensions (0x01; Qt does not
//     draw them) and every application extension except the looping ones
//     (NETSCAPE2.0, ANIMEXTS1.0), which is where XMP ("XMP DataXMP") and ICC
//     profiles live; and anything after the trailer.
//   * WebP: every chunk but the ones an animation is made of (VP8X, ANIM,
//     ANMF, ALPH, VP8, VP8L, ICCP), so EXIF, "XMP " and any unknown or
//     vendor chunk go, with the EXIF/XMP VP8X flags cleared and the RIFF size
//     rewritten; and anything after the RIFF payload.
// Every length is checked before it is used. Anything malformed or truncated
// returns an empty array rather than a partial copy: skipping what cannot be
// parsed is exactly how metadata would slip through.

inline QByteArray stripGifMetadata(const QByteArray &in)
{
    if (!isGif(in))
        return {};
    const qsizetype n = in.size();
    const auto u8 = [&in](qsizetype i) {
        return static_cast<unsigned char>(in.at(i));
    };
    qsizetype p = 13;
    const unsigned char screenFlags = u8(10);
    if (screenFlags & 0x80u)
        p += 3 * (qsizetype(1) << ((screenFlags & 0x07u) + 1));
    if (p > n)
        return {};
    const auto chainEnd = [&](qsizetype q) -> qsizetype {
        while (q < n) {
            const int len = u8(q);
            ++q;
            if (len == 0)
                return q;
            q += len;
        }
        return -1;
    };
    QByteArray out;
    out.reserve(n);
    out.append(in.constData(), p);
    while (p < n) {
        const unsigned char tag = u8(p);
        if (tag == 0x3B) {
            out.append(char(0x3B));
            return out;
        }
        qsizetype end = -1;
        bool keep = false;
        if (tag == 0x21) {
            if (p + 2 > n)
                return {};
            const unsigned char label = u8(p + 1);
            if (label == 0xFF) {
                if (p + 3 + 11 > n || u8(p + 2) != 11)
                    return {};
                const QByteArray id = in.mid(p + 3, 11);
                keep = id == QByteArrayLiteral("NETSCAPE2.0")
                    || id == QByteArrayLiteral("ANIMEXTS1.0");
                end = chainEnd(p + 3 + 11);
            } else {
                keep = label == 0xF9;
                end = chainEnd(p + 2);
            }
        } else if (tag == 0x2C) {
            if (p + 10 > n)
                return {};
            const unsigned char imageFlags = u8(p + 9);
            qsizetype q = p + 10;
            if (imageFlags & 0x80u)
                q += 3 * (qsizetype(1) << ((imageFlags & 0x07u) + 1));
            ++q;
            if (q > n)
                return {};
            end = chainEnd(q);
            keep = true;
        } else {
            return {};
        }
        if (end < 0)
            return {};
        if (keep)
            out.append(in.constData() + p, end - p);
        p = end;
    }
    return {};
}

inline QByteArray stripWebpMetadata(const QByteArray &in)
{
    if (in.size() < 12 || !in.startsWith("RIFF")
        || std::memcmp(in.constData() + 8, "WEBP", 4) != 0)
        return {};
    const auto le32 = [](const QByteArray &b, qsizetype i) {
        return quint32(static_cast<unsigned char>(b.at(i)))
            | (quint32(static_cast<unsigned char>(b.at(i + 1))) << 8)
            | (quint32(static_cast<unsigned char>(b.at(i + 2))) << 16)
            | (quint32(static_cast<unsigned char>(b.at(i + 3))) << 24);
    };
    const qsizetype end = 8 + qsizetype(le32(in, 4));
    if (end < 12 || end > in.size())
        return {};
    QByteArray out = in.left(12);
    qsizetype vp8x = -1;
    qsizetype p = 12;
    while (p < end) {
        if (p + 8 > end)
            return {};
        const quint32 size = le32(in, p + 4);
        const qsizetype next = p + 8 + qsizetype(size) + qsizetype(size & 1u);
        if (next > end)
            return {};
        const QByteArray fourcc = in.mid(p, 4);
        static const QByteArrayList kept{
            QByteArrayLiteral("VP8X"), QByteArrayLiteral("ANIM"),
            QByteArrayLiteral("ANMF"), QByteArrayLiteral("ALPH"),
            QByteArrayLiteral("VP8 "), QByteArrayLiteral("VP8L"),
            QByteArrayLiteral("ICCP") };
        if (kept.contains(fourcc)) {
            if (fourcc == QByteArrayLiteral("VP8X")) {
                if (size < 10 || vp8x >= 0)
                    return {};
                vp8x = out.size();
            }
            out.append(in.constData() + p, next - p);
        }
        p = next;
    }
    if (vp8x < 0)
        return {};
    out[vp8x + 8] = char(static_cast<unsigned char>(out.at(vp8x + 8)) & ~0x0Cu);
    const quint32 riff = quint32(out.size() - 8);
    for (int i = 0; i < 4; ++i)
        out[4 + i] = char((riff >> (8 * i)) & 0xffu);
    return out;
}

inline QByteArray stripAnimationMetadata(const QByteArray &bytes)
{
    if (isGif(bytes))
        return stripGifMetadata(bytes);
    if (bytes.startsWith("RIFF"))
        return stripWebpMetadata(bytes);
    return {};
}

}
