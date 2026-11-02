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

inline constexpr qsizetype kMaxInputBytes = 32LL * 1024 * 1024;
inline constexpr int kMaxInputEdge = 8192;
inline constexpr int kMinInputEdge = 16;
inline constexpr int kNormalizedEdge = 512;

struct NormalizeResult {
    bool ok = false;
    QString category;
    QImage image;
};

QString sniffedRasterFormat(const QByteArray &bytes);

NormalizeResult normalizeIconBytes(const QByteArray &bytes);

}
