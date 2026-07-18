#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// v0.6.1: client-side GIF provider foundation. Pure translation from a GIPHY
// v1 JSON response (trending / search) into safe, presentation-ready result
// structs. No Qt Network, no FFI, no I/O — fully unit-testable without an API
// key or a homeserver. The actual bounded HTTPS request belongs Rust-side
// (mirroring rooms.rs safe_get); this layer only interprets the bytes it is
// handed and enforces the client-side safety rules.
//
// Safety contract:
//  - result URLs are provider CDN https URLs used ONLY to (a) render a bounded
//    preview through the existing safe media path and (b) download the real GIF
//    bytes before sending as m.image — never rendered as a raw Image source,
//    never sent as a bare URL message;
//  - only https provider-CDN hosts (*.giphy.com) are accepted;
//  - provider tracking query params (cid/rid/ct/…) are stripped;
//  - no API key, response headers, or raw provider JSON are ever surfaced;
//  - a safe-search rating cap drops results above the configured rating,
//    including results whose rating is missing/unknown.
namespace gif {

// One safe GIF ready for the picker / send pipeline.
struct GifResult {
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
};

// Safe-search ceiling, most-permissive last.
enum class Rating { G, PG, PG13, R };

struct ParseOutcome {
    QList<GifResult> results;
    int totalCount = -1;   // pagination.total_count, -1 when unknown
    int nextOffset = 0;    // request offset + returned count (for pagination)
    bool ok = false;       // false only on a malformed/unusable response
    QString errorCategory; // "malformed" when !ok; empty otherwise
};

// Client-side hard caps for the sendable GIF variant. The picker preview uses
// smaller renditions; these bound what may be downloaded and sent.
inline constexpr int kMaxGifDimension = 4096;
inline constexpr qint64 kMaxGifBytes = 25LL * 1024 * 1024; // 25 MiB

// Parse a GIPHY v1 trending/search response. `requestOffset` is the offset the
// request used, so nextOffset can be derived without trusting echoed input.
ParseOutcome parseGiphy(const QByteArray &json, Rating maxRating,
                        int requestOffset);

Rating ratingFromString(const QString &value);

// True when `itemRating` is at or below `maxRating`. Missing/unknown ratings
// are treated as the most permissive (R), so they are excluded unless the user
// explicitly allows R.
bool ratingWithin(const QString &itemRating, Rating maxRating);

// True when `url` is an https provider-CDN (*.giphy.com) URL for a .gif — the
// only thing safe to download and send as m.image.
bool isSendableGifUrl(const QString &url);

// Remove provider tracking query params, keeping the bare CDN path.
QString stripTracking(const QString &url);

} // namespace gif
