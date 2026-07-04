#include "matrix/MediaHelpers.h"

#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrlQuery>

namespace matrix::media {

bool parseMxc(const QString &mxc, QString *serverName, QString *mediaId)
{
    static const QLatin1String scheme("mxc://");
    if (!mxc.startsWith(scheme))
        return false;
    const QString rest = mxc.mid(scheme.size());
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash + 1 >= rest.size())
        return false;
    if (serverName) *serverName = rest.left(slash);
    if (mediaId)    *mediaId    = rest.mid(slash + 1);
    return true;
}

static QString trimTrailingSlash(const QString &base)
{
    QString b = base;
    while (b.endsWith(QLatin1Char('/')))
        b.chop(1);
    return b;
}

QUrl downloadUrl(const QString &homeserverBase, const QString &mxc)
{
    QString server, mediaId;
    if (!parseMxc(mxc, &server, &mediaId))
        return {};
    const QString base = trimTrailingSlash(homeserverBase);
    if (base.isEmpty())
        return {};
    return QUrl(QStringLiteral("%1/_matrix/media/v3/download/%2/%3")
                    .arg(base, server, mediaId));
}

QUrl thumbnailUrl(const QString &homeserverBase,
                  const QString &mxc,
                  int width, int height,
                  bool crop)
{
    if (width <= 0 || height <= 0)
        return downloadUrl(homeserverBase, mxc);

    QString server, mediaId;
    if (!parseMxc(mxc, &server, &mediaId))
        return {};
    const QString base = trimTrailingSlash(homeserverBase);
    if (base.isEmpty())
        return {};

    QUrl url(QStringLiteral("%1/_matrix/media/v3/thumbnail/%2/%3")
                 .arg(base, server, mediaId));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("width"),  QString::number(width));
    q.addQueryItem(QStringLiteral("height"), QString::number(height));
    q.addQueryItem(QStringLiteral("method"), crop ? QStringLiteral("crop")
                                                  : QStringLiteral("scale"));
    url.setQuery(q);
    return url;
}

QString mimetypeForFile(const QString &localPath)
{
    static QMimeDatabase db;
    const auto mt = db.mimeTypeForFile(QFileInfo(localPath));
    return mt.isValid() ? mt.name() : QStringLiteral("application/octet-stream");
}

bool isImageMimetype(const QString &mimetype)
{
    return mimetype.startsWith(QLatin1String("image/"), Qt::CaseInsensitive);
}

QString previewSnippet(const QString &body, int maxChars)
{
    QString s = body;
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    s = s.trimmed();
    if (s.size() > maxChars)
        s = s.left(maxChars - 1) + QChar(0x2026); // horizontal ellipsis
    return s;
}

} // namespace matrix::media
