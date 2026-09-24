#pragma once

#include <QString>
#include <QUrl>

// Pure link-preview helpers: URL extraction from message text and
// MIME-validated GIF classification. No models, FFI or I/O.
//
// Exactly one URL per message is previewable (the first eligible one). Only
// https:// and http:// qualify via a positive allow-list, so javascript:,
// data:, file:, blob: and the like never match. URLs with userinfo are
// rejected outright so credentials never reach a request, log or the
// homeserver. URLs inside inline code or code blocks are excluded.
namespace matrix::link_preview {

// First previewable URL in `body`, cleaned of trailing punctuation and
// unbalanced closing parentheses/brackets. Empty when none qualifies.
QString firstPreviewableUrl(const QString &body);

// Hostname of `url` for safe diagnostics; never includes path, query,
// fragment, or userinfo. Empty for unparsable input.
QString sanitizedHost(const QString &url);
QString linkifiedMessageHtml(const QString &body);
bool isSafeExternalUrl(const QUrl &url);

// A preview is a GIF only when the validated MIME type (from og:image:type,
// never the URL suffix) is image/gif and the metadata is within limits.
// Oversized GIFs fall back to a static preview.
struct GifLimits {
    qint64 maxBytes = 10 * 1024 * 1024; // animated payloads beyond this stay static
    int maxWidth = 2048;
    int maxHeight = 2048;
};

enum class GifClass {
    NotGif,    // not a GIF (including deceptive .gif URLs with html MIME)
    Gif,       // validated image/gif inside the limits; animation expected
    Oversized, // validated image/gif but beyond the limits; use static preview
};

GifClass classifyGif(const QString &validatedMime, qint64 sizeBytes,
                     int width, int height, const GifLimits &limits = {});

} // namespace matrix::link_preview
