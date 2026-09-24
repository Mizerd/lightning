#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

// Validation and normalization for the user-selected custom app icon.
//
// - The format comes from magic-byte sniffing, never the file name or MIME.
// - Only static raster formats are accepted; SVG is rejected outright.
// - Decoding is bounded in bytes, dimensions and allocation.
// - Output matches the default logo: center-cropped, 512x512, circular alpha.
namespace appicon {

// Byte and dimension bounds for the untrusted input.
inline constexpr qsizetype kMaxInputBytes = 32LL * 1024 * 1024;
inline constexpr int kMaxInputEdge = 8192;
inline constexpr int kMinInputEdge = 16;
inline constexpr int kNormalizedEdge = 512;

struct NormalizeResult {
    bool ok = false;
    // Stable machine category on failure: "empty", "too_large_bytes",
    // "unsupported_format", "svg_rejected", "format_not_decodable",
    // "decode_failed", "too_small", "too_large_dimensions". Empty on success.
    // "format_not_decodable" means this build lacks the image plugin;
    // "decode_failed" means the bytes are broken.
    QString category;
    QImage image; // 512x512 ARGB32 with circular alpha when ok
};

// Returns the Qt format name ("png", "jpeg", "webp", "bmp", "gif", "jxl") or
// empty when the bytes are not an accepted raster format. Refuses markup, then
// delegates to lightning::imagefmt::sniffRasterQtFormat.
QString sniffedRasterFormat(const QByteArray &bytes);

// Full pipeline: sniff -> bounded decode -> center-crop -> scale ->
// circular mask.
NormalizeResult normalizeIconBytes(const QByteArray &bytes);

} // namespace appicon
