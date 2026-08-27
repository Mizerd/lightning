#include "calls/ShareSourceImageProvider.h"

#include "calls/WindowCaptureSrc.h"

namespace {

// Big enough that a tile stays readable on a high-DPI screen, small enough
// that grabbing one per row costs no visible pause. This is the ceiling
// regardless of what the picker asks for, so a mis-sized Image cannot turn a
// 4K desktop into a full-resolution copy.
//
// RAISED for the grid picker: the preview is the thing the user actually
// reads now — a 64px strip next to a caption was not enough to tell one
// browser window from another, which is what the caption problem and the
// tile size problem had in common.
constexpr int kMaxEdge = 640;

} // namespace

ShareSourceImageProvider::ShareSourceImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage ShareSourceImageProvider::requestImage(const QString &id, QSize *size,
                                              const QSize &requestedSize)
{
    // A NULL image is a valid answer everywhere below: the picker draws its
    // glyph instead, which is what it did before previews existed. A window
    // can close between being listed and being drawn, and that is ordinary.
    QImage image;

    const int edge = requestedSize.isValid() && requestedSize.width() > 0
        ? qMin(kMaxEdge, requestedSize.width())
        : kMaxEdge;

    if (id.startsWith(QLatin1Char('w'))) {
        bool ok = false;
        // toULongLong, so a handle above INT_MAX — which every 64-bit HWND
        // is — survives the round trip through the id.
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
