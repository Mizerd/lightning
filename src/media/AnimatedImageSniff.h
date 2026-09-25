#pragma once

// Byte-level facts about animated images, decided without a decoder: whether
// a payload is a GIF or an animated WebP, whether it has more than one frame,
// and its canvas size. Shared by MediaBridge (display) and ImageCropper
// (upload), so what Lightning uploads as an animation is what it will animate.
//
// Magic only, never a declared mimetype: avatar and banner mimetypes are
// absent or sender-chosen. Nothing here can match SVG (CLAUDE.md §6).

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

// RIFF/WEBP with a VP8X chunk whose animation bit (0x02) is set.
inline bool isAnimatedWebp(const QByteArray &bytes)
{
    return bytes.size() >= 30 && bytes.startsWith("RIFF")
        && std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0
        && std::memcmp(bytes.constData() + 12, "VP8X", 4) == 0
        && (static_cast<unsigned char>(bytes.at(20)) & 0x02u) != 0;
}

// Image descriptors in a GIF, counted up to `stopAt`. Walks the block
// structure (no LZW decode), so a single-frame GIF is told apart from an
// animation cheaply. A truncated file counts the frames it completed.
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
    // Data sub-blocks end with a zero length; false when the bytes run out.
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
        if (tag == 0x3B) // trailer
            break;
        if (tag == 0x21) { // extension: label, then sub-blocks
            ++p;
            if (!skipSubBlocks(p))
                break;
        } else if (tag == 0x2C) { // image descriptor
            if (p + 9 > n)
                break;
            const unsigned char imageFlags = u8(p + 8);
            p += 9;
            if (imageFlags & 0x80u)
                p += 3 * (qsizetype(1) << ((imageFlags & 0x07u) + 1));
            ++p; // LZW minimum code size
            if (p > n || !skipSubBlocks(p))
                break;
            if (++frames >= stopAt)
                return frames;
        } else {
            break; // not a GIF block: stop, keep what was counted
        }
    }
    return frames;
}

// True for a GIF with at least two frames or an animated WebP. A single-frame
// GIF is a still picture and takes the ordinary image path.
inline bool isAnimation(const QByteArray &bytes)
{
    return isAnimatedWebp(bytes) || gifFrameCount(bytes, 2) >= 2;
}

// File suffix for an animation ("gif", "webp"), or "".
inline QString animationSuffix(const QByteArray &bytes)
{
    if (isAnimatedWebp(bytes))
        return QStringLiteral("webp");
    if (gifFrameCount(bytes, 2) >= 2)
        return QStringLiteral("gif");
    return {};
}

// The canvas every frame is drawn into: the GIF logical screen, or the WebP
// VP8X canvas. Invalid when neither header is present.
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

// Pixel-area ceilings for playing an animation. Every frame decodes to a
// canvas-sized buffer, so this bounds the memory one playing item can hold.
// An avatar is drawn at 200 px at most, so 512 x 512 covers 2x DPR.
constexpr qint64 kAvatarMaxPixels = 512LL * 512;         // 1 MiB a frame
constexpr qint64 kBannerMaxPixels = 4LL * 1024 * 1024;   // 16 MiB a frame

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
    // End of the sub-block chain starting at q, or -1 when it runs out.
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
        if (tag == 0x3B) { // trailer: nothing after it is kept
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
                // Application extension: an 11-byte identifier block first.
                if (p + 3 + 11 > n || u8(p + 2) != 11)
                    return {};
                const QByteArray id = in.mid(p + 3, 11);
                keep = id == QByteArrayLiteral("NETSCAPE2.0")
                    || id == QByteArrayLiteral("ANIMEXTS1.0");
                end = chainEnd(p + 3 + 11);
            } else {
                keep = label == 0xF9; // graphic control: delays, disposal
                end = chainEnd(p + 2);
            }
        } else if (tag == 0x2C) {
            if (p + 10 > n)
                return {};
            const unsigned char imageFlags = u8(p + 9);
            qsizetype q = p + 10;
            if (imageFlags & 0x80u)
                q += 3 * (qsizetype(1) << ((imageFlags & 0x07u) + 1));
            ++q; // LZW minimum code size
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
    return {}; // no trailer: truncated
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
        // An allowlist: a chunk nobody here knows may carry anything.
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
        return {}; // not an extended WebP, so not an animation
    // Clear the EXIF (0x08) and XMP (0x04) presence flags.
    out[vp8x + 8] = char(static_cast<unsigned char>(out.at(vp8x + 8)) & ~0x0Cu);
    const quint32 riff = quint32(out.size() - 8);
    for (int i = 0; i < 4; ++i)
        out[4 + i] = char((riff >> (8 * i)) & 0xffu);
    return out;
}

// The animation without its metadata, or empty (not an animation format, or
// malformed).
inline QByteArray stripAnimationMetadata(const QByteArray &bytes)
{
    if (isGif(bytes))
        return stripGifMetadata(bytes);
    if (bytes.startsWith("RIFF"))
        return stripWebpMetadata(bytes);
    return {};
}

} // namespace lightning::animsniff
