#include "media/MediaBridge.h"

#include "matrix/MatrixClient.h"

#include <QDir>
#include <QFileInfo>
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
} // namespace

MediaBridge::MediaBridge(QObject *parent)
    : QObject(parent)
{
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

QString MediaBridge::mediaSource(const QString &mediaKey, const QString &kind)
{
    if (mediaKey.isEmpty() || !supported())
        return {};
    const int kindValue = kind == QLatin1String("thumb") ? 1 : 0;
    const QString cacheKey = mediaCacheKey(mediaKey, kindValue);
    const QString cached = cachedSource(cacheKey);
    if (!cached.isEmpty())
        return cached;
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = kindValue;
        dispatch(request);
    }
    return {};
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
    Q_UNUSED(mimetype);
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
    insertCache(request.cacheKey, bytes);
    Q_EMIT mediaCached(request.cacheKey);
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
}

void MediaBridge::onLoggedOut()
{
    // Decrypted media must not outlive the session in memory, and no stale
    // completion may repopulate the cache for the next account.
    clear();
}
