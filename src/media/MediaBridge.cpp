#include "media/MediaBridge.h"

#include "matrix/MatrixClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QMutexLocker>
#include <QSaveFile>

namespace {
QString mediaCacheKey(const QString &mediaKey, int kind)
{
    return (kind == 1 ? QStringLiteral("thumb:") : QStringLiteral("full:"))
        + mediaKey;
}

QString mxcCacheKey(const QString &mxc, int size)
{
    return QStringLiteral("mxc:%1:%2").arg(size).arg(mxc);
}

bool previewBytesMatchMime(const QByteArray &bytes, const QString &mimetype)
{
    if (mimetype == QLatin1String("image/gif"))
        return bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a");
    if (mimetype == QLatin1String("image/png"))
        return bytes.startsWith("\x89PNG\r\n\x1a\n");
    if (mimetype == QLatin1String("image/jpeg"))
        return bytes.size() >= 3
            && static_cast<unsigned char>(bytes.at(0)) == 0xff
            && static_cast<unsigned char>(bytes.at(1)) == 0xd8
            && static_cast<unsigned char>(bytes.at(2)) == 0xff;
    if (mimetype == QLatin1String("image/webp"))
        return bytes.size() >= 12 && bytes.startsWith("RIFF")
            && bytes.mid(8, 4) == QByteArrayLiteral("WEBP");
    return false;
}
} // namespace

MediaBridge::MediaBridge(QObject *parent)
    : QObject(parent)
{
    m_failureClock.start();
    m_animatedDir = std::make_unique<QTemporaryDir>(
        QDir::tempPath() + QStringLiteral("/lightning-animated-XXXXXX"));
}

void MediaBridge::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::mediaReady,
                this, &MediaBridge::onMediaReady);
        connect(m_client, &MatrixClient::mediaFailed,
                this, &MediaBridge::onMediaFailed);
        connect(m_client, &MatrixClient::loggedOut,
                this, &MediaBridge::onLoggedOut);
    }
    Q_EMIT supportedChanged();
}

bool MediaBridge::supported() const
{
    return m_client && m_client->supportsMediaBridge();
}

QString MediaBridge::cachedSource(const QString &cacheKey) const
{
    QMutexLocker lock(&m_cacheMutex);
    if (!m_cache.contains(cacheKey))
        return {};
    touch(cacheKey);
    return QStringLiteral("image://lightning-media/")
        + QString::fromUtf8(QUrl::toPercentEncoding(cacheKey));
}

QByteArray MediaBridge::cachedBytes(const QString &cacheKey) const
{
    QMutexLocker lock(&m_cacheMutex);
    const auto it = m_cache.constFind(cacheKey);
    if (it == m_cache.constEnd())
        return {};
    touch(cacheKey);
    return it.value();
}

qint64 MediaBridge::cacheBytesUsed() const
{
    QMutexLocker lock(&m_cacheMutex);
    qint64 total = 0;
    for (const QByteArray &bytes : m_cache)
        total += bytes.size();
    return total;
}

void MediaBridge::touch(const QString &cacheKey) const
{
    // Caller holds m_cacheMutex.
    m_lru.removeOne(cacheKey);
    m_lru.prepend(cacheKey);
}

void MediaBridge::insertCache(const QString &cacheKey, const QByteArray &bytes)
{
    QMutexLocker lock(&m_cacheMutex);
    m_cache.insert(cacheKey, bytes);
    touch(cacheKey);
    // Evict least-recently-used entries beyond the byte cap.
    qint64 total = 0;
    for (const QByteArray &entry : m_cache)
        total += entry.size();
    while (total > m_cacheLimit && m_lru.size() > 1) {
        const QString victim = m_lru.takeLast();
        if (victim == cacheKey)
            continue;
        total -= m_cache.value(victim).size();
        m_cache.remove(victim);
    }
}

bool MediaBridge::alreadyPending(const QString &cacheKey) const
{
    for (const Pending &p : m_inflight) {
        if (p.cacheKey == cacheKey && !p.saveRequest)
            return true;
    }
    for (const Pending &p : m_queue) {
        if (p.cacheKey == cacheKey && !p.saveRequest)
            return true;
    }
    return false;
}

QString MediaBridge::failureCategory(const QString &cacheKey) const
{
    return m_failed.value(cacheKey).category;
}

void MediaBridge::retry(const QString &cacheKey)
{
    m_failed.remove(cacheKey);
}

bool MediaBridge::failureBlocks(const QString &cacheKey)
{
    const auto it = m_failed.find(cacheKey);
    if (it == m_failed.end())
        return false;
    // Validation failures never fix themselves; only an explicit retry()
    // may re-dispatch them.
    const bool permanent = it->category == QLatin1String("rejected")
        || it->category == QLatin1String("invalid_gif");
    if (permanent)
        return true;
    if (m_failureClock.elapsed() - it->markedAtMs >= m_failureRetryMs) {
        m_failed.erase(it);
        return false;
    }
    return true;
}

QString MediaBridge::mediaSource(const QString &mediaKey, const QString &kind)
{
    if (mediaKey.isEmpty() || !supported())
        return {};
    const int kindValue = kind == QLatin1String("thumb") ? 1 : 0;
    const QString cacheKey = mediaCacheKey(mediaKey, kindValue);
    const QString cached = cachedSource(cacheKey);
    if (!cached.isEmpty())
        return cached;
    // A marked failure blocks re-dispatch (transient marks expire; see
    // failureBlocks) — QML repolling a broken source must not turn into a
    // request loop.
    if (failureBlocks(cacheKey))
        return {};
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = kindValue;
        dispatch(request);
    }
    return {};
}

QString MediaBridge::animatedSource(const QString &mediaKey)
{
    if (mediaKey.isEmpty() || mediaKey.contains(QLatin1String("send-queue.localhost"))
        || !supported())
        return {};
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const QString path = m_animatedFiles.value(cacheKey);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        m_animatedLru.removeOne(cacheKey);
        m_animatedLru.prepend(cacheKey);
        return QUrl::fromLocalFile(path).toString();
    }
    m_animatedWanted.insert(cacheKey);
    if (failureBlocks(cacheKey))
        return {};
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        const QString written = writeAnimatedFile(cacheKey, cached,
                                                   QStringLiteral("image/gif"));
        return written.isEmpty() ? QString{} : QUrl::fromLocalFile(written).toString();
    }
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = 0;
        dispatch(request);
    }
    return {};
}

QString MediaBridge::previewAnimatedSource(const QString &dataSource,
                                           const QString &mimetype)
{
    if (mimetype != QLatin1String("image/gif")
        || !dataSource.startsWith(QLatin1String("data:image/gif;base64,")))
        return {};
    const QByteArray bytes = QByteArray::fromBase64(
        dataSource.mid(dataSource.indexOf(QLatin1Char(',')) + 1).toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (bytes.isEmpty())
        return {};
    const QString cacheKey = QStringLiteral("preview:") + QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString existing = m_animatedFiles.value(cacheKey);
    if (!existing.isEmpty() && QFileInfo::exists(existing)) {
        m_animatedLru.removeOne(cacheKey);
        m_animatedLru.prepend(cacheKey);
        return QUrl::fromLocalFile(existing).toString();
    }
    const QString path = writeAnimatedFile(cacheKey, bytes, mimetype);
    return path.isEmpty() ? QString{} : QUrl::fromLocalFile(path).toString();
}

QString MediaBridge::previewImageSource(const QString &dataSource,
                                        const QString &mimetype)
{
    static constexpr qsizetype kMaxPreviewBytes = 5 * 1024 * 1024;
    const QString prefix = QStringLiteral("data:") + mimetype
        + QStringLiteral(";base64,");
    if (!dataSource.startsWith(prefix)
        || !mimetype.startsWith(QLatin1String("image/")))
        return {};
    const QByteArray bytes = QByteArray::fromBase64(
        dataSource.mid(prefix.size()).toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (bytes.isEmpty() || bytes.size() > kMaxPreviewBytes
        || !previewBytesMatchMime(bytes, mimetype))
        return {};
    const QString cacheKey = QStringLiteral("preview-image:")
        + QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (cachedSource(cacheKey).isEmpty())
        insertCache(cacheKey, bytes);
    return cachedSource(cacheKey);
}

QString MediaBridge::avatarSource(const QString &mxcUri, int size)
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
    const int edge = qBound(16, size, 512);
    const QString cacheKey = mxcCacheKey(mxcUri, edge);
    const QString cached = cachedSource(cacheKey);
    if (!cached.isEmpty())
        return cached;
    if (failureBlocks(cacheKey))
        return {};
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.isMxc = true;
        request.mediaKey = mxcUri;
        request.kind = 2;
        request.size = edge;
        dispatch(request);
    }
    return {};
}

void MediaBridge::dispatch(const Pending &request)
{
    if (m_inflight.size() >= kMaxConcurrent) {
        m_queue.enqueue(request);
        return;
    }
    quint64 opId = 0;
    if (request.isMxc)
        opId = m_client->fetchMxcThumbnail(request.mediaKey, request.size,
                                           request.size);
    else
        opId = m_client->fetchMedia(request.mediaKey, request.kind);
    if (opId == 0) {
        markFailed(request, QStringLiteral("rejected"));
        Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("rejected"));
        if (request.saveRequest)
            Q_EMIT saveFinished(false, tr("The file could not be downloaded."));
        return;
    }
    m_inflight.insert(opId, request);
}

void MediaBridge::pump()
{
    while (!m_queue.isEmpty() && m_inflight.size() < kMaxConcurrent)
        dispatch(m_queue.dequeue());
}

void MediaBridge::onMediaReady(quint64 opId, const QString &mediaKey, int kind,
                               const QByteArray &bytes, const QString &mimetype,
                               const QString &filename)
{
    Q_UNUSED(mediaKey);
    Q_UNUSED(kind);
    Q_UNUSED(filename);
    const auto it = m_inflight.find(opId);
    if (it == m_inflight.end())
        return; // stale (cleared on sign-out) or foreign op
    const Pending request = it.value();
    m_inflight.erase(it);
    pump();

    if (request.saveRequest) {
        writeSaveFile(request.saveDestination, bytes);
        return;
    }
    m_failed.remove(request.cacheKey);
    insertCache(request.cacheKey, bytes);
    if (m_animatedWanted.remove(request.cacheKey)) {
        if (!writeAnimatedFile(request.cacheKey, bytes, mimetype).isEmpty())
            Q_EMIT animatedMediaReady(request.cacheKey);
        else
            Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("invalid_gif"));
    }
    Q_EMIT mediaCached(request.cacheKey);
}

QString MediaBridge::writeAnimatedFile(const QString &cacheKey,
                                       const QByteArray &bytes,
                                       const QString &mimetype)
{
    constexpr qsizetype maxGifBytes = 20 * 1024 * 1024;
    if (mimetype.section(QLatin1Char(';'), 0, 0).trimmed().toLower()
            != QLatin1String("image/gif")
        || bytes.size() < 10 || bytes.size() > maxGifBytes
        || !(bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))
        || !m_animatedDir || !m_animatedDir->isValid())
        return {};
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(cacheKey.toUtf8(), QCryptographicHash::Sha256).toHex())
        + QStringLiteral(".gif");
    const QString path = m_animatedDir->filePath(name);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size() || !file.commit())
        return {};
    m_animatedFiles.insert(cacheKey, path);
    m_animatedSizes.insert(cacheKey, bytes.size());
    m_animatedLru.removeOne(cacheKey);
    m_animatedLru.prepend(cacheKey);
    qint64 total = 0;
    for (qint64 size : std::as_const(m_animatedSizes))
        total += size;
    while ((total > kAnimatedCacheBytes
            || m_animatedFiles.size() > kAnimatedCacheEntries)
           && m_animatedLru.size() > 1) {
        const QString victim = m_animatedLru.takeLast();
        total -= m_animatedSizes.take(victim);
        QFile::remove(m_animatedFiles.take(victim));
    }
    return path;
}

void MediaBridge::markFailed(const Pending &request, const QString &category)
{
    if (request.saveRequest)
        return; // Save As reports through saveFinished, not source state.
    if (m_failed.size() >= kMaxFailureMarks)
        m_failed.clear(); // defensive bound; never realistically reached
    m_failed.insert(request.cacheKey,
                    {category, m_failureClock.elapsed()});
}

void MediaBridge::onMediaFailed(quint64 opId, const QString &mediaKey, int kind,
                                const QString &category)
{
    Q_UNUSED(mediaKey);
    Q_UNUSED(kind);
    const auto it = m_inflight.find(opId);
    if (it == m_inflight.end())
        return;
    const Pending request = it.value();
    m_inflight.erase(it);
    pump();
    if (request.saveRequest) {
        Q_EMIT saveFinished(false, tr("The file could not be downloaded."));
        return;
    }
    markFailed(request, category);
    Q_EMIT mediaFetchFailed(request.cacheKey, category);
}

QString MediaBridge::sanitizedFileName(const QString &name)
{
    QString out = QFileInfo(name).fileName(); // strips any path components
    out.replace(QLatin1Char('\0'), QLatin1Char('_'));
    if (out.isEmpty() || out == QLatin1String(".") || out == QLatin1String(".."))
        out = QStringLiteral("download");
    return out;
}

void MediaBridge::saveAs(const QString &mediaKey, const QUrl &destination)
{
    if (mediaKey.isEmpty() || !supported() || !destination.isLocalFile()) {
        Q_EMIT saveFinished(false, tr("No destination selected."));
        return;
    }
    // Serve from cache when the full payload is already in memory.
    const QByteArray cached = cachedBytes(mediaCacheKey(mediaKey, 0));
    if (!cached.isEmpty()) {
        writeSaveFile(destination, cached);
        return;
    }
    Pending request;
    request.cacheKey = mediaCacheKey(mediaKey, 0);
    request.mediaKey = mediaKey;
    request.kind = 0;
    request.saveRequest = true;
    request.saveDestination = destination;
    dispatch(request);
}

void MediaBridge::writeSaveFile(const QUrl &destination, const QByteArray &bytes)
{
    const QFileInfo chosen(destination.toLocalFile());
    // The user picked the directory; the file name is re-sanitized so a
    // hostile attachment name can never traverse out of it.
    const QString target =
        chosen.dir().filePath(sanitizedFileName(chosen.fileName()));
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        Q_EMIT saveFinished(false, tr("The destination is not writable."));
        return;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        Q_EMIT saveFinished(false, tr("Writing the file failed."));
        return;
    }
    Q_EMIT saveFinished(true, tr("Saved."));
}

void MediaBridge::clear()
{
    {
        QMutexLocker lock(&m_cacheMutex);
        m_cache.clear();
        m_lru.clear();
    }
    m_inflight.clear();
    m_queue.clear();
    m_failed.clear();
    m_animatedFiles.clear();
    m_animatedSizes.clear();
    m_animatedLru.clear();
    m_animatedWanted.clear();
    m_animatedDir.reset(); // recursively removes decrypted temporary files
    m_animatedDir = std::make_unique<QTemporaryDir>(
        QDir::tempPath() + QStringLiteral("/lightning-animated-XXXXXX"));
}

void MediaBridge::onLoggedOut()
{
    // Decrypted media must not outlive the session in memory, and no stale
    // completion may repopulate the cache for the next account.
    clear();
}
