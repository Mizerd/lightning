#pragma once

// One table of the raster formats Lightning identifies, and one way to ask
// whether the running build can decode them.
//
// Qt image formats are plugins, so decode support is decided by packaging,
// not source; accepting a format the build cannot draw must be detectable.
// The set differs per platform and cannot be hardcoded:
//   * JPEG XL comes only from KDE's kimageformats (kimg_jxl.so), available on
//     Linux packages but not Windows or macOS.
//   * HEIF is the reverse: macOS gets `qmacheif` from Qt, nothing else does.
// So ask the decoder. `lightning-matrix --image-format-status` reports the
// answer for a packaged artifact.
//
// SVG is absent on purpose (CLAUDE.md §6): it is active content and
// `sniffRaster` can never return it.

#include <QByteArray>
#include <QImageReader>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstring>

namespace lightning::imagefmt {

/// One row. `qtFormat` is QImageReader's name (and the plugin key); `mime` is
/// what crosses the wire and what the Rust sniffer reports.
struct RasterFormat {
    const char *mime = nullptr;
    const char *qtFormat = nullptr;
    /// Required formats are ones Lightning itself sends, saves and re-uploads;
    /// a build that cannot decode them is broken. Optional formats are only
    /// ever received, where "cannot show it" is a truthful answer.
    bool required = false;
};

/// PNG, JPEG, GIF and BMP come with qtbase; WebP and JPEG XL must be supplied
/// by packaging.
inline const RasterFormat *rasterFormats(int *count)
{
    static const RasterFormat kFormats[] = {
        { "image/png",  "png",  true  },
        { "image/jpeg", "jpeg", true  },
        { "image/gif",  "gif",  true  },
        { "image/bmp",  "bmp",  true  },
        { "image/webp", "webp", true  },
        // Optional: only available via kimageformats on Linux.
        { "image/jxl",  "jxl",  false },
    };
    if (count)
        *count = int(sizeof(kFormats) / sizeof(kFormats[0]));
    return kFormats;
}

/// Magic-byte identification, or nullptr for anything else (SVG, HEIF, AVIF,
/// TIFF, unknown), which every caller refuses. Byte-based rather than
/// QImageReader::format(), which is plugin-backed and would make acceptance
/// depend on packaging. Whether the build can draw it is canDecode's question.
inline const RasterFormat *sniffRaster(const QByteArray &bytes)
{
    // Twelve bytes is the longest signature (the JPEG XL container) and matches
    // the Rust sniffer's floor.
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
    // JPEG XL, both forms (checked against real cjxl output):
    //   bare codestream   FF 0A
    //   ISOBMFF container 00 00 00 0C "JXL " 0D 0A 87 0A, then "ftypjxl "
    // bytes[4..8] of the container are "JXL ", not "ftyp", so MediaBridge's A/V
    // probe cannot mistake it for MP4. The two-byte codestream signature is
    // weak, but a false positive only yields a declared image/jxl the decoder
    // refuses.
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

/// The Qt format key, or "". Use with setAutoDetectImageFormat(false) so no
/// other plugin re-guesses.
inline QString sniffRasterQtFormat(const QByteArray &bytes)
{
    const RasterFormat *f = sniffRaster(bytes);
    return f ? QString::fromLatin1(f->qtFormat) : QString();
}

/// Pure, so the policy is testable regardless of the host's plugins.
inline bool canDecodeWith(const QSet<QString> &available, const QString &qtFormat)
{
    return !qtFormat.isEmpty() && available.contains(qtFormat.toLower());
}

/// What this process can decode, asked of Qt once and cached (the plugin scan
/// is not cheap). Requires a QCoreApplication.
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

/// Table formats this build cannot decode, as `image/...` strings; empty is
/// healthy.
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
