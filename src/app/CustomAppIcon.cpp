#include "app/CustomAppIcon.h"

#include "media/ImageFormatSupport.h"

#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>

namespace appicon {

QString sniffedRasterFormat(const QByteArray &bytes)
{
    if (bytes.size() < 12)
        return {};
    const auto u = [&bytes](int i) { return static_cast<unsigned char>(bytes.at(i)); };
    // Reject anything that starts with markup after whitespace/BOM.
    int first = 0;
    while (first < bytes.size()
           && (u(first) == 0xEF || u(first) == 0xBB || u(first) == 0xBF
               || bytes.at(first) == ' ' || bytes.at(first) == '\n'
               || bytes.at(first) == '\r' || bytes.at(first) == '\t'))
        ++first;
    if (first < bytes.size() && bytes.at(first) == '<')
        return {};
    // One shared table, mirrored by Rust's rooms::sniff_image_mime; see
    // media/ImageFormatSupport.h.
    return lightning::imagefmt::sniffRasterQtFormat(bytes);
}

NormalizeResult normalizeIconBytes(const QByteArray &bytes)
{
    NormalizeResult result;
    if (bytes.isEmpty()) {
        result.category = QStringLiteral("empty");
        return result;
    }
    if (bytes.size() > kMaxInputBytes) {
        result.category = QStringLiteral("too_large_bytes");
        return result;
    }
    // So the category says "svg_rejected" rather than "unsupported_format".
    const QByteArray head = bytes.left(512).toLower();
    if (head.contains("<svg") || head.contains("<?xml")) {
        result.category = QStringLiteral("svg_rejected");
        return result;
    }
    const QString format = sniffedRasterFormat(bytes);
    if (format.isEmpty()) {
        result.category = QStringLiteral("unsupported_format");
        return result;
    }
    // Identified but this build has no plugin for it (image formats are
    // dlopen'd, so this varies by package). Not a corrupt file.
    if (!lightning::imagefmt::canDecode(format)) {
        result.category = QStringLiteral("format_not_decodable");
        return result;
    }

    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format.toLatin1());
    // The sniffed format is authoritative; never let the reader re-guess.
    reader.setAutoDetectImageFormat(false);
    reader.setAutoTransform(true);
    reader.setAllocationLimit(128); // MB — far above any legitimate icon
    const QSize declared = reader.size();
    if (declared.isValid()
        && (declared.width() > kMaxInputEdge || declared.height() > kMaxInputEdge)) {
        result.category = QStringLiteral("too_large_dimensions");
        return result;
    }
    QImage decoded = reader.read();
    if (decoded.isNull()) {
        result.category = QStringLiteral("decode_failed");
        return result;
    }
    if (decoded.width() > kMaxInputEdge || decoded.height() > kMaxInputEdge) {
        result.category = QStringLiteral("too_large_dimensions");
        return result;
    }
    if (decoded.width() < kMinInputEdge || decoded.height() < kMinInputEdge) {
        result.category = QStringLiteral("too_small");
        return result;
    }

    // Center-crop to square.
    const int side = qMin(decoded.width(), decoded.height());
    const QRect crop((decoded.width() - side) / 2, (decoded.height() - side) / 2,
                     side, side);
    QImage square = decoded.copy(crop).convertToFormat(QImage::Format_ARGB32);
    QImage scaled = square.scaled(kNormalizedEdge, kNormalizedEdge,
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // Circular alpha mask, matching the default logo.
    QImage out(kNormalizedEdge, kNormalizedEdge, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QPainterPath circle;
    circle.addEllipse(QRectF(0, 0, kNormalizedEdge, kNormalizedEdge));
    painter.setClipPath(circle);
    painter.drawImage(0, 0, scaled);
    painter.end();

    result.ok = true;
    result.image = out;
    return result;
}

} // namespace appicon
