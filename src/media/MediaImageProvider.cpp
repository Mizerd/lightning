#include "media/MediaImageProvider.h"

#include "media/MediaBridge.h"

#include <QBuffer>
#include <QImageReader>
#include <QUrl>

namespace {
// Hard decode bound: no attachment may decode above this edge length.
constexpr int kMaxDecodeEdge = 4096;
} // namespace

MediaImageProvider::MediaImageProvider(MediaBridge *bridge)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_bridge(bridge)
{
}

QImage MediaImageProvider::requestImage(const QString &id, QSize *size,
                                        const QSize &requestedSize)
{
    if (!m_bridge)
        return {};
    const QString cacheKey = QUrl::fromPercentEncoding(id.toUtf8());
    QByteArray bytes = m_bridge->cachedBytes(cacheKey);
    if (bytes.isEmpty())
        return {};

    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);

    // Bound the decode: honor the requested size, and never decode beyond
    // the safety edge even when no size was requested.
    const QSize natural = reader.size();
    QSize target = natural;
    if (requestedSize.isValid() && !requestedSize.isEmpty())
        target = natural.isValid()
            ? natural.scaled(requestedSize, Qt::KeepAspectRatio)
            : requestedSize;
    if (target.isValid()
        && (target.width() > kMaxDecodeEdge || target.height() > kMaxDecodeEdge))
        target.scale(kMaxDecodeEdge, kMaxDecodeEdge, Qt::KeepAspectRatio);
    if (target.isValid() && natural.isValid() && target != natural)
        reader.setScaledSize(target);

    const QImage image = reader.read();
    if (size)
        *size = image.size();
    return image;
}
