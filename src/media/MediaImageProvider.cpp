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
// Qt's default allocation limit is 256 MiB. Nothing decoded here is a
// legitimate quarter-gigabyte image, and the bytes are attacker-chosen.
constexpr int kMaxDecodeAllocationMiB = 64;

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

// Attributed for stall tracing (2026-08-19): a provider of type Image is
// invoked on the GUI thread unless the requesting Image opted into async
// loading, so a burst of 100-700 KB decodes during pagination lands here.
// stalltrace::Scope is inert off the GUI thread, so an async request cannot
// misattribute someone else's stall.
QImage MediaImageProvider::requestImage(const QString &id, QSize *size,
                                        const QSize &requestedSize)
{
    stalltrace::Scope stallScope("image-decode");
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

    // THE FORMAT IS DECIDED BY THE BYTES, AND ONLY FROM THE KNOWN TABLE.
    //
    // MediaBridge refuses markup before anything reaches this cache, and that
    // is the primary defence. This is the second one, at the site that
    // actually hands bytes to a decoder: sniff the raster format ourselves,
    // pin the reader to it, and turn autodetection OFF, so a payload whose
    // shape we do not recognise is refused rather than handed to whichever
    // image plugin claims it. `sniffRaster`'s table deliberately excludes
    // SVG — it is not a raster format and it is active content — so an
    // unrecognised payload cannot reach the SVG handler even on a build that
    // ships qsvg. CustomAppIcon already does exactly this and says why.
    // A FORMAT WE RECOGNISE IS PINNED; ONE WE DO NOT IS STILL REFUSED IF IT
    // COULD BE ACTIVE CONTENT.
    //
    // sniffRaster's table is deliberately narrow — it is the ACCEPT list, and
    // HEIF, AVIF and TIFF are intentionally absent from it while
    // looksLikeAvContainer lets those brands through "if an image plugin ever
    // appears". Refusing everything the table does not name would therefore
    // have blanked a HEIC on macOS, where qmacheif exists and it used to
    // render, with no diagnostic at all.
    //
    // So: a recognised format is pinned with autodetection OFF, which is what
    // keeps an unrecognised payload away from the SVG handler. An
    // unrecognised one falls back to autodetection ONLY after the markup and
    // compressed check has refused it a second time, so SVG and SVGZ cannot
    // reach a decoder either way.
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
    // A decoder is handed attacker-chosen bytes, so cap the allocation as
    // well as the edge. Qt's default is 256 MiB; nothing on this path is a
    // legitimate quarter-gigabyte image.
    reader.setAllocationLimit(kMaxDecodeAllocationMiB);

    // Bound the decode: honor the requested size, and never decode beyond
    // the safety edge even when no size was requested.
    const QSize natural = reader.size();
    // An unreadable header means setScaledSize below is never called and the
    // edge cap never applies, so refuse rather than decode unbounded.
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
