#include "gif/GifProvider.h"

#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace gif {
namespace {

QString ratingParam(Rating r) { return ratingToString(r); }

// Shared client-side category shortcuts — plain search terms, presented as
// tiles. Not claimed to be server-authoritative categories.
const QStringList kCategoryShortcuts = {
    QStringLiteral("reactions"), QStringLiteral("celebrate"),
    QStringLiteral("laugh"),     QStringLiteral("love"),
    QStringLiteral("animals"),   QStringLiteral("gaming"),
    QStringLiteral("sports"),    QStringLiteral("memes"),
    QStringLiteral("agree"),     QStringLiteral("no"),
    QStringLiteral("thanks"),    QStringLiteral("wow"),
};

// ── GIPHY ────────────────────────────────────────────────────────────────
// Offset-based pagination; key is the `api_key` query parameter.
class GiphyGifProvider final : public GifProvider {
public:
    QString id() const override { return QStringLiteral("giphy"); }
    QString displayName() const override { return QStringLiteral("GIPHY"); }
    QString attribution() const override { return QStringLiteral("Powered by GIPHY"); }
    bool supportsServerCategories() const override { return true; }

    QString trendingUrl(Rating rating, int page, int perPage,
                        const QString &apiKey) const override
    {
        if (apiKey.isEmpty())
            return {};
        QUrl url(QStringLiteral("https://api.giphy.com/v1/gifs/trending"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("api_key"), apiKey);
        q.addQueryItem(QStringLiteral("limit"), QString::number(perPage));
        q.addQueryItem(QStringLiteral("offset"),
                       QString::number(qMax(0, page) * perPage));
        q.addQueryItem(QStringLiteral("rating"), ratingParam(rating));
        q.addQueryItem(QStringLiteral("bundle"),
                       QStringLiteral("messaging_non_clips"));
        url.setQuery(q);
        return url.toString();
    }

    QString searchUrl(const QString &query, Rating rating, int page, int perPage,
                      const QString &apiKey) const override
    {
        if (apiKey.isEmpty() || query.trimmed().isEmpty())
            return {};
        QUrl url(QStringLiteral("https://api.giphy.com/v1/gifs/search"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("api_key"), apiKey);
        q.addQueryItem(QStringLiteral("q"), query.trimmed().left(50));
        q.addQueryItem(QStringLiteral("limit"), QString::number(perPage));
        q.addQueryItem(QStringLiteral("offset"),
                       QString::number(qMax(0, page) * perPage));
        q.addQueryItem(QStringLiteral("rating"), ratingParam(rating));
        q.addQueryItem(QStringLiteral("bundle"),
                       QStringLiteral("messaging_non_clips"));
        url.setQuery(q);
        return url.toString();
    }

    QString categoriesUrl(const QString &apiKey) const override
    {
        if (apiKey.isEmpty())
            return {};
        QUrl url(QStringLiteral("https://api.giphy.com/v1/gifs/categories"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("api_key"), apiKey);
        url.setQuery(q);
        return url.toString();
    }

    ParseOutcome parse(const QByteArray &json, Rating rating, int requestPage,
                       int perPage) const override
    {
        return parseGiphy(json, rating, qMax(0, requestPage) * perPage);
    }
};

// ── KLIPY ────────────────────────────────────────────────────────────────
// Page-based pagination; key is a path segment. Percent-encode the key so it
// can never break the path structure.
class KlipyGifProvider final : public GifProvider {
public:
    QString id() const override { return QStringLiteral("klipy"); }
    QString displayName() const override { return QStringLiteral("KLIPY"); }
    QString attribution() const override { return QStringLiteral("Powered by KLIPY"); }
    bool supportsServerCategories() const override { return true; }

    QString trendingUrl(Rating rating, int page, int perPage,
                        const QString &apiKey) const override
    {
        if (apiKey.isEmpty())
            return {};
        QUrl url(base(apiKey) + QStringLiteral("/gifs/trending"));
        url.setQuery(pageQuery(rating, page, perPage));
        return url.toString();
    }

    QString searchUrl(const QString &query, Rating rating, int page, int perPage,
                      const QString &apiKey) const override
    {
        if (apiKey.isEmpty() || query.trimmed().isEmpty())
            return {};
        QUrl url(base(apiKey) + QStringLiteral("/gifs/search"));
        QUrlQuery q = pageQuery(rating, page, perPage);
        q.addQueryItem(QStringLiteral("q"), query.trimmed());
        url.setQuery(q);
        return url.toString();
    }

    QString categoriesUrl(const QString &apiKey) const override
    {
        if (apiKey.isEmpty())
            return {};
        return QUrl(base(apiKey) + QStringLiteral("/gifs/categories")).toString();
    }

    ParseOutcome parse(const QByteArray &json, Rating rating, int requestPage,
                       int /*perPage*/) const override
    {
        return parseKlipy(json, rating, qMax(0, requestPage));
    }

private:
    static QString base(const QString &apiKey)
    {
        return QStringLiteral("https://api.klipy.com/api/v1/")
            + QString::fromUtf8(
                QUrl::toPercentEncoding(apiKey)); // key is a path segment
    }
    static QUrlQuery pageQuery(Rating rating, int page, int perPage)
    {
        QUrlQuery q;
        // KLIPY per_page is clamped to [8, 50]; pages are 1-based.
        q.addQueryItem(QStringLiteral("per_page"),
                       QString::number(std::clamp(perPage, 8, 50)));
        q.addQueryItem(QStringLiteral("page"), QString::number(qMax(0, page) + 1));
        q.addQueryItem(QStringLiteral("rating"), ratingParam(rating));
        return q;
    }
};

} // namespace

QString GifProvider::categoriesUrl(const QString & /*apiKey*/) const
{
    return {};
}

QStringList GifProvider::categoryShortcuts() const
{
    return kCategoryShortcuts;
}

std::unique_ptr<GifProvider> makeGifProvider(const QString &id)
{
    if (id == QLatin1String("giphy"))
        return std::make_unique<GiphyGifProvider>();
    if (id == QLatin1String("klipy"))
        return std::make_unique<KlipyGifProvider>();
    return nullptr;
}

QStringList knownGifProviderIds()
{
    return { QStringLiteral("giphy"), QStringLiteral("klipy") };
}

} // namespace gif
