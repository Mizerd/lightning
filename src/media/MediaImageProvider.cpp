#include "media/MediaImageProvider.h"

#include "media/MediaBridge.h"

#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

namespace {
// Hard decode bound: no attachment may decode above this edge length.
constexpr int kMaxDecodeEdge = 4096;

// Bake the avatar shape into the decoded bitmap: centre-crop to a square,
// then cut rounded corners into the alpha channel. Masking here — once per
// decoded image, cached by source URL — replaces the per-item
// MultiEffect+layer mask that cost two extra render passes per avatar on
// every frame of a scroll.
QImage roundedMasked(const QImage &src, bool circle, qreal radiusRatio)
{
    if (src.isNull())
        return src;
    const int edge = qMin(src.width(), src.height());
    if (edge <= 0)
        return src;
    const QRect crop((src.width() - edge) / 2, (src.height() - edge) / 2,
                     edge, edge);
    const QImage squared =
        src.copy(crop).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const qreal radius = circle ? edge / 2.0 : radiusRatio * edge;
    QImage out(edge, edge, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, edge, edge), radius, radius);
    painter.setClipPath(path);
    painter.drawImage(0, 0, squared);
    painter.end();
    return out;
}
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
    QString cacheKey = QUrl::fromPercentEncoding(id.toUtf8());

    // Optional avatar-shape suffix appended by Avatar.qml/SpacesRail.qml:
    // "|shape:circle" or "|shape:rsq:<radius permille of the edge>". The
    // suffix is not part of the cache key; it selects the baked mask.
    bool maskCircle = false;
    qreal maskRatio = 0.0;
    const int shapePos = cacheKey.lastIndexOf(QLatin1String("|shape:"));
    if (shapePos >= 0) {
        const QString shape = cacheKey.mid(shapePos + 7);
        cacheKey.truncate(shapePos);
        if (shape == QLatin1String("circle")) {
            maskCircle = true;
        } else if (shape.startsWith(QLatin1String("rsq:"))) {
            bool ok = false;
            const int permille = shape.mid(4).toInt(&ok);
            if (ok && permille > 0 && permille <= 500)
                maskRatio = permille / 1000.0;
        }
    }

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

    QImage image = reader.read();
    if (!image.isNull() && (maskCircle || maskRatio > 0.0))
        image = roundedMasked(image, maskCircle, maskRatio);
    if (size)
        *size = image.size();
    return image;
}
