#pragma once

#include "gif/GifResponseParser.h"

#include <QString>
#include <QStringList>

#include <memory>

// v0.6.1: shared GIF-provider abstraction. A GifProvider owns the
// provider-specific concerns — endpoint construction, API-key injection,
// pagination convention, safe-search mapping, response parsing, attribution and
// media-host policy — behind one interface the shared controller drives. The
// API key is passed per-call (from the environment / settings) and never held
// in provider state, so a provider object can never leak a key.
namespace gif {

// The request kind the controller is running (drives debounce + attribution).
enum class QueryKind { Trending, Search, Categories };

// Controller-facing request state (Phase 1). Presentation-safe; carries no key.
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

    virtual QString id() const = 0;          // "giphy" / "klipy"
    virtual QString displayName() const = 0; // "GIPHY" / "KLIPY"
    virtual QString attribution() const = 0; // e.g. "Powered by GIPHY"
    virtual bool supportsServerCategories() const = 0;

    // Build the request URL for a 0-based `page` of `perPage` results. Returns
    // an https URL with `apiKey` embedded, or an empty string when `apiKey` is
    // empty (unconfigured) or the query is unsupported. Callers must treat the
    // returned URL as secret (it contains the key) — never log it.
    virtual QString trendingUrl(Rating rating, int page, int perPage,
                                const QString &apiKey) const = 0;
    virtual QString searchUrl(const QString &query, Rating rating, int page,
                              int perPage, const QString &apiKey) const = 0;
    virtual QString categoriesUrl(const QString &apiKey) const;

    // Parse a trending/search response for the 0-based `requestPage`.
    virtual ParseOutcome parse(const QByteArray &json, Rating rating,
                               int requestPage, int perPage) const = 0;

    // Client-side category shortcuts (plain search terms) for providers without
    // server categories, or as a stable fallback set.
    virtual QStringList categoryShortcuts() const;
};

// Factory: "giphy" / "klipy". Returns nullptr for an unknown id.
std::unique_ptr<GifProvider> makeGifProvider(const QString &id);

// Every implemented provider id, in display order.
QStringList knownGifProviderIds();

} // namespace gif
