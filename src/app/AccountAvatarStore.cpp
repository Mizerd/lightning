#include "app/AccountAvatarStore.h"

#include "storage/AppDataPaths.h"

#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

/// The same shape-based refusal `MediaBridge` applies at its choke point, for
/// the same reason (CLAUDE.md §6): every raster format this client accepts
/// opens with binary magic, so an "image" beginning with `<` is markup — SVG
/// or worse — and an avatar is never that. Listing the spellings an attacker
/// must avoid is what makes a list evadable; this asks what the bytes ARE.
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
    // One extension for every format. The bytes carry their own magic and
    // QML's image loader sniffs rather than trusting the suffix, so a
    // per-format extension would only create a second thing to keep in step
    // with the first — and a stale `.png` beside a new `.webp` is exactly the
    // "which one is current" question this store exists to avoid.
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
    // See the header: the mtime is what makes a REPLACED picture reload. Qt
    // caches an Image by its source string, so a constant path would show the
    // first picture for the life of the process.
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
    // 0700 on the directory and 0600 on the file, set BEFORE the bytes are
    // written, exactly as the animated-media and playable-file writers do.
    // An avatar is public content, so this is not protecting a secret — it is
    // protecting the FACT that this machine holds a file named after a Matrix
    // account, which is the same reasoning §6 gives for account-scoped paths.
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
    // "Target absent" and "removed" are different outcomes (§6): a caller
    // that reports a cleanup must be able to tell them apart, so an absent
    // file returns false rather than a cheerful true.
    if (!QFile::remove(path))
        return false;
    Q_EMIT avatarStored(userId);
    return true;
}
