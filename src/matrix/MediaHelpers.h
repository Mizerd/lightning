#pragma once

#include <QString>
#include <QUrl>

// Small helpers for Matrix media URLs, used by the non-Rust backends. They
// build legacy unauthenticated /_matrix/media/v3 URLs; the Rust backend goes
// through the SDK's authenticated media API instead.
namespace matrix::media {

// Parses `mxc://server.name/mediaId` into (serverName, mediaId).
// Returns false if the URL is malformed.
bool parseMxc(const QString &mxc, QString *serverName, QString *mediaId);

// Builds the download URL against the given homeserver base. Returns an
// invalid QUrl for malformed input.
QUrl downloadUrl(const QString &homeserverBase, const QString &mxc);

// Builds a thumbnail URL. Returns downloadUrl() if width/height are zero.
QUrl thumbnailUrl(const QString &homeserverBase,
                  const QString &mxc,
                  int width, int height,
                  bool crop = false);

// Best-effort filename → mimetype detection using QMimeDatabase.
QString mimetypeForFile(const QString &localPath);

// True if mimetype starts with "image/".
bool isImageMimetype(const QString &mimetype);

// Trim body to a single line, capped at `maxChars`, for reply and
// last-message previews.
QString previewSnippet(const QString &body, int maxChars = 80);

} // namespace matrix::media
