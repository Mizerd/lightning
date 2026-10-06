#pragma once

// Local SVG -> PNG conversion for every picture the user UPLOADS ("rasterize
// on send"). A recipient only ever receives a PNG; SVG that arrives from
// anyone else stays refused exactly as before (CLAUDE.md §6, MediaBridge's
// shape sniff, Rust's sniff_image_mime). Nothing in this header is reachable
// from a receive path.
//
// The input is the user's own local file, but that file may have come from
// anywhere (downloaded from the web), so it is treated as hostile:
//   * `screen()` refuses, before QtSvg sees a byte: gzip (SVGZ), documents
//     over the size, element, `<use>` or depth bounds, external entities,
//     entity expansion past a budget, processing instructions, every
//     `<image>`/`<feImage>` (QtSvg would load a raster from the local disk and
//     draw it into the picture that is then uploaded), every reference that is
//     not document-internal, and CSS `@import`/external `url()`.
//   * `rasterize()` renders with the SVG 1.2 Tiny feature subset, no
//     animation, straight into a QImage at the target size. The size comes
//     from the document's declared size (width/height with units, else the
//     viewBox), clamped, aspect ratio preserved.
//   * The conversion runs in a HELPER PROCESS (the app binary with the hidden
//     `--rasterize-svg` flag, see SvgRasterJob.h) that is killed on a timeout,
//     because QtSvg cannot be interrupted and a pathological file (nested
//     `<use>` multiplication) can otherwise hold a thread for ever.
//
// Rendering needs Qt SVG (`LIGHTNING_HAVE_QT_SVG`, set by CMake when the module
// is linked). Without it `rasterize()` reports "unavailable". The screen is
// Qt Core only and always compiled.

#include <QByteArray>
#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

#if defined(LIGHTNING_HAVE_QT_SVG)
#include <QPainter>
#include <QSvgRenderer>
#endif

namespace lightning::svgraster {

/// Largest SVG source converted. Bounds parse and render time together with
/// the element, `<use>` and depth caps.
inline constexpr qsizetype kMaxSourceBytes = 2 * 1024 * 1024;
inline constexpr int kMaxElements = 20000;
inline constexpr int kMaxUseElements = 1000;
inline constexpr int kMaxDepth = 64;
/// The most characters internal entities may expand to across a document.
/// QXmlStreamReader caps each reference (4096), not the total.
inline constexpr qsizetype kMaxEntityExpansion = 2 * 1024 * 1024;
inline constexpr qsizetype kEntityReferenceLimit = 4096;
/// The longest side a conversion may ever produce, whatever the destination
/// asks for or the file declares.
inline constexpr int kAbsoluteMaxEdge = 4096;
/// A PNG bigger than this is refused rather than uploaded.
inline constexpr qsizetype kMaxPngBytes = 24 * 1024 * 1024;
/// The helper process is killed after this long.
inline constexpr int kDefaultTimeoutMs = 10000;

/// Output size policy: the longest side of the PNG is the document's own
/// longest side, raised to `minEdge` (a vector is drawn at the target size, so
/// a 24x24 icon is crisp, not an upscaled raster) and lowered to `maxEdge`.
struct Policy {
    int minEdge = 1024;
    int maxEdge = 4096;
};

/// A chat attachment: at least 1024, at most 4096.
inline constexpr Policy attachmentPolicy() { return { 1024, 4096 }; }
/// The source an avatar or banner is cropped from. The crop output is capped
/// at 512, so this leaves room to zoom in.
inline constexpr Policy cropSourcePolicy() { return { 1024, 2048 }; }
/// A chat background: screen scale, and the backdrop pipeline's own ceiling.
inline constexpr Policy backgroundPolicy() { return { 2560, 2560 }; }
/// A sticker or emote.
inline constexpr Policy stickerPolicy() { return { 512, 1024 }; }

/// True for the MIME types QMimeDatabase reports for SVG and SVGZ.
inline bool isSvgMime(const QString &mime)
{
    return mime.startsWith(QLatin1String("image/svg"), Qt::CaseInsensitive);
}

/// True when `bytes` look like an SVG document by SHAPE: after a BOM and
/// whitespace the first character is `<`. Used to route a picked file whose
/// raster sniff failed; it grants nothing (the screen decides).
inline bool looksLikeSvg(const QByteArray &bytes)
{
    qsizetype i = 0;
    if (bytes.startsWith("\xEF\xBB\xBF"))
        i = 3;
    while (i < bytes.size()
           && (bytes.at(i) == ' ' || bytes.at(i) == '\t' || bytes.at(i) == '\r'
               || bytes.at(i) == '\n'))
        ++i;
    return i < bytes.size() && bytes.at(i) == '<';
}

/// True for gzip magic: an SVGZ (or any gzip) file. Refused, not inflated.
inline bool isGzip(const QByteArray &bytes)
{
    return bytes.size() >= 2 && static_cast<unsigned char>(bytes.at(0)) == 0x1F
        && static_cast<unsigned char>(bytes.at(1)) == 0x8B;
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

inline bool isTextElement(QStringView name)
{
    return name == QLatin1String("text") || name == QLatin1String("tspan")
        || name == QLatin1String("tref") || name == QLatin1String("textPath")
        || name == QLatin1String("flowRoot") || name == QLatin1String("flowPara")
        || name == QLatin1String("flowSpan") || name == QLatin1String("textArea");
}

} // namespace detail

/// Why `bytes` must not be rasterised, or an empty string when it may be.
/// Qt Core only; no rendering happens here. `allowText` is false in a process
/// that has no QGuiApplication (Qt aborts when text needs a font database).
inline QString screen(const QByteArray &bytes, bool allowText = true)
{
    if (bytes.isEmpty())
        return QStringLiteral("empty");
    if (bytes.size() > kMaxSourceBytes)
        return QStringLiteral("too_large");
    if (isGzip(bytes))
        return QStringLiteral("compressed");

    QXmlStreamReader xml(bytes);
    int depth = 0;
    int elements = 0;
    int uses = 0;
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
            if (!allowText && detail::isTextElement(name))
                return QStringLiteral("text");
            if (name == QLatin1String("use") && ++uses > kMaxUseElements)
                return QStringLiteral("too_many_elements");
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

namespace detail {

/// One SVG length as CSS pixels (96 dpi). Returns a negative value for a
/// percentage (no intrinsic size) and NaN when it does not parse.
inline double lengthToPixels(QStringView text)
{
    const QStringView t = text.trimmed();
    qsizetype i = 0;
    const qsizetype n = t.size();
    if (i < n && (t.at(i) == u'+' || t.at(i) == u'-'))
        ++i;
    const qsizetype digitsStart = i;
    while (i < n && (t.at(i).isDigit() || t.at(i) == u'.'))
        ++i;
    if (i == digitsStart)
        return std::nan("");
    // An exponent only when `e` is followed by a digit or sign: `em` and `ex`
    // are units.
    if (i + 1 < n && (t.at(i) == u'e' || t.at(i) == u'E')
        && (t.at(i + 1).isDigit()
            || ((t.at(i + 1) == u'-' || t.at(i + 1) == u'+') && i + 2 < n
                && t.at(i + 2).isDigit()))) {
        i += 2;
        while (i < n && t.at(i).isDigit())
            ++i;
    }
    bool ok = false;
    const double value = t.left(i).toDouble(&ok);
    if (!ok || !std::isfinite(value))
        return std::nan("");
    const QString unit = t.mid(i).trimmed().toString().toLower();
    if (unit.isEmpty() || unit == QLatin1String("px"))
        return value;
    if (unit == QLatin1String("pt"))
        return value * 96.0 / 72.0;
    if (unit == QLatin1String("pc"))
        return value * 16.0;
    if (unit == QLatin1String("mm"))
        return value * 96.0 / 25.4;
    if (unit == QLatin1String("cm"))
        return value * 96.0 / 2.54;
    if (unit == QLatin1String("in"))
        return value * 96.0;
    if (unit == QLatin1String("em"))
        return value * 16.0;
    if (unit == QLatin1String("ex"))
        return value * 8.0;
    if (unit == QLatin1String("%"))
        return -1.0;
    return std::nan("");
}

} // namespace detail

/// The document's own size in CSS pixels, from the root's width/height (units
/// honoured) and viewBox. A missing or percentage dimension follows the
/// viewBox, keeping its aspect ratio. An invalid size means the document
/// declares none. Values are NOT clamped here; `targetSize` does that.
inline QSizeF declaredSize(const QByteArray &bytes)
{
    QXmlStreamReader xml(bytes);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (xml.name() != QLatin1String("svg"))
            return {};
        const double w = detail::lengthToPixels(
            xml.attributes().value(QLatin1String("width")));
        const double h = detail::lengthToPixels(
            xml.attributes().value(QLatin1String("height")));
        double vw = 0, vh = 0;
        const QString box =
            xml.attributes().value(QLatin1String("viewBox")).toString();
        if (!box.isEmpty()) {
            QString normalised = box;
            normalised.replace(u',', u' ');
            const QStringList parts =
                normalised.split(u' ', Qt::SkipEmptyParts);
            if (parts.size() == 4) {
                bool a = false, b = false;
                vw = parts.at(2).toDouble(&a);
                vh = parts.at(3).toDouble(&b);
                if (!a || !b || !std::isfinite(vw) || !std::isfinite(vh)
                    || vw <= 0 || vh <= 0)
                    vw = vh = 0;
            }
        }
        const bool haveW = std::isfinite(w) && w > 0;
        const bool haveH = std::isfinite(h) && h > 0;
        const bool haveBox = vw > 0 && vh > 0;
        if (haveW && haveH)
            return { w, h };
        if (haveW && haveBox)
            return { w, w * vh / vw };
        if (haveH && haveBox)
            return { h * vw / vh, h };
        if (haveBox)
            return { vw, vh };
        return {};
    }
    return {};
}

/// The PNG size for a document of `intrinsic` size: the longest side becomes
/// `clamp(longest, policy.minEdge, policy.maxEdge)` (never above
/// kAbsoluteMaxEdge), the aspect ratio is kept to within one pixel, and
/// neither side is below 1. Invalid input gives an invalid size.
inline QSize targetSize(const QSizeF &intrinsic, Policy policy)
{
    const double w = intrinsic.width();
    const double h = intrinsic.height();
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0)
        return {};
    const int hi = std::clamp(policy.maxEdge, 1, kAbsoluteMaxEdge);
    const int lo = std::clamp(policy.minEdge, 1, hi);
    const double longest = std::max(w, h);
    const double edge = std::clamp(longest, double(lo), double(hi));
    const double scale = edge / longest;
    const int tw = std::clamp(int(std::lround(w * scale)), 1, hi);
    const int th = std::clamp(int(std::lround(h * scale)), 1, hi);
    return { tw, th };
}

struct Result {
    QByteArray png;   // empty on refusal
    QSize size;       // of `png`
    QSize intrinsic;  // the document's own size, rounded, for diagnostics
    QString refusal;  // empty on success: a short reason code
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

/// Screen, then draw the document at `targetSize` into a transparent image and
/// encode a PNG. Paints into a QImage only, so it is safe off the GUI thread
/// (it still cannot be interrupted: callers run it in the helper process).
inline Result rasterize(const QByteArray &bytes, Policy policy,
                        bool allowText = true)
{
    Result out;
    out.refusal = screen(bytes, allowText);
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
    QSizeF intrinsic = declaredSize(bytes);
    if (!intrinsic.isValid() || intrinsic.isEmpty())
        intrinsic = renderer.viewBoxF().size();
    if (!intrinsic.isValid() || intrinsic.isEmpty())
        intrinsic = QSizeF(renderer.defaultSize());
    const QSize box = targetSize(intrinsic, policy);
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
        // The box has the document's aspect to within the rounding of the
        // longest side's scale (under half a pixel), so filling it is not a
        // stretch; KeepAspectRatio would letterbox that half pixel instead.
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
    if (png.isEmpty() || png.size() > kMaxPngBytes) {
        out.refusal = QStringLiteral("too_large_output");
        return out;
    }
    out.png = png;
    out.size = box;
    out.intrinsic = intrinsic.toSize();
#else
    Q_UNUSED(policy)
    out.refusal = QStringLiteral("unavailable");
#endif
    return out;
}

/// The sentence shown for a refusal code. Every code `screen`, `rasterize`
/// and the helper job can produce maps to one; unknown codes get the general
/// one, never the raw code.
inline QString userMessage(const QString &reason)
{
    if (reason == QLatin1String("too_large"))
        return QCoreApplication::translate("SvgRaster", "That SVG is too large to convert (the limit is 2 MB).");
    if (reason == QLatin1String("compressed"))
        return QCoreApplication::translate("SvgRaster", "Compressed SVG (.svgz) can't be converted. Save it as a "
                 "plain .svg and try again.");
    if (reason == QLatin1String("too_many_elements")
        || reason == QLatin1String("too_deep")
        || reason == QLatin1String("entities") || reason == QLatin1String("timeout"))
        return QCoreApplication::translate("SvgRaster", "That SVG is too complex to convert.");
    if (reason == QLatin1String("external_image")
        || reason == QLatin1String("external_reference"))
        return QCoreApplication::translate("SvgRaster", "That SVG links to other files or images, which Lightning "
                 "won't load. Embed or remove them and try again.");
    if (reason == QLatin1String("text"))
        return QCoreApplication::translate("SvgRaster", "That SVG contains text, which can't be converted on this "
                 "platform. Convert the text to outlines and try again.");
    if (reason == QLatin1String("unavailable"))
        return QCoreApplication::translate("SvgRaster", "This build of Lightning can't convert SVG pictures.");
    if (reason == QLatin1String("empty") || reason == QLatin1String("not_svg")
        || reason == QLatin1String("malformed")
        || reason == QLatin1String("processing_instruction"))
        return QCoreApplication::translate("SvgRaster", "That file isn't a valid SVG.");
    return QCoreApplication::translate("SvgRaster", "That SVG uses features Lightning can't convert.");
}

} // namespace lightning::svgraster
