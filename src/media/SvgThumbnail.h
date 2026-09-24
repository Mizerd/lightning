#pragma once

// Raster thumbnail for an SVG the user is SENDING, so receivers (Lightning,
// Element and others) can show a preview without decoding SVG themselves.
// Received SVG is never rendered (CLAUDE.md §6); nothing here is reachable
// from a receive path.
//
// The input is the user's own local file, but that file may have come from
// anywhere, so it is screened before QtSvg sees it:
//   * QtSvg loads a raster file named by `<image href>` from the local disk
//     (absolute, or relative to the document) and would draw it into the
//     thumbnail that is then uploaded. Every `<image>`/`<feImage>` is
//     refused, and every other reference must be document-internal (`#id`);
//     hyperlinks (`<a href>`) are inert and allowed.
//   * CSS `@import` and `url()` pointing outside the document are refused.
//   * External entities, processing instructions, gzip (SVGZ), malformed XML,
//     and documents over the size, element or depth bounds are refused.
// A refusal only means no thumbnail; the file still sends.
//
// Rendering needs Qt SVG (`LIGHTNING_HAVE_QT_SVG`, set by CMake when the
// module is linked). Without it `render()` reports "unavailable". The screen
// is Qt Core only and always compiled.

#include <QByteArray>
#include <QBuffer>
#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringView>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

#if defined(LIGHTNING_HAVE_QT_SVG)
#include <QPainter>
#include <QSvgRenderer>
#endif

namespace lightning::svgthumb {

/// Largest SVG source rasterised. Bounds parse and render time together with
/// the element and depth caps.
inline constexpr qsizetype kMaxSourceBytes = 4 * 1024 * 1024;
inline constexpr int kMaxElements = 20000;
inline constexpr int kMaxDepth = 64;
/// The most characters internal entities may expand to across a document.
/// QXmlStreamReader caps each reference (4096), not the total.
inline constexpr qsizetype kMaxEntityExpansion = 2 * 1024 * 1024;
inline constexpr qsizetype kEntityReferenceLimit = 4096;
/// Element's thumbnail box: fit within 800x600, never upscaled.
inline constexpr int kMaxThumbWidth = 800;
inline constexpr int kMaxThumbHeight = 600;
/// Matches the Rust side's poster bound (`rooms::MAX_POSTER_BYTES`); a larger
/// encode would be dropped there.
inline constexpr qsizetype kMaxThumbBytes = 2 * 1024 * 1024;
/// Declared intrinsic sizes beyond this are clamped for the event metadata.
inline constexpr int kMaxIntrinsicEdge = 65535;

/// True for the MIME types QMimeDatabase reports for SVG and SVGZ.
inline bool isSvgMime(const QString &mime)
{
    return mime.startsWith(QLatin1String("image/svg"), Qt::CaseInsensitive);
}

namespace detail {

inline bool isXmlSpace(QChar c)
{
    return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n';
}

// Every `url(` in a value must point inside the document (`url(#id)`).
inline bool hasExternalUrl(QStringView text)
{
    qsizetype from = 0;
    while (true) {
        const qsizetype at =
            text.indexOf(QLatin1String("url("), from, Qt::CaseInsensitive);
        if (at < 0)
            return false;
        qsizetype i = at + 4;
        while (i < text.size()
               && (isXmlSpace(text.at(i)) || text.at(i) == u'"'
                   || text.at(i) == u'\''))
            ++i;
        if (i >= text.size() || text.at(i) != u'#')
            return true;
        from = i;
    }
}

inline bool hasCssImport(QStringView text)
{
    return text.contains(QLatin1String("@import"), Qt::CaseInsensitive);
}

inline bool isInternalReference(QStringView value)
{
    const QStringView trimmed = value.trimmed();
    return trimmed.startsWith(u'#');
}

} // namespace detail

/// Why `bytes` must not be rasterised, or an empty string when it may be.
/// Qt Core only; no rendering happens here.
inline QString screen(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return QStringLiteral("empty");
    if (bytes.size() > kMaxSourceBytes)
        return QStringLiteral("too_large");
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes.at(0)) == 0x1F
        && static_cast<unsigned char>(bytes.at(1)) == 0x8B)
        return QStringLiteral("compressed");

    QXmlStreamReader xml(bytes);
    int depth = 0;
    int elements = 0;
    // Depth of the open <style>, or 0: CSS runs until </style>, not until
    // the next end tag.
    int styleDepth = 0;
    bool sawRoot = false;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::DTD: {
            // Internal entities (older Illustrator exports declare namespace
            // shorthands) expand under the reader's expansion limit and are
            // screened in place like the rest. External ones are refused, and
            // so is a document whose references could expand past the budget,
            // checked here before any expansion allocates.
            const auto declarations = xml.entityDeclarations();
            for (const QXmlStreamEntityDeclaration &entity : declarations) {
                if (!entity.systemId().isEmpty() || !entity.publicId().isEmpty()
                    || !entity.notationName().isEmpty())
                    return QStringLiteral("entities");
            }
            if (!declarations.isEmpty()
                && bytes.count('&') * kEntityReferenceLimit > kMaxEntityExpansion)
                return QStringLiteral("entities");
            break;
        }
        case QXmlStreamReader::EntityReference:
            return QStringLiteral("entities");
        case QXmlStreamReader::ProcessingInstruction:
            return QStringLiteral("processing_instruction");
        case QXmlStreamReader::StartElement: {
            ++depth;
            ++elements;
            if (depth > kMaxDepth)
                return QStringLiteral("too_deep");
            if (elements > kMaxElements)
                return QStringLiteral("too_many_elements");
            // The local name whatever the prefix or namespace, so a prefixed
            // spelling cannot pass as something else.
            const QStringView name = xml.name();
            if (!sawRoot) {
                if (name != QLatin1String("svg"))
                    return QStringLiteral("not_svg");
                sawRoot = true;
            }
            if (name == QLatin1String("image")
                || name == QLatin1String("feImage"))
                return QStringLiteral("external_image");
            const bool hyperlink = name == QLatin1String("a");
            const auto attributes = xml.attributes();
            for (const QXmlStreamAttribute &attribute : attributes) {
                const QStringView value = attribute.value();
                if (attribute.name() == QLatin1String("href") && !hyperlink
                    && !detail::isInternalReference(value))
                    return QStringLiteral("external_reference");
                if (detail::hasExternalUrl(value) || detail::hasCssImport(value))
                    return QStringLiteral("external_reference");
            }
            if (styleDepth == 0 && name == QLatin1String("style"))
                styleDepth = depth;
            break;
        }
        case QXmlStreamReader::EndElement:
            if (depth == styleDepth)
                styleDepth = 0;
            --depth;
            break;
        case QXmlStreamReader::Characters:
            if (styleDepth > 0
                && (detail::hasExternalUrl(xml.text())
                    || detail::hasCssImport(xml.text())))
                return QStringLiteral("external_reference");
            break;
        default:
            break;
        }
    }
    if (xml.hasError())
        return QStringLiteral("malformed");
    if (!sawRoot)
        return QStringLiteral("not_svg");
    return {};
}

/// The thumbnail box for an intrinsic size: fit within the maxima, keep the
/// aspect ratio, never upscale, at least 1x1. Invalid input gives an invalid
/// size.
inline QSize fitThumbnail(const QSizeF &intrinsic,
                          int maxWidth = kMaxThumbWidth,
                          int maxHeight = kMaxThumbHeight)
{
    const double w = intrinsic.width();
    const double h = intrinsic.height();
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0
        || maxWidth <= 0 || maxHeight <= 0)
        return {};
    const double scale = std::min({ 1.0, maxWidth / w, maxHeight / h });
    const int tw = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int th = std::max(1, static_cast<int>(std::lround(h * scale)));
    return { std::min(tw, maxWidth), std::min(th, maxHeight) };
}

/// The intrinsic size as whole pixels for `info.w`/`info.h`, aspect kept when
/// clamped. Invalid input gives an invalid size.
inline QSize intrinsicPixels(const QSizeF &intrinsic)
{
    return fitThumbnail(intrinsic, kMaxIntrinsicEdge, kMaxIntrinsicEdge);
}

struct Result {
    QByteArray png;      // empty on refusal
    QSize size;          // of `png`
    QSize intrinsic;     // the SVG's own size, for the event metadata
    QString refusal;     // empty on success
};

/// True when this build can rasterise SVG at all.
inline constexpr bool available()
{
#if defined(LIGHTNING_HAVE_QT_SVG)
    return true;
#else
    return false;
#endif
}

/// Screen, then rasterise to a PNG no larger than the thumbnail box. Safe to
/// call off the GUI thread: it paints into a QImage only.
inline Result render(const QByteArray &bytes)
{
    Result out;
    out.refusal = screen(bytes);
    if (!out.refusal.isEmpty())
        return out;
#if defined(LIGHTNING_HAVE_QT_SVG)
    QSvgRenderer renderer;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    // The static SVG 1.2 Tiny subset, no animation.
    renderer.setOptions(QtSvg::Tiny12FeaturesOnly);
    renderer.setAnimationEnabled(false);
#endif
    if (!renderer.load(bytes) || !renderer.isValid()) {
        out.refusal = QStringLiteral("unrenderable");
        return out;
    }
    QSizeF intrinsic = renderer.defaultSize();
    if (intrinsic.isEmpty())
        intrinsic = renderer.viewBoxF().size();
    const QSize box = fitThumbnail(intrinsic);
    if (!box.isValid()) {
        out.refusal = QStringLiteral("no_size");
        return out;
    }
    QImage image(box, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        out.refusal = QStringLiteral("allocation");
        return out;
    }
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(box)));
    }
    QByteArray png;
    {
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
            out.refusal = QStringLiteral("encode");
            return out;
        }
    }
    if (png.isEmpty() || png.size() > kMaxThumbBytes) {
        out.refusal = QStringLiteral("encode");
        return out;
    }
    out.png = png;
    out.size = box;
    out.intrinsic = intrinsicPixels(intrinsic);
#else
    out.refusal = QStringLiteral("unavailable");
#endif
    return out;
}

} // namespace lightning::svgthumb
