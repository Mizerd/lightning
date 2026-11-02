#include "gif/GifStarredStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>

GifStarredStore::GifStarredStore(QObject *parent)
    : QObject(parent)
    , m_model(std::make_unique<GifStarredModel>(nullptr, this))
{
    connect(m_model.get(), &GifStoredModel::countChanged,
            this, &GifStarredStore::countChanged);
}

GifStarredStore::~GifStarredStore() = default;

bool GifStarredStore::isSafeHashHex(const QString &hash)
{
    if (hash.size() != 64)
        return false;
    for (const QChar c : hash) {
        const ushort u = c.unicode();
        const bool digit = u >= '0' && u <= '9';
        const bool lower = u >= 'a' && u <= 'f';
        if (!digit && !lower)
            return false;
    }
    return true;
}

QString GifStarredStore::filePath(const QString &hash, const QString &ext) const
{
    const QString suffix = ext.isEmpty() ? QStringLiteral("gif") : ext;
    return m_dir + QLatin1Char('/') + hash + QLatin1Char('.') + suffix;
}

QString GifStarredStore::extForHash(const QString &hash) const
{
    for (int i = 0; i < m_model->count(); ++i) {
        const gif::GifResult r = m_model->resultAt(i);
        if (r.id == hash)
            return r.localExt.isEmpty() ? QStringLiteral("gif") : r.localExt;
    }
    return QString();
}

bool GifStarredStore::ensureDirectory() const
{
    if (m_dir.isEmpty())
        return false;
    if (!QDir().mkpath(m_dir))
        return false;
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ExeOwner);
    return true;
}

QString GifStarredStore::categoryMessage(const QString &category) const
{
    if (category == QLatin1String("not_a_gif")
        || category == QLatin1String("unsupported_format"))
        return tr("That file is not a supported image.");
    if (category == QLatin1String("invalid_media"))
        return tr("That file could not be read.");
    if (category == QLatin1String("too_large"))
        return tr("That image is too large to save.");
    if (category == QLatin1String("cap_items")) {
        return tr("Saved GIFs is full (%1 items) — remove one first.")
            .arg(m_maxItems);
    }
    if (category == QLatin1String("cap_bytes")) {
        const qint64 mib = m_maxTotalBytes / (1024 * 1024);
        return tr("Saved GIFs is full (%1 MiB) — remove one first.")
            .arg(mib);
    }
    if (category == QLatin1String("write_failed"))
        return tr("Lightning could not save that GIF to this device.");
    if (category == QLatin1String("not_open"))
        return tr("Saved GIFs is unavailable right now.");
    if (category == QLatin1String("unavailable"))
        return tr("The GIF could not be fetched.");
    if (category == QLatin1String("timeout"))
        return tr("Fetching the GIF timed out.");
    if (category == QLatin1String("already_starred"))
        return tr("Already in your saved GIFs.");
    return tr("The GIF could not be saved.");
}

void GifStarredStore::openFor(const QString &accountDir)
{
    m_settings.reset();
    m_dir.clear();
    m_mediaKeyToHash.clear();

    const QString dir = accountDir.trimmed();
    if (dir.isEmpty()) {
        m_model->reopen(nullptr);
        return;
    }
    // Refuse a symlinked account root or store directory: writing decrypted
    // bytes or deleting through a link could land anywhere (as
    // isSafeAccountIdentity does for the Rust store). The directory is created
    // lazily on first write.
    const QFileInfo dirInfo(dir);
    const QFileInfo parentInfo(dirInfo.dir().absolutePath());
    if (dirInfo.isSymLink() || parentInfo.isSymLink()) {
        m_model->reopen(nullptr);
        return;
    }
    m_dir = dir;
    m_settings = std::make_unique<QSettings>(
        dir + QStringLiteral("/index.ini"), QSettings::IniFormat);
    m_model->reopen(m_settings.get());

    QStringList stale;
    for (int i = 0; i < m_model->count(); ++i) {
        const gif::GifResult r = m_model->resultAt(i);
        const QString ext = r.localExt.isEmpty() ? QStringLiteral("gif")
                                                  : r.localExt;
        if (!isSafeHashHex(r.id) || !QFileInfo::exists(filePath(r.id, ext)))
            stale.append(r.id);
    }
    for (const QString &hash : std::as_const(stale))
        m_model->unstar(hash);

    QDir dirObj(m_dir);
    if (dirObj.exists()) {
        const auto entries = dirObj.entryList(
            { QStringLiteral("*.gif"), QStringLiteral("*.png"),
              QStringLiteral("*.jpg"), QStringLiteral("*.webp") },
            QDir::Files);
        for (const QString &entry : entries) {
            const int dot = entry.lastIndexOf(QLatin1Char('.'));
            const QString hash = dot > 0 ? entry.left(dot) : QString();
            if (hash.isEmpty() || !isSafeHashHex(hash) || !m_model->hasHash(hash))
                QFile::remove(dirObj.filePath(entry));
        }
    }
}

void GifStarredStore::close()
{
    openFor(QString());
}

void GifStarredStore::starBytes(const QString &mediaKey, const QByteArray &bytes)
{
    if (!isOpen()) {
        Q_EMIT starFinished(mediaKey, false, QStringLiteral("not_open"),
                            categoryMessage(QStringLiteral("not_open")));
        return;
    }
    const gif::RasterByteValidation v = gif::validateRasterBytes(bytes);
    if (!v.ok) {
        Q_EMIT starFinished(mediaKey, false, v.category,
                            categoryMessage(v.category));
        return;
    }
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString path = filePath(hash, v.ext);
    const bool existing = m_model->hasHash(hash) && QFileInfo::exists(path);
    if (!existing) {
        if (m_model->count() >= m_maxItems) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("cap_items"),
                                categoryMessage(QStringLiteral("cap_items")));
            return;
        }
        if (m_model->totalBytes() + bytes.size() > m_maxTotalBytes) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("cap_bytes"),
                                categoryMessage(QStringLiteral("cap_bytes")));
            return;
        }
        if (!ensureDirectory()) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("write_failed"),
                                categoryMessage(QStringLiteral("write_failed")));
            return;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("write_failed"),
                                categoryMessage(QStringLiteral("write_failed")));
            return;
        }
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (file.write(bytes) != bytes.size() || !file.commit()) {
            Q_EMIT starFinished(mediaKey, false, QStringLiteral("write_failed"),
                                categoryMessage(QStringLiteral("write_failed")));
            return;
        }
    }
    gif::GifResult r;
    r.provider = QStringLiteral("local");
    r.id = hash;
    r.gifWidth = v.width;
    r.gifHeight = v.height;
    r.gifBytes = bytes.size();
    r.localExt = v.ext;
    m_model->insertLocal(r);
    m_mediaKeyToHash.insert(mediaKey, hash);
    if (existing) {
        Q_EMIT starFinished(mediaKey, true, QStringLiteral("already_starred"),
                            categoryMessage(QStringLiteral("already_starred")));
    } else {
        Q_EMIT starFinished(mediaKey, true, QString(), QString());
    }
}

void GifStarredStore::unstar(const QString &hash)
{
    if (!isOpen() || !isSafeHashHex(hash))
        return;
    const QString ext = extForHash(hash);
    if (!ext.isEmpty())
        QFile::remove(filePath(hash, ext));
    for (auto it = m_mediaKeyToHash.begin(); it != m_mediaKeyToHash.end();) {
        if (it.value() == hash)
            it = m_mediaKeyToHash.erase(it);
        else
            ++it;
    }
    const bool removed = m_model->unstar(hash);
    if (removed)
        Q_EMIT unstarFinished(hash);
}

void GifStarredStore::unstarByMediaKey(const QString &mediaKey)
{
    const QString hash = m_mediaKeyToHash.value(mediaKey);
    if (!hash.isEmpty())
        unstar(hash);
}

void GifStarredStore::clearAll()
{
    if (!isOpen() || m_model->count() == 0)
        return;
    for (int i = 0; i < m_model->count(); ++i) {
        const gif::GifResult r = m_model->resultAt(i);
        if (isSafeHashHex(r.id)) {
            const QString ext =
                r.localExt.isEmpty() ? QStringLiteral("gif") : r.localExt;
            QFile::remove(filePath(r.id, ext));
        }
    }
    m_mediaKeyToHash.clear();
    m_model->clearAll();
}

QByteArray GifStarredStore::readBytes(const QString &hash) const
{
    if (!isOpen() || !isSafeHashHex(hash))
        return {};
    const QString ext = extForHash(hash);
    if (ext.isEmpty())
        return {};
    QFile file(filePath(hash, ext));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    if (file.size() > gif::kMaxGifBytes)
        return {};
    return file.readAll();
}

QString GifStarredStore::source(const QString &hash) const
{
    if (!isOpen() || !isSafeHashHex(hash))
        return {};
    const QString ext = extForHash(hash);
    if (ext.isEmpty())
        return {};
    const QString path = filePath(hash, ext);
    if (!QFileInfo::exists(path))
        return {};
    return QUrl::fromLocalFile(path).toString();
}

QString GifStarredStore::sourceExt(const QString &hash) const
{
    if (!isOpen() || !isSafeHashHex(hash))
        return {};
    return extForHash(hash);
}
