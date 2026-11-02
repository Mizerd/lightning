#pragma once

#include <QString>
#include <QUrl>

namespace matrix::link_preview {

QString firstPreviewableUrl(const QString &body);

QString sanitizedHost(const QString &url);
QString linkifiedMessageHtml(const QString &body);
QString linkifiedTopicHtml(const QString &topic);
bool isSafeExternalUrl(const QUrl &url);

// A preview is a GIF only when the validated MIME type (from og:image:type,
// never the URL suffix) is image/gif and the metadata is within limits.
// Oversized GIFs fall back to a static preview.
struct GifLimits {
    qint64 maxBytes = 10 * 1024 * 1024;
    int maxWidth = 2048;
    int maxHeight = 2048;
};

enum class GifClass {
    NotGif,
    Gif,
    Oversized,
};

GifClass classifyGif(const QString &validatedMime, qint64 sizeBytes,
                     int width, int height, const GifLimits &limits = {});

}
