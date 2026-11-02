#pragma once

#include <QString>
#include <QUrl>

namespace matrix::media {

bool parseMxc(const QString &mxc, QString *serverName, QString *mediaId);

QUrl downloadUrl(const QString &homeserverBase, const QString &mxc);

QUrl thumbnailUrl(const QString &homeserverBase,
                  const QString &mxc,
                  int width, int height,
                  bool crop = false);

QString mimetypeForFile(const QString &localPath);

bool isImageMimetype(const QString &mimetype);

QString previewSnippet(const QString &body, int maxChars = 80);

}
