#include "app/AccountAvatarStore.h"

#include "storage/AppDataPaths.h"

#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

/// The same shape-based SVG refusal `MediaBridge` applies: every accepted
/// raster format opens with binary magic, so data starting with `<` is markup.
bool looksLikeSupportedRaster(const QByteArray &bytes)
{
    if (bytes.size() < 12)
        return false;
    if (bytes.startsWith("\x89PNG\r\n\x1a\n"))
        return true;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xD8
        && static_cast<unsigned char>(bytes[2]) == 0xFF)
        return true;
    if (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))
        return true;
    if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == QByteArray("WEBP"))
        return true;
    return false;
}

QString fileFor(const QString &userId)
{
    const QString slug = matrix::app_data::safeUserSlug(userId.trimmed());
    if (slug.isEmpty())
        return {};
    const QString root = AccountAvatarStore::storeRoot();
    if (root.isEmpty())
        return {};
    // One extension for every format; QML's loader sniffs the bytes.
    return root + QLatin1Char('/') + slug + QStringLiteral(".avatar");
}

} // namespace

AccountAvatarStore::AccountAvatarStore(QObject *parent)
    : QObject(parent)
{
}

QString AccountAvatarStore::storeRoot()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        return {};
    return base + QStringLiteral("/account-avatars");
}

QString AccountAvatarStore::avatarUrlFor(const QString &userId) const
{
    const QString path = fileFor(userId);
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (!info.exists() || info.size() <= 0)
        return {};
    // The mtime makes a replaced picture reload (Qt caches Image by URL).
    return QStringLiteral("file://") + path + QStringLiteral("?m=")
        + QString::number(info.lastModified().toSecsSinceEpoch());
}

bool AccountAvatarStore::store(const QString &userId, const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > kMaxBytes)
        return false;
    if (!looksLikeSupportedRaster(bytes))
        return false;
    const QString path = fileFor(userId);
    if (path.isEmpty())
        return false;
    const QString root = storeRoot();
    if (!QDir().mkpath(root))
        return false;
    // 0700/0600 before writing: the file name reveals a Matrix account.
    QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return false;
    Q_EMIT avatarStored(userId);
    return true;
}

bool AccountAvatarStore::forget(const QString &userId)
{
    const QString path = fileFor(userId);
    if (path.isEmpty() || !QFile::exists(path))
        return false;
    // An absent file returns false: "target absent" is not "removed".
    if (!QFile::remove(path))
        return false;
    Q_EMIT avatarStored(userId);
    return true;
}
