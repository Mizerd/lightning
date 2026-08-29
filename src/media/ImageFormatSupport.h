#pragma once

// ONE table of the raster image formats Lightning identifies, and ONE way to
// ask whether the RUNNING BUILD can actually decode them.
//
// WHY THIS EXISTS. A Qt image format is a dlopen'd plugin, so what a build can
// decode is decided by PACKAGING, not by source. Up to 0.8.0 every Linux
// package shipped exactly the three plugins qtbase itself carries — libqgif,
// libqico, libqjpeg — while Lightning's own byte sniffers ACCEPTED image/webp.
// The client accepted, forwarded and re-uploaded a format it could not draw,
// and nothing said so: the AppImage's usr/plugins/imageformats holds three
// files and no check ever looked. Windows had staged qwebp.dll all along and
// macdeployqt copies libqwebp.dylib, so the disagreement existed on Linux only
// — which is exactly why it survived: the dev shell decodes 92 formats because
// the MAINTAINER'S NixOS system profile has kimageformats installed, not
// because anything in this repository provides it.
//
// THE SET GENUINELY DIFFERS PER PLATFORM AND CANNOT BE HARDCODED:
//   * JPEG XL comes from KDE's kimageformats (kimg_jxl.so). Qt has never
//     shipped a JXL plugin — qt/qtimageformats at v6.11.1 is dds, icns, jp2,
//     macheif, macjp2, mng, tga, tiff, wbmp, webp — and neither Fedora's
//     mingw64 repository nor Homebrew packages a cross/macOS build of
//     kimageformats. So Linux packages decode JPEG XL and Windows and macOS
//     do not.
//   * HEIF is the mirror image: macOS gets `qmacheif` free from Qt (it wraps
//     Apple ImageIO) and no other platform has it.
// A single compiled-in list would therefore be wrong on at least one platform.
// Ask the decoder instead. `matrix-client --image-format-status` prints the
// answer for a packaged artifact.
//
// SVG IS ABSENT ON PURPOSE (CLAUDE.md §6): it must never reach a media path as
// active content, so it is not in the table and `sniffRaster` cannot return it.

#include <QByteArray>
#include <QImageReader>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstring>

namespace lightning::imagefmt {

/// One row of the table. `qtFormat` is the name QImageReader uses, which is
/// also the plugin's key; `mime` is what crosses the wire and what the Rust
/// sniffer reports.
struct RasterFormat {
    const char *mime = nullptr;
    const char *qtFormat = nullptr;
    /// REQUIRED means a build that cannot decode it is broken, not merely
    /// limited: these are the formats Lightning itself sends, saves and
    /// re-uploads, so accepting them without a decoder is the accept/decode
    /// disagreement this file exists to prevent. Optional formats are ones
    /// Lightning only ever RECEIVES, where "this build cannot show it" is a
    /// truthful answer rather than a defect.
    bool required = false;
};

/// The complete table. PNG, JPEG, GIF and BMP need no plugin beyond qtbase's
/// built-ins and its gif plugin; WebP and JPEG XL are the two that packaging
/// has to supply.
inline const RasterFormat *rasterFormats(int *count)
{
    static const RasterFormat kFormats[] = {
        { "image/png",  "png",  true  },
        { "image/jpeg", "jpeg", true  },
        { "image/gif",  "gif",  true  },
        { "image/bmp",  "bmp",  true  },
        { "image/webp", "webp", true  },
        // JPEG XL. Optional because it is reachable only through KDE
        // kimageformats, which exists for Linux and for nothing else — see the
        // header comment. A build without it must say so, not pretend.
        { "image/jxl",  "jxl",  false },
    };
    if (count)
        *count = int(sizeof(kFormats) / sizeof(kFormats[0]));
    return kFormats;
}

/// Magic-byte identification. Returns nullptr when the bytes are not one of
/// the listed rasters — SVG, HEIF, AVIF, TIFF and anything unrecognised all
/// land here, and every caller treats that as "refuse", never as "probably
/// fine".
///
/// Deliberately byte-based rather than QImageReader::format(): the reader is
/// PLUGIN-BACKED, so on a build missing a plugin it would answer "not an
/// image" for content that plainly is one, and the accept decision would then
/// swing with the packaging. Identification is a property of the bytes;
/// whether this build can DRAW them is the separate question `canDecode`
/// answers.
inline const RasterFormat *sniffRaster(const QByteArray &bytes)
{
    // Twelve is the longest signature below (the JPEG XL container box), and
    // matching the Rust sniffer's floor keeps the two from disagreeing about
    // a truncated payload.
    if (bytes.size() < 12)
        return nullptr;
    const auto starts = [&bytes](const char *magic, int len) {
        return std::memcmp(bytes.constData(), magic, size_t(len)) == 0;
    };
    const auto at = [&bytes](int i) {
        return static_cast<unsigned char>(bytes.at(i));
    };

    int n = 0;
    const RasterFormat *table = rasterFormats(&n);
    const auto row = [table, n](const char *mime) -> const RasterFormat * {
        for (int i = 0; i < n; ++i)
            if (std::strcmp(table[i].mime, mime) == 0)
                return &table[i];
        return nullptr;
    };

    if (starts("\x89PNG\r\n\x1a\n", 8))
        return row("image/png");
    if (at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF)
        return row("image/jpeg");
    if (starts("GIF87a", 6) || starts("GIF89a", 6))
        return row("image/gif");
    if (starts("RIFF", 4) && std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0)
        return row("image/webp");
    // JPEG XL, both shapes, VERIFIED against real cjxl output rather than
    // taken from a spec summary:
    //   bare codestream  FF 0A                                   (lossy and -d 0)
    //   ISOBMFF container 00 00 00 0C "JXL " 0D 0A 87 0A, then "ftypjxl "
    // Note bytes[4..8] of the container form is "JXL ", NOT "ftyp", so the A/V
    // container probe in MediaBridge cannot mistake a .jxl for an MP4.
    //
    // The codestream signature is only TWO bytes, which is weak — no weaker
    // than the "BM" already accepted below, and a false positive costs a
    // declared image/jxl the decoder then refuses, never a wrong decode.
    if (starts("\x00\x00\x00\x0c\x4a\x58\x4c\x20\x0d\x0a\x87\x0a", 12))
        return row("image/jxl");
    if (at(0) == 0xFF && at(1) == 0x0A)
        return row("image/jxl");
    if (starts("BM", 2))
        return row("image/bmp");
    return nullptr;
}

/// Convenience: the MIME string, or an empty QString.
inline QString sniffRasterMime(const QByteArray &bytes)
{
    const RasterFormat *f = sniffRaster(bytes);
    return f ? QString::fromLatin1(f->mime) : QString();
}

/// The Qt format key, or an empty QString. Hand this to
/// `QImageReader::setFormat` with `setAutoDetectImageFormat(false)` so the
/// sniffed answer stays authoritative and no other plugin re-guesses.
inline QString sniffRasterQtFormat(const QByteArray &bytes)
{
    const RasterFormat *f = sniffRaster(bytes);
    return f ? QString::fromLatin1(f->qtFormat) : QString();
}

/// PURE, so a test can pin the policy without depending on which plugins the
/// host happens to have installed — the property that let this defect hide for
/// a year.
inline bool canDecodeWith(const QSet<QString> &available, const QString &qtFormat)
{
    return !qtFormat.isEmpty() && available.contains(qtFormat.toLower());
}

/// What THIS process can decode, asked of Qt once. Cached because
/// `supportedImageFormats()` walks the plugin directory, and the timeline asks
/// per image.
///
/// Requires a QCoreApplication to exist (plugin loading needs library paths);
/// every caller in Lightning runs well after that.
inline const QSet<QString> &decodableQtFormats()
{
    static const QSet<QString> kAvailable = [] {
        QSet<QString> s;
        const QList<QByteArray> formats = QImageReader::supportedImageFormats();
        for (const QByteArray &f : formats)
            s.insert(QString::fromLatin1(f).toLower());
        return s;
    }();
    return kAvailable;
}

inline bool canDecode(const QString &qtFormat)
{
    return canDecodeWith(decodableQtFormats(), qtFormat);
}

/// Formats in the table this build cannot decode, as `image/...` strings.
/// Empty is the healthy answer. PURE overload first, for the same reason.
inline QStringList undecodableWith(const QSet<QString> &available,
                                   bool requiredOnly)
{
    QStringList out;
    int n = 0;
    const RasterFormat *table = rasterFormats(&n);
    for (int i = 0; i < n; ++i) {
        if (requiredOnly && !table[i].required)
            continue;
        if (!canDecodeWith(available, QString::fromLatin1(table[i].qtFormat)))
            out << QString::fromLatin1(table[i].mime);
    }
    return out;
}

inline QStringList undecodable(bool requiredOnly = false)
{
    return undecodableWith(decodableQtFormats(), requiredOnly);
}

} // namespace lightning::imagefmt
