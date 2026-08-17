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

// Round the corners of a message image WITHOUT cropping (aspect preserved).
// radius = radiusRatio * min(w, h). Baked once per decoded image, cached by
// source URL — no per-frame mask/effect. Used for timeline image/video media so
// media reads as part of the message rather than a pasted-in rectangle.
QImage roundedCorners(const QImage &src, qreal radiusRatio)
{
    if (src.isNull())
        return src;
    const int w = src.width();
    const int h = src.height();
    if (w <= 0 || h <= 0)
        return src;
    const QImage in = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const qreal radius = radiusRatio * qMin(w, h);
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, w, h), radius, radius);
    painter.setClipPath(path);
    painter.drawImage(0, 0, in);
    painter.end();
    return out;
}

// Resolves the QML `sourceSize` a caller asked for against the source's own
// dimensions.
//
// This exists because the obvious guard is WRONG. QML's documented idiom for
// "scale to this width and keep the aspect" is to set one axis and leave the
// other 0, and every timeline image uses it (`sourceSize.width: 640`). But
// QSize::isEmpty() is true whenever EITHER axis is below 1, so the usual
// `requestedSize.isValid() && !requestedSize.isEmpty()` test rejects exactly
// that idiom — and the decode then silently fell back to the source's full
// resolution, bounded only by kMaxDecodeEdge (4096). A 1.7 MB screenshot was
// decoded to tens of megabytes of pixels and handed to a 348px-wide box, and
// scrolling up through a media-heavy room did dozens of those per gesture.
//
// Upscaling is refused UNLESS a shape is being baked in. A plain image gains
// nothing from being inflated in memory — the scene graph interpolates just
// as well from the source pixels. But a mask baked into the bitmap is
// rasterized once at whatever size it is baked at, so a circular avatar
// resolved from a small source and shown large must still bake at the size
// that was asked for or its edge visibly aliases. That was the pre-existing
// behaviour for avatars and it is preserved deliberately.
QSize effectiveDecodeSize(const QSize &natural, const QSize &requested,
                          bool allowUpscale)
{
    const int rw = qMax(0, requested.width());
    const int rh = qMax(0, requested.height());
    if (rw <= 0 && rh <= 0)
        return natural;                 // nothing asked for
    if (!natural.isValid() || natural.isEmpty())
        return QSize(rw, rh);           // source size unknown; honour the ask

    QSize target;
    if (rw > 0 && rh > 0)
        target = natural.scaled(rw, rh, Qt::KeepAspectRatio);
    else if (rw > 0)
        target = QSize(rw, qMax(1, qRound(double(natural.height()) * rw
                                          / natural.width())));
    else
        target = QSize(qMax(1, qRound(double(natural.width()) * rh
                                      / natural.height())), rh);

    if (!allowUpscale
        && (target.width() >= natural.width()
            || target.height() >= natural.height()))
        return natural;
    return target;
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
    qreal roundRatio = 0.0;   // aspect-preserving corner rounding (message media)
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
        } else if (shape.startsWith(QLatin1String("round:"))) {
            bool ok = false;
            const int permille = shape.mid(6).toInt(&ok);
            if (ok && permille > 0 && permille <= 300)
                roundRatio = permille / 1000.0;
        }
    }

    // Strip the cache-revision suffix MediaBridge appends to provider URLs
    // ("?r=<n>", bumped on every byte re-insert so a QML Image stuck in
    // Error gets a fresh source string). It is not part of the cache key.
    const int revisionPos = cacheKey.lastIndexOf(QLatin1String("?r="));
    if (revisionPos >= 0)
        cacheKey.truncate(revisionPos);

    if (cacheKey.startsWith(QLatin1String("artwork:"))) {
        QImage image = m_bridge->cachedArtwork(cacheKey);
        if (image.isNull())
            return {};
        const bool bakesShape =
            maskCircle || maskRatio > 0.0 || roundRatio > 0.0;
        const QSize artTarget = effectiveDecodeSize(image.size(), requestedSize,
                                                    bakesShape);
        if (artTarget.isValid() && !artTarget.isEmpty()
            && artTarget != image.size()) {
            image = image.scaled(artTarget, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        }
        if (maskCircle || maskRatio > 0.0)
            image = roundedMasked(image, maskCircle, maskRatio);
        else if (roundRatio > 0.0)
            image = roundedCorners(image, roundRatio);
        if (size)
            *size = image.size();
        return image;
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
    const bool bakesShape = maskCircle || maskRatio > 0.0 || roundRatio > 0.0;
    QSize target = effectiveDecodeSize(natural, requestedSize, bakesShape);
    if (target.isValid()
        && (target.width() > kMaxDecodeEdge || target.height() > kMaxDecodeEdge))
        target.scale(kMaxDecodeEdge, kMaxDecodeEdge, Qt::KeepAspectRatio);
    if (target.isValid() && natural.isValid() && target != natural)
        reader.setScaledSize(target);

    QImage image = reader.read();
    if (!image.isNull() && (maskCircle || maskRatio > 0.0))
        image = roundedMasked(image, maskCircle, maskRatio);
    else if (!image.isNull() && roundRatio > 0.0)
        image = roundedCorners(image, roundRatio);
    if (size)
        *size = image.size();
    return image;
}
