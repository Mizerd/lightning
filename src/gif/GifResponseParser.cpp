#include "gif/GifResponseParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

namespace gif {
namespace {

// GIPHY numeric fields (width/height/size) arrive as strings; be liberal.
int asInt(const QJsonValue &v)
{
    if (v.isDouble())
        return static_cast<int>(v.toDouble());
    if (v.isString())
        return v.toString().toInt();
    return 0;
}

qint64 asInt64(const QJsonValue &v)
{
    if (v.isDouble())
        return static_cast<qint64>(v.toDouble());
    if (v.isString())
        return v.toString().toLongLong();
    return 0;
}

int ratingRank(Rating r)
{
    switch (r) {
    case Rating::G:    return 0;
    case Rating::PG:   return 1;
    case Rating::PG13: return 2;
    case Rating::R:    return 3;
    }
    return 3;
}

// Pick the first rendition in `keys` whose `url` is a sendable .gif on one of
// `hosts`.
QString firstGifUrl(const QJsonObject &images, const QStringList &keys,
                    const QStringList &hosts, int &width, int &height,
                    qint64 &bytes)
{
    for (const QString &key : keys) {
        const QJsonObject r = images.value(key).toObject();
        const QString url = stripTracking(r.value(QStringLiteral("url")).toString());
        if (isSendableGifUrlForHosts(url, hosts)) {
            width = asInt(r.value(QStringLiteral("width")));
            height = asInt(r.value(QStringLiteral("height")));
            bytes = asInt64(r.value(QStringLiteral("size")));
            return url;
        }
    }
    return QString();
}

QString firstHttpsUrl(const QJsonObject &images, const QStringList &keys,
                      const QString &field, int &width, int &height)
{
    for (const QString &key : keys) {
        const QJsonObject r = images.value(key).toObject();
        const QString url = stripTracking(r.value(field).toString());
        if (url.startsWith(QLatin1String("https://"))) {
            width = asInt(r.value(QStringLiteral("width")));
            height = asInt(r.value(QStringLiteral("height")));
            return url;
        }
    }
    return QString();
}

} // namespace

Rating ratingFromString(const QString &value)
{
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("g"))
        return Rating::G;
    if (v == QLatin1String("pg"))
        return Rating::PG;
    if (v == QLatin1String("pg-13") || v == QLatin1String("pg13"))
        return Rating::PG13;
    // "r", "nsfw", unknown, empty → most permissive so unknowns are excluded
    // unless the user explicitly allows R.
    return Rating::R;
}

QString ratingToString(Rating r)
{
    switch (r) {
    case Rating::G:    return QStringLiteral("g");
    case Rating::PG:   return QStringLiteral("pg");
    case Rating::PG13: return QStringLiteral("pg-13");
    case Rating::R:    return QStringLiteral("r");
    }
    return QStringLiteral("r");
}

bool ratingWithin(const QString &itemRating, Rating maxRating)
{
    return ratingRank(ratingFromString(itemRating)) <= ratingRank(maxRating);
}

QString stripTracking(const QString &url)
{
    if (url.isEmpty())
        return url;
    QUrl u(url);
    if (!u.isValid())
        return QString();
    u.setQuery(QString());
    u.setFragment(QString());
    return u.toString();
}

bool isSendableGifUrlForHosts(const QString &url,
                              const QStringList &allowedHostSuffixes)
{
    if (!url.startsWith(QLatin1String("https://")))
        return false;
    const QUrl u(url);
    if (!u.isValid())
        return false;
    const QString host = u.host().toLower();
    // Provider-CDN host policy: only the provider's own media hosts. A suffix
    // like ".giphy.com" also matches the bare apex "giphy.com".
    bool hostOk = false;
    for (const QString &suffix : allowedHostSuffixes) {
        const QString bare = suffix.startsWith(QLatin1Char('.'))
            ? suffix.mid(1) : suffix;
        if (host == bare || host.endsWith(suffix)) {
            hostOk = true;
            break;
        }
    }
    if (!hostOk)
        return false;
    // Must be a GIF, not an mp4/webp/jpg still renamed into place.
    return u.path().endsWith(QLatin1String(".gif"), Qt::CaseInsensitive);
}

bool isSendableGifUrl(const QString &url)
{
    return isSendableGifUrlForHosts(url, { QStringLiteral(".giphy.com") });
}

GifByteValidation validateGifBytes(const QByteArray &bytes)
{
    GifByteValidation out;
    if (bytes.size() < 10) {
        out.category = QStringLiteral("invalid_media");
        return out;
    }
    // GIF magic: "GIF87a" or "GIF89a" — nothing else may be treated as a
    // real GIF (never an HTML page, JSON error, mp4, or webp renamed into
    // place). Matches rust/src/gifs.rs::validate_gif_bytes exactly.
    if (!bytes.startsWith("GIF87a") && !bytes.startsWith("GIF89a")) {
        out.category = QStringLiteral("not_a_gif");
        return out;
    }
    // Logical-screen descriptor: width/height are little-endian u16 at [6..10).
    const auto *d = reinterpret_cast<const unsigned char *>(bytes.constData());
    const int width = d[6] | (d[7] << 8);
    const int height = d[8] | (d[9] << 8);
    if (width <= 0 || height <= 0) {
        out.category = QStringLiteral("invalid_media");
        return out;
    }
    if (width > kMaxGifDimension || height > kMaxGifDimension
        || bytes.size() > kMaxGifBytes) {
        out.category = QStringLiteral("too_large");
        return out;
    }
    out.ok = true;
    out.width = width;
    out.height = height;
    return out;
}

namespace {

// PNG: 8-byte signature, then the mandatory first chunk (length[4] + "IHDR"
// + width[4] + height[4], all big-endian) per the PNG spec — width/height
// are always the IHDR chunk's first two fields.
bool pngDims(const QByteArray &bytes, int &width, int &height)
{
    static const char kSig[8] = {
        char(0x89), 'P', 'N', 'G', '\r', '\n', char(0x1a), '\n' };
    if (bytes.size() < 24 || bytes.left(8) != QByteArray(kSig, 8))
        return false;
    if (bytes.mid(12, 4) != QByteArrayLiteral("IHDR"))
        return false;
    const auto *d = reinterpret_cast<const unsigned char *>(bytes.constData());
    width = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
    height = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
    return width > 0 && height > 0;
}

// JPEG: walk the marker segments after SOI (FFD8) looking for a Start-Of-
// Frame marker (C0-CF, excluding DHT/JPG/DAC at C4/C8/CC — those are not
// SOF markers despite being in the same byte range). A SOF segment's
// payload is precision[1] + height[2] + width[2], big-endian. Bounded
// iteration count guards against a crafted file with a marker loop.
bool jpegDims(const QByteArray &bytes, int &width, int &height)
{
    const auto *d = reinterpret_cast<const unsigned char *>(bytes.constData());
    const int n = bytes.size();
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return false;
    int pos = 2;
    for (int guard = 0; guard < 2000 && pos + 2 <= n; ++guard) {
        if (d[pos] != 0xFF) { ++pos; continue; } // resync past fill bytes
        const unsigned char marker = d[pos + 1];
        pos += 2;
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01
            || (marker >= 0xD0 && marker <= 0xD7))
            continue; // no length field on these markers
        if (pos + 2 > n)
            break;
        const int segLen = (d[pos] << 8) | d[pos + 1];
        if (segLen < 2)
            break;
        const bool isSof = marker >= 0xC0 && marker <= 0xCF
            && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (isSof) {
            if (pos + 7 > n)
                break;
            height = (d[pos + 3] << 8) | d[pos + 4];
            width = (d[pos + 5] << 8) | d[pos + 6];
            return width > 0 && height > 0;
        }
        if (marker == 0xDA) // start of scan — entropy data, no more markers
            break;
        pos += segLen;
    }
    return false;
}

// WebP: 12-byte "RIFF"+size+"WEBP" container, then the first chunk's FourCC
// decides the dimension encoding: VP8X (extended header, 24-bit LE
// width-1/height-1), VP8L (lossless, packed 14-bit fields), or "VP8 "
// (lossy, 14-bit fields after the 3-byte start code 9D 01 2A).
bool webpDims(const QByteArray &bytes, int &width, int &height)
{
    if (bytes.size() < 20 || bytes.left(4) != QByteArrayLiteral("RIFF")
        || bytes.mid(8, 4) != QByteArrayLiteral("WEBP"))
        return false;
    const QByteArray fourCc = bytes.mid(12, 4);
    const auto *d = reinterpret_cast<const unsigned char *>(bytes.constData());
    const int c = 20; // first chunk's data starts after FourCC[4]+size[4]
    if (fourCc == QByteArrayLiteral("VP8X")) {
        if (bytes.size() < c + 10)
            return false;
        width = (d[c + 4] | (d[c + 5] << 8) | (d[c + 6] << 16)) + 1;
        height = (d[c + 7] | (d[c + 8] << 8) | (d[c + 9] << 16)) + 1;
        return width > 0 && height > 0;
    }
    if (fourCc == QByteArrayLiteral("VP8L")) {
        if (bytes.size() < c + 5 || d[c] != 0x2F)
            return false;
        const quint32 v = static_cast<quint32>(d[c + 1])
            | (static_cast<quint32>(d[c + 2]) << 8)
            | (static_cast<quint32>(d[c + 3]) << 16)
            | (static_cast<quint32>(d[c + 4]) << 24);
        width = static_cast<int>(v & 0x3FFF) + 1;
        height = static_cast<int>((v >> 14) & 0x3FFF) + 1;
        return true;
    }
    if (fourCc == QByteArrayLiteral("VP8 ")) {
        if (bytes.size() < c + 10 || d[c + 3] != 0x9D || d[c + 4] != 0x01
            || d[c + 5] != 0x2A)
            return false;
        width = (d[c + 6] | (d[c + 7] << 8)) & 0x3FFF;
        height = (d[c + 8] | (d[c + 9] << 8)) & 0x3FFF;
        return width > 0 && height > 0;
    }
    return false; // unrecognized first chunk — not a decodable still image
}

} // namespace

RasterByteValidation validateRasterBytes(const QByteArray &bytes)
{
    RasterByteValidation out;

    if (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a")) {
        const GifByteValidation v = validateGifBytes(bytes);
        out.ok = v.ok;
        out.category = v.category;
        out.width = v.width;
        out.height = v.height;
        if (v.ok) {
            out.ext = QStringLiteral("gif");
            out.mime = QStringLiteral("image/gif");
        }
        return out;
    }

    int w = 0, h = 0;
    QString ext, mime;
    if (bytes.startsWith(QByteArrayLiteral("\x89PNG\r\n\x1a\n"))) {
        if (!pngDims(bytes, w, h)) {
            out.category = QStringLiteral("invalid_media");
            return out;
        }
        ext = QStringLiteral("png");
        mime = QStringLiteral("image/png");
    } else if (bytes.size() >= 3
               && static_cast<unsigned char>(bytes[0]) == 0xFF
               && static_cast<unsigned char>(bytes[1]) == 0xD8
               && static_cast<unsigned char>(bytes[2]) == 0xFF) {
        if (!jpegDims(bytes, w, h)) {
            out.category = QStringLiteral("invalid_media");
            return out;
        }
        ext = QStringLiteral("jpg");
        mime = QStringLiteral("image/jpeg");
    } else if (bytes.startsWith(QByteArrayLiteral("RIFF"))
               && bytes.mid(8, 4) == QByteArrayLiteral("WEBP")) {
        if (!webpDims(bytes, w, h)) {
            out.category = QStringLiteral("invalid_media");
            return out;
        }
        ext = QStringLiteral("webp");
        mime = QStringLiteral("image/webp");
    } else {
        // Not one of the four supported magics — explicitly including SVG
        // (text, no binary magic), HTML error pages, BMP, TIFF, AVIF, and
        // everything else. Rejected, never guessed at.
        out.category = QStringLiteral("unsupported_format");
        return out;
    }

    if (w > kMaxGifDimension || h > kMaxGifDimension || bytes.size() > kMaxGifBytes) {
        out.category = QStringLiteral("too_large");
        return out;
    }
    out.ok = true;
    out.ext = ext;
    out.mime = mime;
    out.width = w;
    out.height = h;
    return out;
}

ParseOutcome parseGiphy(const QByteArray &json, Rating maxRating,
                        int requestOffset)
{
    ParseOutcome out;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        out.errorCategory = QStringLiteral("malformed");
        return out;
    }
    const QJsonObject root = doc.object();
    const QJsonValue dataVal = root.value(QStringLiteral("data"));
    if (!dataVal.isArray()) {
        out.errorCategory = QStringLiteral("malformed");
        return out;
    }

    const QJsonObject pagination =
        root.value(QStringLiteral("pagination")).toObject();
    out.totalCount = pagination.contains(QStringLiteral("total_count"))
        ? asInt(pagination.value(QStringLiteral("total_count")))
        : -1;

    const QStringList giphyHosts = { QStringLiteral(".giphy.com") };
    QSet<QString> seen;
    const QJsonArray data = dataVal.toArray();
    for (const QJsonValue &entry : data) {
        const QJsonObject obj = entry.toObject();
        const QString id = obj.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || seen.contains(id))
            continue; // drop malformed / duplicate results

        const QString rating = obj.value(QStringLiteral("rating")).toString();
        if (!ratingWithin(rating, maxRating))
            continue; // safe-search: excludes unknown ratings too

        const QJsonObject images =
            obj.value(QStringLiteral("images")).toObject();

        GifResult r;
        r.gifUrl = firstGifUrl(images,
            { QStringLiteral("original"), QStringLiteral("downsized_medium"),
              QStringLiteral("downsized"), QStringLiteral("fixed_height") },
            giphyHosts, r.gifWidth, r.gifHeight, r.gifBytes);
        if (r.gifUrl.isEmpty())
            continue; // nothing sendable — skip entirely
        // Enforce the sendable caps when the provider reports dimensions/size.
        if (r.gifWidth > kMaxGifDimension || r.gifHeight > kMaxGifDimension
            || (r.gifBytes > 0 && r.gifBytes > kMaxGifBytes))
            continue;

        // Small animated preview for the grid. Its own size must not clobber
        // the sendable variant's dimensions.
        {
            qint64 ignoreBytes = 0;
            int pw = 0, ph = 0;
            const QString preview = firstGifUrl(images,
                { QStringLiteral("fixed_width"), QStringLiteral("preview_gif"),
                  QStringLiteral("fixed_width_small") }, giphyHosts, pw, ph,
                ignoreBytes);
            if (!preview.isEmpty()) {
                r.previewUrl = preview;
                r.previewWidth = pw;
                r.previewHeight = ph;
            } else {
                r.previewUrl = r.gifUrl; // fall back to the sendable gif
                r.previewWidth = r.gifWidth;
                r.previewHeight = r.gifHeight;
            }
        }

        int sw = 0, sh = 0;
        r.stillUrl = firstHttpsUrl(images,
            { QStringLiteral("fixed_width_still"),
              QStringLiteral("480w_still"),
              QStringLiteral("original_still") },
            QStringLiteral("url"), sw, sh);

        // Optional preview video (mp4) — never sent, picker preview only.
        for (const QString &key : { QStringLiteral("original"),
                                    QStringLiteral("fixed_width") }) {
            const QJsonObject rendition = images.value(key).toObject();
            const QString mp4 =
                stripTracking(rendition.value(QStringLiteral("mp4")).toString());
            if (mp4.startsWith(QLatin1String("https://"))) {
                r.mp4Url = mp4;
                break;
            }
        }

        r.provider = QStringLiteral("giphy");
        r.id = id;
        r.title = obj.value(QStringLiteral("title")).toString();
        r.rating = ratingToString(ratingFromString(rating));
        seen.insert(id);
        out.results.append(r);
    }

    out.ok = true;
    out.nextOffset = requestOffset + static_cast<int>(data.size());
    // Another page exists when the server reports more than we have seen.
    if (out.totalCount >= 0)
        out.hasMore = out.nextOffset < out.totalCount;
    else
        out.hasMore = !data.isEmpty();
    return out;
}

// ── KLIPY ───────────────────────────────────────────────────────────────
// Response: { result, data: { data:[items], current_page, per_page, has_next }}
// Item: { id, slug, title, type, file: { hd|md|sm|xs: { gif|webp|jpg|mp4: {
//         url, width, height, size } } } }. No per-item rating — safe-search is
// applied at request time, so `requestRating` is stamped onto each result.
ParseOutcome parseKlipy(const QByteArray &json, Rating requestRating,
                        int requestPage)
{
    ParseOutcome out;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        out.errorCategory = QStringLiteral("malformed");
        return out;
    }
    const QJsonObject root = doc.object();
    // KLIPY reports failures as {"result":false,...}. Treat as malformed so the
    // controller shows a bounded error rather than an empty success.
    if (root.contains(QStringLiteral("result"))
        && !root.value(QStringLiteral("result")).toBool(true)) {
        out.errorCategory = QStringLiteral("malformed");
        return out;
    }
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const QJsonValue itemsVal = data.value(QStringLiteral("data"));
    if (!itemsVal.isArray()) {
        out.errorCategory = QStringLiteral("malformed");
        return out;
    }

    const QStringList klipyHosts = { QStringLiteral(".klipy.com") };
    const QString ratingStr = ratingToString(requestRating);
    QSet<QString> seen;
    const QJsonArray items = itemsVal.toArray();
    for (const QJsonValue &entry : items) {
        const QJsonObject obj = entry.toObject();
        // id may be a number or a string.
        const QJsonValue idVal = obj.value(QStringLiteral("id"));
        const QString id = idVal.isDouble()
            ? QString::number(static_cast<qint64>(idVal.toDouble()))
            : idVal.toString();
        if (id.isEmpty() || seen.contains(id))
            continue;
        if (obj.value(QStringLiteral("type")).toString(QStringLiteral("gif"))
                != QLatin1String("gif"))
            continue; // gifs only from the gifs endpoint

        const QJsonObject file = obj.value(QStringLiteral("file")).toObject();

        // Build a {size: {url,width,height,size}} view of the .gif renditions
        // so the shared firstGifUrl helper can pick one.
        QJsonObject gifBySize;
        for (const QString &size : { QStringLiteral("md"), QStringLiteral("hd"),
                                     QStringLiteral("sm"), QStringLiteral("xs") }) {
            const QJsonObject gif =
                file.value(size).toObject().value(QStringLiteral("gif")).toObject();
            if (!gif.isEmpty())
                gifBySize.insert(size, gif);
        }

        GifResult r;
        r.gifUrl = firstGifUrl(gifBySize,
            { QStringLiteral("md"), QStringLiteral("hd"), QStringLiteral("sm") },
            klipyHosts, r.gifWidth, r.gifHeight, r.gifBytes);
        if (r.gifUrl.isEmpty())
            continue;
        if (r.gifWidth > kMaxGifDimension || r.gifHeight > kMaxGifDimension
            || (r.gifBytes > 0 && r.gifBytes > kMaxGifBytes))
            continue;

        {
            qint64 ignoreBytes = 0;
            int pw = 0, ph = 0;
            const QString preview = firstGifUrl(gifBySize,
                { QStringLiteral("sm"), QStringLiteral("xs"),
                  QStringLiteral("md") }, klipyHosts, pw, ph, ignoreBytes);
            if (!preview.isEmpty()) {
                r.previewUrl = preview;
                r.previewWidth = pw;
                r.previewHeight = ph;
            } else {
                r.previewUrl = r.gifUrl;
                r.previewWidth = r.gifWidth;
                r.previewHeight = r.gifHeight;
            }
        }

        // Static still (jpg) for fast scrolling.
        QJsonObject jpgBySize;
        for (const QString &size : { QStringLiteral("sm"), QStringLiteral("md"),
                                     QStringLiteral("xs") }) {
            const QJsonObject jpg =
                file.value(size).toObject().value(QStringLiteral("jpg")).toObject();
            if (!jpg.isEmpty())
                jpgBySize.insert(size, jpg);
        }
        int sw = 0, sh = 0;
        r.stillUrl = firstHttpsUrl(jpgBySize,
            { QStringLiteral("sm"), QStringLiteral("md"), QStringLiteral("xs") },
            QStringLiteral("url"), sw, sh);

        // Optional preview mp4 — never sent as a gif.
        for (const QString &size : { QStringLiteral("sm"), QStringLiteral("md") }) {
            const QString mp4 = file.value(size).toObject()
                .value(QStringLiteral("mp4")).toObject()
                .value(QStringLiteral("url")).toString();
            if (mp4.startsWith(QLatin1String("https://"))) {
                r.mp4Url = mp4;
                break;
            }
        }

        r.provider = QStringLiteral("klipy");
        r.id = id;
        r.title = obj.value(QStringLiteral("title")).toString();
        r.rating = ratingStr;
        seen.insert(id);
        out.results.append(r);
    }

    out.ok = true;
    out.nextPage = requestPage + 1;
    out.hasMore = data.value(QStringLiteral("has_next")).toBool(false);
    return out;
}

} // namespace gif
