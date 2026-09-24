#include "calls/ShareSourceImageProvider.h"

#include "calls/WindowCaptureSrc.h"

namespace {

// Ceiling on preview size, whatever the picker requests: readable on high-DPI
// screens, cheap enough to grab per row, and never a full-resolution copy.
constexpr int kMaxEdge = 640;

} // namespace

ShareSourceImageProvider::ShareSourceImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage ShareSourceImageProvider::requestImage(const QString &id, QSize *size,
                                              const QSize &requestedSize)
{
    // A null image is a valid answer (the picker then draws its glyph); a
    // window can close between being listed and drawn.
    QImage image;

    const int edge = requestedSize.isValid() && requestedSize.width() > 0
        ? qMin(kMaxEdge, requestedSize.width())
        : kMaxEdge;

    if (id.startsWith(QLatin1Char('w'))) {
        bool ok = false;
        // toULongLong: 64-bit HWNDs exceed INT_MAX.
        const quint64 handle = QStringView(id).mid(1).toULongLong(&ok);
        if (ok && handle != 0)
            image = lightning::wincap::captureThumbnail(handle, edge);
    } else if (id.startsWith(QLatin1Char('s'))) {
        bool ok = false;
        const int index = QStringView(id).mid(1).toInt(&ok);
        if (ok && index >= 0)
            image = lightning::wincap::captureScreenThumbnail(index, edge);
    }

    if (size)
        *size = image.size();
    return image;
}
