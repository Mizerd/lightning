#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Pure translation of a GIPHY or KLIPY trending/search response into safe,
// presentation-ready results. No network, FFI or I/O; the bounded HTTPS
// request lives Rust-side.
//
// Safety contract:
//  - result URLs are provider CDN https URLs used only to render a bounded
//    preview through the safe media path and to download the real bytes
//    before sending as m.image; never a raw Image source or a bare URL
//    message;
//  - only https provider-CDN hosts (*.giphy.com / *.klipy.com) are accepted
//    for the sendable variant;
//  - provider tracking query parameters are stripped;
//  - no API key, response header or raw provider JSON is ever surfaced;
//  - the safe-search cap drops results above the configured rating, and
//    results with a missing or unknown rating.
namespace gif {

struct GifResult {
    QString provider;
    QString id;
    QString title;
    QString rating;
    QString previewUrl;
    int previewWidth = 0;
    int previewHeight = 0;
    QString stillUrl;
    QString gifUrl;
    int gifWidth = 0;
    int gifHeight = 0;
    qint64 gifBytes = 0;
    QString mp4Url;
    QString localExt;
};

enum class Rating { G, PG, PG13, R };

struct ParseOutcome {
    QList<GifResult> results;
    int totalCount = -1;
    int nextOffset = 0;
    int nextPage = 0;
    bool hasMore = false;
    bool ok = false;
    QString errorCategory;
};

inline constexpr int kMaxGifDimension = 4096;
inline constexpr qint64 kMaxGifBytes = 25LL * 1024 * 1024;

// `requestOffset` is the request's own offset, so nextOffset does not trust
// echoed input.
ParseOutcome parseGiphy(const QByteArray &json, Rating maxRating,
                        int requestOffset);

ParseOutcome parseKlipy(const QByteArray &json, Rating requestRating,
                        int requestPage);

Rating ratingFromString(const QString &value);
QString ratingToString(Rating r);

// True when `itemRating` is at or below `maxRating`. Missing or unknown
// ratings count as R, so they are excluded unless R is allowed.
bool ratingWithin(const QString &itemRating, Rating maxRating);

bool isSendableGifUrlForHosts(const QString &url,
                              const QStringList &allowedHostSuffixes);

bool isSendableGifUrl(const QString &url);

QString stripTracking(const QString &url);

struct GifByteValidation {
    bool ok = false;
    QString category;
    int width = 0;
    int height = 0;
};
GifByteValidation validateGifBytes(const QByteArray &bytes);

struct RasterByteValidation {
    bool ok = false;
    QString category;
    QString ext;
    QString mime;
    int width = 0;
    int height = 0;
};
RasterByteValidation validateRasterBytes(const QByteArray &bytes);

}
