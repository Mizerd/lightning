#include "media/MediaImageProvider.h"

#include "app/GuiStallTracer.h"

#include "media/ImageFormatSupport.h"
#include "media/MediaBridge.h"

#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

namespace {
// Hard decode bound: no attachment may decode above this edge length.
constexpr int kMaxDecodeEdge = 4096;
// Well below Qt's 256 MiB default; the bytes are attacker-chosen.
constexpr int kMaxDecodeAllocationMiB = 64;

// Bakes the avatar shape into the decoded bitmap (square crop plus rounded
// alpha corners), once per image and cached by URL, instead of a per-item
// MultiEffect mask costing two render passes per avatar per frame.
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

// Rounds a message image's corners without cropping; radius = radiusRatio *
// min(w, h). Baked once per image, cached by URL.
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

// Resolves the QML `sourceSize` against the source dimensions. QML's idiom for
// "scale to this width" leaves the other axis 0, and QSize::isEmpty() is true
// then, so the usual isValid() && !isEmpty() guard would decode at full
// resolution. Upscaling is refused unless a shape mask is baked in, which must
// be rasterized at the requested size to avoid aliased edges.
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

// Image providers run on the GUI thread unless the Image is async, so decodes
// are attributed for stall tracing. stalltrace::Scope is inert off the GUI
// thread.
QImage MediaImageProvider::requestImage(const QString &id, QSize *size,
                                        const QSize &requestedSize)
{
    stalltrace::Scope stallScope("image-decode");
    if (!m_bridge)
        return {};
    QString cacheKey = QUrl::fromPercentEncoding(id.toUtf8());

    // Optional shape suffix from Avatar.qml/SpacesRail.qml: "|shape:circle" or
    // "|shape:rsq:<radius permille of the edge>". Not part of the cache key.
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

    // Strip MediaBridge's "?r=<n>" revision suffix; not part of the cache key.
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

    // The format is decided by the bytes. MediaBridge refuses markup first;
    // this is the second defence where bytes reach a decoder. A recognised
    // raster format is pinned with autodetection off, and sniffRaster's table
    // excludes SVG, so the SVG handler is unreachable even if qsvg ships.
    // Unrecognised formats (e.g. HEIC via qmacheif) may autodetect only after
    // the markup/gzip check has refused SVG and SVGZ.
    const lightning::imagefmt::RasterFormat *sniffed =
        lightning::imagefmt::sniffRaster(bytes);
    if (!sniffed && MediaBridge::looksLikeMarkupOrCompressed(bytes))
        return {};

    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    if (sniffed) {
        reader.setAutoDetectImageFormat(false);
        reader.setFormat(QByteArray(sniffed->qtFormat));
    }
    reader.setAutoTransform(true);
    // Attacker-chosen bytes: cap the allocation as well as the edge.
    reader.setAllocationLimit(kMaxDecodeAllocationMiB);

    // Bound the decode to the requested size and never beyond the safety edge.
    const QSize natural = reader.size();
    // An unreadable header means the edge cap cannot apply; refuse.
    if (!natural.isValid())
        return {};
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
