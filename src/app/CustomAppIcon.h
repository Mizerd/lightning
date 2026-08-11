#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

// Validation + normalization for the user-selected custom application icon
// (Settings -> Appearance). Pure functions so the pipeline is unit-testable
// without an AppController or a QGuiApplication window.
//
// Safety contract:
// - The input is untrusted bytes from an arbitrary user-picked file. The
//   format decision comes from byte-level magic sniffing, never from the
//   file name or a claimed MIME type.
// - Only static raster formats are accepted (PNG, JPEG, WebP, BMP, GIF's
//   first frame). SVG is rejected outright: Qt6::Svg is not linked and
//   active vector content has no place in an icon import path.
// - Decoding is bounded (dimension and byte caps + QImageReader allocation
//   limit) so a hostile file cannot force an unbounded allocation.
// - The result is normalized to the same presentation as the default logo:
//   center-cropped square, scaled to 512x512, circular alpha mask with
//   transparent corners.
namespace appicon {

// Byte and dimension bounds for the untrusted input.
inline constexpr qsizetype kMaxInputBytes = 32LL * 1024 * 1024;
inline constexpr int kMaxInputEdge = 8192;
inline constexpr int kMinInputEdge = 16;
inline constexpr int kNormalizedEdge = 512;

struct NormalizeResult {
    bool ok = false;
    // Stable machine category on failure: "empty", "too_large_bytes",
    // "unsupported_format", "svg_rejected", "decode_failed",
    // "too_small", "too_large_dimensions". Empty on success.
    QString category;
    QImage image; // 512x512 ARGB32 with circular alpha when ok
};

// Sniffs the raster format from magic bytes. Returns the canonical Qt
// format name ("png", "jpeg", "webp", "bmp", "gif") or an empty string
// when the bytes are not an accepted static raster format.
QString sniffedRasterFormat(const QByteArray &bytes);

// Full pipeline: sniff -> bounded decode -> center-crop -> scale ->
// circular mask.
NormalizeResult normalizeIconBytes(const QByteArray &bytes);

} // namespace appicon
