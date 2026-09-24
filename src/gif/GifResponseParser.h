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

// One safe GIF ready for the picker / send pipeline.
struct GifResult {
    QString provider;      // "giphy" / "klipy" — favorites/recents identity
    QString id;            // provider id — dedup, favorites, recents key
    QString title;         // accessible description / body fallback
    QString rating;        // normalized g / pg / pg-13 / r
    QString previewUrl;    // small ANIMATED preview for the grid
    int previewWidth = 0;
    int previewHeight = 0;
    QString stillUrl;      // static thumbnail for fast scrolling (may be empty)
    QString gifUrl;        // the actual image/gif to download + send
    int gifWidth = 0;
    int gifHeight = 0;
    qint64 gifBytes = 0;   // 0 = unknown
    QString mp4Url;        // optional preview video — NEVER sent as a gif
    // Canonical suffix ("gif"/"png"/"jpg"/"webp") for a locally saved row
    // (provider "local"); always empty for provider rows. Empty on a local row
    // means a legacy entry ("gif"). Callers needing the real format ask the
    // store, which re-validates against disk.
    QString localExt;
};

// Safe-search ceiling, most-permissive last.
enum class Rating { G, PG, PG13, R };

struct ParseOutcome {
    QList<GifResult> results;
    int totalCount = -1;   // total when the provider reports it, else -1
    int nextOffset = 0;    // GIPHY: request offset + returned count
    int nextPage = 0;      // KLIPY: request page + 1 (page-based providers)
    bool hasMore = false;  // another page exists (from total_count / has_next)
    bool ok = false;       // false only on a malformed/unusable response
    QString errorCategory; // "malformed" when !ok; empty otherwise
};

// Client-side hard caps for the sendable GIF variant. The picker preview uses
// smaller renditions; these bound what may be downloaded and sent.
inline constexpr int kMaxGifDimension = 4096;
inline constexpr qint64 kMaxGifBytes = 25LL * 1024 * 1024; // 25 MiB

// `requestOffset` is the request's own offset, so nextOffset does not trust
// echoed input.
ParseOutcome parseGiphy(const QByteArray &json, Rating maxRating,
                        int requestOffset);

// `requestPage` is the 0-based page requested. KLIPY returns no per-item
// rating, so the request's safe-search level is recorded on each result.
ParseOutcome parseKlipy(const QByteArray &json, Rating requestRating,
                        int requestPage);

Rating ratingFromString(const QString &value);
QString ratingToString(Rating r);

// True when `itemRating` is at or below `maxRating`. Missing or unknown
// ratings count as R, so they are excluded unless R is allowed.
bool ratingWithin(const QString &itemRating, Rating maxRating);

// True when `url` is an https URL on one of `allowedHostSuffixes` (e.g.
// {".giphy.com"}) for a .gif — the only thing safe to download and send as
// m.image.
bool isSendableGifUrlForHosts(const QString &url,
                              const QStringList &allowedHostSuffixes);

// GIPHY overload (*.giphy.com).
bool isSendableGifUrl(const QString &url);

// Remove provider tracking query params, keeping the bare CDN path.
QString stripTracking(const QString &url);

// Byte-level GIF validation for bytes that do not come through the Rust
// provider download (locally saved chat GIFs). Matches
// rust/src/gifs.rs::validate_gif_bytes exactly: magic, logical screen size,
// kMaxGifDimension and kMaxGifBytes.
struct GifByteValidation {
    bool ok = false;
    // "not_a_gif" | "invalid_media" | "too_large" | "" when ok.
    QString category;
    int width = 0;
    int height = 0;
};
GifByteValidation validateGifBytes(const QByteArray &bytes);

// Byte-level validation for any raster format accepted for a locally saved
// image (GIF/PNG/JPEG/WebP), used by GifStarredStore and GifSendController.
// The bytes decide the format; a claimed extension or Content-Type is never
// trusted. Same caps as validateGifBytes for every format, and the stored
// bytes are exactly what was validated.
struct RasterByteValidation {
    bool ok = false;
    // "invalid_media" | "too_large" | "unsupported_format" | "" when ok. Never
    // "not_a_gif".
    QString category;
    // "gif" | "png" | "jpg" | "webp", from the magic bytes. Empty when !ok.
    QString ext;
    // What the bytes actually are: "image/gif" | "image/png" | "image/jpeg" |
    // "image/webp". Empty when !ok.
    QString mime;
    int width = 0;
    int height = 0;
};
RasterByteValidation validateRasterBytes(const QByteArray &bytes);

} // namespace gif
