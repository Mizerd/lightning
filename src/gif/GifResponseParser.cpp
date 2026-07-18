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

// Pick the first rendition in `keys` that has a usable https .gif url.
QString firstGifUrl(const QJsonObject &images, const QStringList &keys,
                    int &width, int &height, qint64 &bytes)
{
    for (const QString &key : keys) {
        const QJsonObject r = images.value(key).toObject();
        const QString url = stripTracking(r.value(QStringLiteral("url")).toString());
        if (isSendableGifUrl(url)) {
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

bool isSendableGifUrl(const QString &url)
{
    if (!url.startsWith(QLatin1String("https://")))
        return false;
    const QUrl u(url);
    if (!u.isValid())
        return false;
    const QString host = u.host().toLower();
    // Provider-CDN host policy: only GIPHY media hosts.
    const bool giphyHost = host == QLatin1String("giphy.com")
        || host.endsWith(QLatin1String(".giphy.com"));
    if (!giphyHost)
        return false;
    // Must be a GIF, not an mp4/webp/jpg still renamed into place.
    return u.path().endsWith(QLatin1String(".gif"), Qt::CaseInsensitive);
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
            r.gifWidth, r.gifHeight, r.gifBytes);
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
                  QStringLiteral("fixed_width_small") }, pw, ph, ignoreBytes);
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

        r.id = id;
        r.title = obj.value(QStringLiteral("title")).toString();
        r.rating = ratingToString(ratingFromString(rating));
        seen.insert(id);
        out.results.append(r);
    }

    out.ok = true;
    out.nextOffset = requestOffset + static_cast<int>(data.size());
    return out;
}

} // namespace gif
