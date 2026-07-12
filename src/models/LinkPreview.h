#pragma once

#include <QString>
#include <QUrl>

// v0.5.11: pure link-preview helpers — URL extraction from message text and
// MIME-validated GIF classification. No Qt models, no FFI, no I/O; fully
// unit-testable.
//
// Policy: exactly ONE URL per message is previewable (the first eligible
// one). Only https:// and http:// qualify; unsafe schemes (javascript:,
// data:, file:, blob:, …) never match because the scheme allow-list is
// positive-only. URLs carrying userinfo ("https://user:pass@host/…") are
// rejected outright so embedded credentials can never reach a request,
// a log line, or the homeserver. Inline-code and code-block spans
// (`…` / ```…```) are excluded from extraction.
namespace matrix::link_preview {

// First previewable URL in `body`, cleaned of trailing punctuation and
// unbalanced closing parentheses/brackets. Empty when none qualifies.
QString firstPreviewableUrl(const QString &body);

// Hostname of `url` for safe diagnostics; never includes path, query,
// fragment, or userinfo. Empty for unparsable input.
QString sanitizedHost(const QString &url);
QString linkifiedMessageHtml(const QString &body);
bool isSafeExternalUrl(const QUrl &url);

// v0.5.11 (Phase 7): direct-GIF classification. A preview is a GIF only
// when the VALIDATED MIME type (from the homeserver's og:image:type —
// never the URL suffix) says image/gif AND the metadata stays inside the
// safety limits. Oversized GIFs fall back to a normal static preview.
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
