#pragma once

#include "gif/GifResponseParser.h"

#include <QString>
#include <QStringList>

#include <memory>

// Shared GIF-provider abstraction. A provider owns endpoint construction, key
// injection, pagination, safe-search mapping, parsing, attribution and media
// host policy. The API key is passed per call and never held in provider
// state.
namespace gif {

enum class QueryKind { Trending, Search, Categories };

enum class RequestState {
    Idle,
    Loading,
    LoadingMore,
    Ready,
    NoResults,
    Offline,
    RateLimited,
    ProviderError,
    MissingKey,
    Cancelled,
};

class GifProvider {
public:
    virtual ~GifProvider() = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual QString attribution() const = 0;
    virtual bool supportsServerCategories() const = 0;

    virtual QString trendingUrl(Rating rating, int page, int perPage,
                                const QString &apiKey) const = 0;
    virtual QString searchUrl(const QString &query, Rating rating, int page,
                              int perPage, const QString &apiKey) const = 0;
    virtual QString categoriesUrl(const QString &apiKey) const;

    virtual ParseOutcome parse(const QByteArray &json, Rating rating,
                               int requestPage, int perPage) const = 0;

    virtual QStringList categoryShortcuts() const;
};

std::unique_ptr<GifProvider> makeGifProvider(const QString &id);

QStringList knownGifProviderIds();

}
