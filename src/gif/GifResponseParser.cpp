#include "gif/GifResponseParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

namespace gif {
namespace {

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

// Whether `url` is https on one of the provider's own hosts. The one policy
// both the sendable rendition and the DISPLAYED renditions go through.
bool isProviderHttpsUrl(const QString &url, const QStringList &allowedHostSuffixes)
{
    if (!url.startsWith(QLatin1String("https://")))
        return false;
    const QUrl u(url);
    if (!u.isValid() || !u.userInfo().isEmpty())
        return false;
    const QString host = u.host().toLower();
    for (const QString &suffix : allowedHostSuffixes) {
        const QString bare = suffix.startsWith(QLatin1Char('.'))
            ? suffix.mid(1) : suffix;
        if (host == bare || host.endsWith(suffix))
            return true;
    }
    return false;
}

QString firstHttpsUrl(const QJsonObject &images, const QStringList &keys,
                      const QString &field, const QStringList &hosts,
                      int &width, int &height)
{
    for (const QString &key : keys) {
        const QJsonObject r = images.value(key).toObject();
        const QString url = stripTracking(r.value(field).toString());
        if (isProviderHttpsUrl(url, hosts)) {
            width = asInt(r.value(QStringLiteral("width")));
            height = asInt(r.value(QStringLiteral("height")));
            return url;
        }
    }
    return QString();
}

}

Rating ratingFromString(const QString &value)
{
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("g"))
        return Rating::G;
    if (v == QLatin1String("pg"))
        return Rating::PG;
    if (v == QLatin1String("pg-13") || v == QLatin1String("pg13"))
        return Rating::PG13;
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
    if (!bytes.startsWith("GIF87a") && !bytes.startsWith("GIF89a")) {
        out.category = QStringLiteral("not_a_gif");
        return out;
    }
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

bool jpegDims(const QByteArray &bytes, int &width, int &height)
{
    const auto *d = reinterpret_cast<const unsigned char *>(bytes.constData());
    const int n = bytes.size();
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return false;
    int pos = 2;
    for (int guard = 0; guard < 2000 && pos + 2 <= n; ++guard) {
        if (d[pos] != 0xFF) { ++pos; continue; }
        const unsigned char marker = d[pos + 1];
        pos += 2;
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01
            || (marker >= 0xD0 && marker <= 0xD7))
            continue;
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
        if (marker == 0xDA)
            break;
        pos += segLen;
    }
    return false;
}

bool webpDims(const QByteArray &bytes, int &width, int &height)
{
    if (bytes.size() < 20 || bytes.left(4) != QByteArrayLiteral("RIFF")
        || bytes.mid(8, 4) != QByteArrayLiteral("WEBP"))
        return false;
    const QByteArray fourCc = bytes.mid(12, 4);
    const auto *d = reinterpret_cast<const unsigned char *>(bytes.constData());
    const int c = 20;
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
    return false;
}

}

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
            continue;

        const QString rating = obj.value(QStringLiteral("rating")).toString();
        if (!ratingWithin(rating, maxRating))
            continue;

        const QJsonObject images =
            obj.value(QStringLiteral("images")).toObject();

        GifResult r;
        r.gifUrl = firstGifUrl(images,
            { QStringLiteral("original"), QStringLiteral("downsized_medium"),
              QStringLiteral("downsized"), QStringLiteral("fixed_height") },
            giphyHosts, r.gifWidth, r.gifHeight, r.gifBytes);
        if (r.gifUrl.isEmpty())
            continue;
        if (r.gifWidth > kMaxGifDimension || r.gifHeight > kMaxGifDimension
            || (r.gifBytes > 0 && r.gifBytes > kMaxGifBytes))
            continue;

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
                r.previewUrl = r.gifUrl;
                r.previewWidth = r.gifWidth;
                r.previewHeight = r.gifHeight;
            }
        }

        int sw = 0, sh = 0;
        r.stillUrl = firstHttpsUrl(images,
            { QStringLiteral("fixed_width_still"),
              QStringLiteral("480w_still"),
              QStringLiteral("original_still") },
            QStringLiteral("url"), giphyHosts, sw, sh);

        // Optional preview video (mp4) — never sent, picker preview only.
        for (const QString &key : { QStringLiteral("original"),
                                    QStringLiteral("fixed_width") }) {
            const QJsonObject rendition = images.value(key).toObject();
            const QString mp4 =
                stripTracking(rendition.value(QStringLiteral("mp4")).toString());
            if (isProviderHttpsUrl(mp4, giphyHosts)) {
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
    if (out.totalCount >= 0)
        out.hasMore = out.nextOffset < out.totalCount;
    else
        out.hasMore = !data.isEmpty();
    return out;
}

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
        const QJsonValue idVal = obj.value(QStringLiteral("id"));
        const QString id = idVal.isDouble()
            ? QString::number(static_cast<qint64>(idVal.toDouble()))
            : idVal.toString();
        if (id.isEmpty() || seen.contains(id))
            continue;
        if (obj.value(QStringLiteral("type")).toString(QStringLiteral("gif"))
                != QLatin1String("gif"))
            continue;

        const QJsonObject file = obj.value(QStringLiteral("file")).toObject();

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
            QStringLiteral("url"), klipyHosts, sw, sh);

        for (const QString &size : { QStringLiteral("sm"), QStringLiteral("md") }) {
            const QString mp4 = file.value(size).toObject()
                .value(QStringLiteral("mp4")).toObject()
                .value(QStringLiteral("url")).toString();
            if (isProviderHttpsUrl(mp4, klipyHosts)) {
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

}
