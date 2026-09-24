#include "media/MediaBridge.h"

#include "media/ImageFormatSupport.h"

#include "storage/PortableMode.h"


#include "app/GuiStallTracer.h"

#include "matrix/MatrixClient.h"
#include "media/PlayableFileWriter.h"
#include "media/VideoPosterExtractor.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QImageReader>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <cstring>

// Media pipeline diagnostics (QT_LOGGING_RULES="lightning.media.debug=true").
// Logs only sanitized key tags, byte counts, coarse MIME and timings; never
// decrypted bytes, bodies, authenticated URLs, tokens or provider keys.
Q_LOGGING_CATEGORY(lcMedia, "lightning.media")

// Cache hits, off by default. mediaSource()/avatarSource() re-run on every
// delegate rebind while scrolling, so logging hits on lightning.media (whose
// debug output is on by default) flooded the GUI thread. Misses, dispatches
// and failures stay on lightning.media.
Q_LOGGING_CATEGORY(lcMediaTrace, "lightning.media.trace", QtWarningMsg)

namespace {
QString mediaCacheKey(const QString &mediaKey, int kind)
{
    return (kind == 1 ? QStringLiteral("thumb:")
                      : kind == 2 ? QStringLiteral("listthumb:")
                                  : QStringLiteral("full:"))
        + mediaKey;
}

// Log-safe tag for a cache key: SDK media keys can embed room/event
// structure, so everything after the scope prefix is reduced to a short
// SHA-256 prefix.
QString keyTag(const QString &cacheKey)
{
    const qsizetype colon = cacheKey.indexOf(QLatin1Char(':'));
    const QString scope = colon > 0 ? cacheKey.left(colon) : cacheKey;
    const QString shortHash = QString::fromLatin1(
        QCryptographicHash::hash(cacheKey.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(10));
    return scope + QLatin1Char('#') + shortHash;
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
    // JPEG XL. Container form first: a container's payload starts with the bare
    // codestream signature, so the short test would mis-match it.
    if (mimetype == QLatin1String("image/jxl")) {
        return bytes.startsWith(
                   QByteArrayLiteral("\x00\x00\x00\x0CJXL \r\n\x87\n"))
            || bytes.startsWith(QByteArrayLiteral("\xff\x0a"));
    }
    // Fail closed: an unrecognised type is refused.
    return false;
}
} // namespace

MediaBridge::MediaBridge(QObject *parent)
    : QObject(parent)
    , m_playableMaxEntries(kPlayableCacheEntries)
    , m_playableMaxBytes(kPlayableCacheBytes)
{
    m_failureClock.start();
    m_animatedDir = std::make_unique<QTemporaryDir>(
        lightning::portable::mediaScratchRoot()
        + QStringLiteral("/lightning-animated-XXXXXX"));
    // Marked live so another instance's stale-scratch sweep cannot delete this
    // session's files; the sweep's one-hour mtime floor alone does not protect
    // a long session.
    lightning::portable::holdScratchDirLive(m_animatedDir->path());
    // Reclaims slots pinned by ops the backend never completes. The 5 s cadence
    // only bounds reclaim latency; healthy fetches never reach the timeout.
    m_watchdog.setInterval(5000);
    m_watchdog.setTimerType(Qt::CoarseTimer);
    connect(&m_watchdog, &QTimer::timeout,
            this, &MediaBridge::checkInflightTimeouts);
    m_watchdog.start();

    // One summary line per burst; 900 ms of quiet is past a room's avatar
    // fan-out.
    m_burstSummary.setSingleShot(true);
    m_burstSummary.setInterval(900);
    m_burstSummary.setTimerType(Qt::CoarseTimer);
    connect(&m_burstSummary, &QTimer::timeout, this, [this] {
        if (m_burstCompleted == 0 && m_burstFailed == 0)
            return;
        qCDebug(lcMedia,
                "media burst: %lld fetched (%lld KiB), %lld failed, "
                "peak queue %lld — per-request detail is in "
                "lightning.media.trace",
                static_cast<long long>(m_burstCompleted),
                static_cast<long long>(m_burstBytes / 1024),
                static_cast<long long>(m_burstFailed),
                static_cast<long long>(m_burstPeakQueued));
        m_burstCompleted = 0;
        m_burstFailed = 0;
        m_burstBytes = 0;
        m_burstPeakQueued = 0;
    });
}

MediaBridge::~MediaBridge()
{
    // Release the live mark before QTemporaryDir removes the directory, or
    // QLockFile later fails to remove its own lock file at exit.
    if (m_animatedDir)
        lightning::portable::releaseScratchDir(m_animatedDir->path());
}

void MediaBridge::noteMediaActivity()
{
    m_burstPeakQueued = qMax(m_burstPeakQueued,
                             static_cast<qint64>(m_queue.size()));
    m_burstSummary.start();
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

bool MediaBridge::isAvatarClassKey(const QString &cacheKey)
{
    return cacheKey.startsWith(QLatin1String("mxc:"));
}

bool MediaBridge::looksLikeMarkupOrCompressed(const QByteArray &bytes)
{
    // SVG is kept out of inline media paths (CLAUDE.md §6). The mimetype cannot
    // enforce that: sticker packs are member-writable room state and MSC2545
    // lets `mimetype` be omitted, so the bytes decide. Every accepted raster
    // format starts with binary magic, so refusing a leading `<` or gzip has no
    // false positives and nothing to evade.
    qsizetype i = 0;
    // A UTF-8 BOM and whitespace may precede an XML declaration.
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes.at(0)) == 0xEF
        && static_cast<unsigned char>(bytes.at(1)) == 0xBB
        && static_cast<unsigned char>(bytes.at(2)) == 0xBF)
        i = 3;
    // Not std::isspace: locale-dependent, and UB for negative char values.
    const auto isXmlSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (i < bytes.size() && isXmlSpace(bytes.at(i)))
        ++i;
    if (i < bytes.size() && bytes.at(i) == '<')
        return true;
    // SVGZ: Qt's SVG handler decompresses gzip.
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes.at(0)) == 0x1F
        && static_cast<unsigned char>(bytes.at(1)) == 0x8B)
        return true;
    return false;
}

bool MediaBridge::looksLikeAvContainer(const QByteArray &bytes)
{
    if (bytes.size() < 12)
        return false;
    const auto u8 = [&bytes](qsizetype i) {
        return static_cast<unsigned char>(bytes.at(i));
    };
    if (bytes.mid(4, 4) == QByteArrayLiteral("ftyp")) {
        // ISO BMFF, but HEIC/AVIF images share the container.
        const QByteArray brand = bytes.mid(8, 4);
        if (brand == QByteArrayLiteral("avif")
            || brand == QByteArrayLiteral("avis")
            || brand == QByteArrayLiteral("heic")
            || brand == QByteArrayLiteral("heix")
            || brand == QByteArrayLiteral("mif1"))
            return false;
        return true; // MP4/M4A/MOV
    }
    if (u8(0) == 0x1A && u8(1) == 0x45 && u8(2) == 0xDF && u8(3) == 0xA3)
        return true; // Matroska/WebM
    if (bytes.startsWith("OggS"))
        return true;
    if (bytes.startsWith("RIFF")) {
        const QByteArray form = bytes.mid(8, 4);
        // RIFF/WEBP is an image and stays acceptable.
        if (form == QByteArrayLiteral("AVI ")
            || form == QByteArrayLiteral("WAVE"))
            return true;
    }
    if (bytes.startsWith("fLaC") || bytes.startsWith("ID3"))
        return true;
    return false;
}

QString MediaBridge::cachedSource(const QString &cacheKey) const
{
    QMutexLocker lock(&m_cacheMutex);
    if (!m_cache.contains(cacheKey))
        return {};
    touch(cacheKey);
    // "?r=<revision>" changes only on a byte insert, so a re-cached payload
    // gets a new source and an Image in Error reloads. MediaImageProvider
    // strips it.
    return QStringLiteral("image://lightning-media/")
        + QString::fromUtf8(QUrl::toPercentEncoding(cacheKey))
        + QStringLiteral("?r=")
        + QString::number(m_revision.value(cacheKey, 1));
}

QImage MediaBridge::cachedArtwork(const QString &cacheKey) const
{
    QMutexLocker lock(&m_cacheMutex);
    const auto it = m_artworkCache.constFind(cacheKey);
    return it == m_artworkCache.constEnd() ? QImage{} : it.value();
}

QString MediaBridge::audioArtworkSource(const QString &mediaKey,
                                        const QVariant &artwork)
{
    if (mediaKey.isEmpty() || !artwork.canConvert<QImage>())
        return {};
    const QImage image = artwork.value<QImage>();
    if (image.isNull() || image.width() <= 0 || image.height() <= 0
        || image.width() > kArtworkMaxEdge || image.height() > kArtworkMaxEdge
        || static_cast<qint64>(image.sizeInBytes()) > kArtworkMaxBytes) {
        return {};
    }

    const QString cacheKey = QStringLiteral("artwork:")
        + QString::fromLatin1(
            QCryptographicHash::hash(mediaKey.toUtf8(),
                                     QCryptographicHash::Sha256).toHex());
    int revision = 1;
    {
        QMutexLocker lock(&m_cacheMutex);
        if (!m_artworkCache.contains(cacheKey)) {
            const qint64 bytes = static_cast<qint64>(image.sizeInBytes());
            while (!m_artworkLru.isEmpty()
                   && (m_artworkCache.size() >= kArtworkMaxEntries
                       || m_artworkBytes + bytes > kArtworkMaxBytes)) {
                const QString victim = m_artworkLru.takeLast();
                const auto removed = m_artworkCache.take(victim);
                m_artworkBytes -= static_cast<qint64>(removed.sizeInBytes());
            }
            m_artworkCache.insert(cacheKey, image);
            m_artworkLru.prepend(cacheKey);
            m_artworkBytes += bytes;
            ++m_revision[cacheKey];
        } else {
            m_artworkLru.removeOne(cacheKey);
            m_artworkLru.prepend(cacheKey);
        }
        revision = m_revision.value(cacheKey, 1);
    }
    return QStringLiteral("image://lightning-media/")
        + QString::fromUtf8(QUrl::toPercentEncoding(cacheKey))
        + QStringLiteral("?r=")
        + QString::number(revision);
}

QByteArray MediaBridge::cachedBytes(const QString &cacheKey) const
{
    QMutexLocker lock(&m_cacheMutex);
    const auto it = m_cache.constFind(cacheKey);
    if (it == m_cache.constEnd())
        return {};
    // No touch(): this runs on the image-decode thread, and the O(n) LRU
    // reorder would contend with the GUI thread. cachedSource() already
    // recorded recency.
    return it.value();
}

qint64 MediaBridge::cacheBytesUsed() const
{
    QMutexLocker lock(&m_cacheMutex);
    return m_cacheBytesMain + m_cacheBytesAvatar;
}

void MediaBridge::touch(const QString &cacheKey) const
{
    // Caller holds m_cacheMutex.
    QList<QString> &lru = isAvatarClassKey(cacheKey) ? m_avatarLru : m_lru;
    lru.removeOne(cacheKey);
    lru.prepend(cacheKey);
}

void MediaBridge::insertCache(const QString &cacheKey, const QByteArray &bytes)
{
    QMutexLocker lock(&m_cacheMutex);
    const bool avatarClass = isAvatarClassKey(cacheKey);
    qint64 &classTotal = avatarClass ? m_cacheBytesAvatar : m_cacheBytesMain;
    // An overwrite must retire the old payload's bytes first.
    if (const auto existing = m_cache.constFind(cacheKey);
        existing != m_cache.constEnd())
        classTotal -= existing.value().size();
    m_cache.insert(cacheKey, bytes);
    classTotal += bytes.size();
    // Only a byte insert bumps the revision, so cache hits keep a stable URL.
    ++m_revision[cacheKey];
    touch(cacheKey);
    // Evict LRU entries beyond this class's budget; avatar and main classes
    // never evict each other.
    QList<QString> &lru = avatarClass ? m_avatarLru : m_lru;
    const qint64 limit = avatarClass ? m_avatarCacheLimit : m_cacheLimit;
    while (classTotal > limit && lru.size() > 1) {
        const QString victim = lru.takeLast();
        if (victim == cacheKey)
            continue;
        classTotal -= m_cache.value(victim).size();
        m_cache.remove(victim);
        // Drop the evicted key's memoized hash too.
        m_contentHashCache.remove(victim);
    }
}

bool MediaBridge::alreadyPending(const QString &cacheKey) const
{
    // Save/star requests never populate the cache or emit mediaCached(), so an
    // ordinary caller must not wait on one.
    for (const Pending &p : m_inflight) {
        if (p.cacheKey == cacheKey && !p.saveRequest && !p.starRequest)
            return true;
    }
    for (const Pending &p : m_queue) {
        if (p.cacheKey == cacheKey && !p.saveRequest && !p.starRequest)
            return true;
    }
    return false;
}

QString MediaBridge::failureCategory(const QString &cacheKey) const
{
    return m_failed.value(cacheKey).category;
}

QString MediaBridge::avatarFailureCategory(const QString &mxcUri) const
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")))
        return {};
    return failureCategory(mxcCacheKey(mxcUri, kAvatarCanonicalEdge));
}

void MediaBridge::retry(const QString &cacheKey)
{
    m_failed.remove(cacheKey);
}

bool MediaBridge::isPermanentCategory(const QString &category)
{
    // Only backend-reported validation failures are permanent. Network, timeout
    // and the local "unavailable" dispatch failure (opId 0 during
    // restore/switch) are transient.
    return category == QLatin1String("rejected")
        || category == QLatin1String("invalid_gif");
}

bool MediaBridge::failureBlocks(const QString &cacheKey)
{
    const auto it = m_failed.find(cacheKey);
    if (it == m_failed.end())
        return false;
    if (isPermanentCategory(it->category))
        return true;
    if (m_failureClock.elapsed() - it->markedAtMs >= m_failureRetryMs) {
        m_failed.erase(it);
        return false;
    }
    return true;
}

void MediaBridge::sweepExpiredFailureMarks()
{
    if (m_failed.isEmpty())
        return;
    const qint64 now = m_failureClock.elapsed();
    // Collect first: mediaRetryable handlers re-enter and mutate m_failed.
    QStringList retryable;
    for (auto it = m_failed.begin(); it != m_failed.end();) {
        if (!isPermanentCategory(it->category)
            && now - it->markedAtMs >= m_failureRetryMs) {
            retryable.append(it.key());
            it = m_failed.erase(it);
        } else {
            ++it;
        }
    }
    for (const QString &cacheKey : std::as_const(retryable)) {
        qCDebug(lcMediaTrace, "retryable %s (transient mark expired)",
                qUtf8Printable(keyTag(cacheKey)));
        Q_EMIT mediaRetryable(cacheKey);
    }
}

QString MediaBridge::mediaSource(const QString &mediaKey, const QString &kind)
{
    if (mediaKey.isEmpty() || !supported())
        return {};
    const int kindValue = kind == QLatin1String("thumb") ? 1
        : kind == QLatin1String("list_thumb") ? 2 : 0;
    const QString cacheKey = mediaCacheKey(mediaKey, kindValue);
    const QString cached = cachedSource(cacheKey);
    if (!cached.isEmpty()) {
        ++m_statCacheHit;
        qCDebug(lcMediaTrace, "media %s cache=hit",
                qUtf8Printable(keyTag(cacheKey)));
        return cached;
    }
    // Read-through across classes. The classes limit what a request may fetch
    // (a 42x34 list tile never triggers a full encrypted download), not what it
    // may reuse. Only the smallest class borrows, and only from larger ones:
    // the reverse would render a row at the wrong size and move the reader
    // during a prepend.
    if (kindValue == 2) {
        for (const int larger : { 1, 0 }) {
            const QString borrowed =
                cachedSource(mediaCacheKey(mediaKey, larger));
            if (borrowed.isEmpty())
                continue;
            ++m_statCacheHit;
            qCDebug(lcMediaTrace, "media %s cache=hit(class %d)",
                    qUtf8Printable(keyTag(cacheKey)), larger);
            return borrowed;
        }
    }
    // A failure mark blocks re-dispatch so QML repolling cannot loop.
    if (failureBlocks(cacheKey)) {
        qCDebug(lcMediaTrace, "media %s suppressed=failure-mark(%s)",
                qUtf8Printable(keyTag(cacheKey)),
                qUtf8Printable(failureCategory(cacheKey)));
        return {};
    }
    if (!alreadyPending(cacheKey)) {
        ++m_statCacheMiss;
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = kindValue;
        // Thumbnails are visible chrome; full static payloads wait behind them.
        request.priority = kindValue != 0 ? 1 : 2;
        qCDebug(lcMediaTrace, "media %s cache=miss dispatching",
                qUtf8Printable(keyTag(cacheKey)));
        dispatch(request);
    } else {
        promoteQueuedRequest(cacheKey, kindValue != 0 ? 1 : 2, 0);
    }
    return {};
}

QString MediaBridge::animatedSource(const QString &mediaKey, bool speculative)
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
    // Interest sets record an outstanding fetch, so nothing is added on a path
    // that dispatches none. A stranded key in m_animatedWanted makes
    // cancelPlayable() return early forever.
    if (failureBlocks(cacheKey))
        return {};
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        const QString written = writeAnimatedFile(cacheKey, cached);
        if (written.isEmpty()) {
            // A demanding caller renders nothing else and is owed the answer.
            if (!speculative)
                Q_EMIT mediaFetchFailed(cacheKey, QStringLiteral("invalid_gif"));
            return {};
        }
        return QUrl::fromLocalFile(written).toString();
    }
    m_animatedWanted.insert(cacheKey);
    // A demanding caller (confirmed GIF) is owed mediaFetchFailed when the
    // bytes are not an animation. A speculative one (sticker) is not: its still
    // Image already shows the bytes.
    if (!speculative)
        m_animatedDemanded.insert(cacheKey);
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = 0;
        // Speculative autoplay prefetch; must not starve chrome or playback.
        request.priority = 3;
        dispatch(request);
    }
    return {};
}

QString MediaBridge::mxcAnimatedSource(const QString &mxcUri)
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
    // Distinct from the `mxcimg:<edge>:` still tile, which holds a server
    // thumbnail of the same mxc. Not "mxc:"-prefixed, so it is charged to the
    // main budget and never evicts avatars.
    const QString cacheKey = QStringLiteral("mxcanim:") + mxcUri;
    const QString path = m_animatedFiles.value(cacheKey);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        m_animatedLru.removeOne(cacheKey);
        m_animatedLru.prepend(cacheKey);
        return QUrl::fromLocalFile(path).toString();
    }
    if (failureBlocks(cacheKey))
        return {};
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        // No signal on refusal: this caller never demands.
        const QString written = writeAnimatedFile(cacheKey, cached);
        return written.isEmpty() ? QString{} : QUrl::fromLocalFile(written).toString();
    }
    // Registered only once a fetch is outstanding (see animatedSource()). Not
    // added to m_animatedDemanded: a non-animation is answered with silence.
    m_animatedWanted.insert(cacheKey);
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.isMxc = true;
        request.mediaKey = mxcUri;
        // For mxc requests `kind` is a C++-side classification only. 2
        // (thumbnail class) enables the A/V-container refusal and skips
        // playableSizeLearned.
        request.kind = 2;
        // Size 0 makes media_fetch_mxc (rust/src/rooms.rs) request the original
        // file rather than a single-frame server thumbnail.
        request.size = 0;
        // Speculative: must not starve the still tiles.
        request.priority = 3;
        dispatch(request);
    }
    return {};
}

QString MediaBridge::playableSource(const QString &mediaKey)
{
    if (mediaKey.isEmpty()
        || mediaKey.contains(QLatin1String("send-queue.localhost"))
        || !supported())
        return {};
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const QString path = m_playableFiles.value(cacheKey);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        m_playableLru.removeOne(cacheKey);
        m_playableLru.prepend(cacheKey);
        return QUrl::fromLocalFile(path).toString();
    }
    ++m_playableWanted[cacheKey]; // refcounted
    if (failureBlocks(cacheKey)) {
        // No fetch will run; do not leave a phantom interest count.
        if (--m_playableWanted[cacheKey] <= 0)
            m_playableWanted.remove(cacheKey);
        return {};
    }
    const auto writing = m_playableWriting.find(cacheKey);
    if (writing != m_playableWriting.end()) {
        // Coalesce onto the write already on the worker thread and require it
        // to report failure. The interest count stays for the completion to
        // retire.
        writing->notifyFailure = true;
        return {};
    }
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        // Empty mimetype: the container magic decides.
        if (beginPlayableWrite(cacheKey, mediaKey, cached, {}, true))
            return {}; // materializing off-thread; QML re-asks on the signal
        // Refused before any file was created: fall through to a fetch and keep
        // the interest count, which the fetch's completion consumes.
    }
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = 0;
        request.timeoutClass = 1; // playable class (90s Rust / 100s watchdog)
        request.priority = 0;     // the user pressed Play
        dispatch(request);
    } else {
        // A queued speculative prefetch must not keep its lower class.
        promoteQueuedRequest(cacheKey, 0, 1);
    }
    return {};
}

void MediaBridge::prefetchPlayable(const QString &mediaKey, double sizeBytes)
{
    if (mediaKey.isEmpty()
        || mediaKey.contains(QLatin1String("send-queue.localhost"))
        || !supported())
        return;
    // Only a declared, in-cap size is worth speculative bandwidth.
    const qint64 declared = static_cast<qint64>(sizeBytes);
    if (declared <= 0 || declared > kSpeculativePlayableMaxBytes)
        return;
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const QString path = m_playableFiles.value(cacheKey);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        // Already materialized; only a pending poster hook may remain.
        if (m_posterWanted.remove(cacheKey))
            startPosterExtraction(mediaKey, path);
        return;
    }
    if (failureBlocks(cacheKey))
        return;
    if (m_playableWriting.contains(cacheKey))
        return; // already materializing off-thread for someone else
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        // The write completion fires playableMediaReady and the poster hook. A
        // prefetch is owed no failure signal.
        beginPlayableWrite(cacheKey, mediaKey, cached, {}, false);
        return;
    }
    if (alreadyPending(cacheKey))
        return;
    m_prefetchWanted.insert(cacheKey);
    Pending request;
    request.cacheKey = cacheKey;
    request.mediaKey = mediaKey;
    request.kind = 0;
    request.timeoutClass = 1; // playable-class bound fits the payload size
    request.priority = 3;     // speculative — never crowds explicit intent
    qCDebug(lcMediaTrace, "prefetch %s (declared %lld bytes)",
            qUtf8Printable(keyTag(cacheKey)),
            static_cast<long long>(declared));
    dispatch(request);
}

QString MediaBridge::videoPosterSource(const QString &mediaKey,
                                       double sizeBytes)
{
    if (mediaKey.isEmpty()
        || mediaKey.contains(QLatin1String("send-queue.localhost"))
        || !supported())
        return {};
    const QString posterKey = mediaCacheKey(mediaKey, 1);
    const QString cached = cachedSource(posterKey);
    if (!cached.isEmpty())
        return cached;
    if (failureBlocks(posterKey))
        return {};
    const QString playableKey = mediaCacheKey(mediaKey, 0);
    const QString path = m_playableFiles.value(playableKey);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        startPosterExtraction(mediaKey, path);
        return {};
    }
    // Materialize first (within the speculative cap), then extract once the
    // file exists. Over-cap or unknown-size videos keep the placeholder until
    // played.
    m_posterWanted.insert(playableKey);
    prefetchPlayable(mediaKey, sizeBytes);
    // If the prefetch declined, drop the hook: a hook with no delivery path
    // leaks and vetoes later cancels. A write already on the worker thread
    // counts as a delivery path.
    if (!alreadyPending(playableKey) && !m_playableFiles.contains(playableKey)
        && !m_playableWriting.contains(playableKey))
        m_posterWanted.remove(playableKey);
    return {};
}

void MediaBridge::cancelPlayable(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const auto wanted = m_playableWanted.find(cacheKey);
    if (wanted == m_playableWanted.end())
        return; // no playable consumer was waiting on this key
    // Another card still waits on the same bytes.
    if (--wanted.value() > 0)
        return;
    m_playableWanted.erase(wanted);
    // A GIF row wanting the same bytes keeps the fetch alive. A poster hook or
    // prefetch does not veto a user cancel; the poster can be re-derived later.
    if (m_animatedWanted.contains(cacheKey)) {
        m_prefetchWanted.remove(cacheKey);
        m_posterWanted.remove(cacheKey);
        return;
    }
    m_prefetchWanted.remove(cacheKey);
    m_posterWanted.remove(cacheKey);
    // Abandon a write already on the worker thread too. QSaveFile discards the
    // partial file and no completion is emitted; a fresh Play re-materializes
    // from the cached bytes.
    if (m_playableWriting.remove(cacheKey) > 0 && m_playableWriter)
        m_playableWriter->cancel(cacheKey);
    for (int i = m_queue.size() - 1; i >= 0; --i) {
        const Pending &p = m_queue.at(i);
        if (p.cacheKey == cacheKey && !p.saveRequest && !p.starRequest)
            m_queue.removeAt(i);
    }
    for (auto it = m_inflight.begin(); it != m_inflight.end(); ++it) {
        if (it->cacheKey == cacheKey && !it->saveRequest && !it->starRequest) {
            const quint64 opId = it.key();
            m_inflight.erase(it);
            ++m_statCancelled;
            qCDebug(lcMediaTrace, "cancel %s opId=%llu",
                    qUtf8Printable(keyTag(cacheKey)),
                    static_cast<unsigned long long>(opId));
            if (m_client)
                m_client->cancelMediaFetch(opId);
            break;
        }
    }
    // No failure mark: a fresh Play must re-dispatch immediately.
    pump();
}

VideoPosterExtractor *MediaBridge::ensurePosterExtractor()
{
    if (!m_posterExtractor) {
        m_posterExtractor = new VideoPosterExtractor(this);
        connect(m_posterExtractor, &VideoPosterExtractor::posterReady,
                this, &MediaBridge::onPosterReady);
    }
    return m_posterExtractor;
}

void MediaBridge::startPosterExtraction(const QString &mediaKey,
                                        const QString &filePath)
{
    if (m_posterExtracting.contains(mediaKey))
        return;
    m_posterExtracting.insert(mediaKey);
    ensurePosterExtractor()->requestPoster(mediaKey, filePath);
}

void MediaBridge::warmMultimediaBackend()
{
    // The first QVideoSink in a process costs ~1 s (Qt Multimedia backend init
    // plus a hardware-decoder probe), and QML creates it on the GUI thread. Pay
    // it on the extractor's worker thread once a playable file exists. Not
    // reset by clear(): the initialization is process-global.
    if (m_multimediaWarmed)
        return;
    // Only a GUI application can play inline, and the guiless media suites must
    // never construct a decoder. Checked by class name to avoid a QtGui
    // dependency.
    const QCoreApplication *app = QCoreApplication::instance();
    if (!app || !app->inherits("QGuiApplication"))
        return;
    m_multimediaWarmed = true;
    ensurePosterExtractor()->warmUp();
}

void MediaBridge::onPosterReady(const QString &mediaKey,
                                const QByteArray &jpeg)
{
    // Session isolation: a queued completion can outlive the disconnect in
    // clear(), so the tracking set, not the connection, is the authority.
    if (!m_posterExtracting.remove(mediaKey))
        return;
    const QString posterKey = mediaCacheKey(mediaKey, 1);
    if (jpeg.isEmpty()) {
        // Permanent for this session; retry() (cover tap) clears it.
        m_failed.insert(posterKey, {QStringLiteral("rejected"),
                                    m_failureClock.elapsed()});
        Q_EMIT mediaFetchFailed(posterKey, QStringLiteral("rejected"));
        return;
    }
    insertCache(posterKey, jpeg);
    qCDebug(lcMediaTrace, "poster %s bytes=%lld",
            qUtf8Printable(keyTag(posterKey)),
            static_cast<long long>(jpeg.size()));
    // Header-only decode: the poster has the video's display shape (rotation
    // applied). The card only uses the ratio.
    {
        QBuffer buffer;
        buffer.setData(jpeg);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        if (size.isValid() && size.width() > 0 && size.height() > 0)
            Q_EMIT videoDimensionsLearned(mediaKey, size.width(),
                                          size.height());
    }
    Q_EMIT mediaCached(posterKey);
}

QString MediaBridge::playableExtensionFor(const QByteArray &bytes,
                                          const QString &mimetype)
{
    if (bytes.size() < 12)
        return {};
    const QString mime =
        mimetype.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    const auto u8 = [&bytes](qsizetype i) {
        return static_cast<unsigned char>(bytes.at(i));
    };
    // ISO BMFF (MP4/M4A/MOV): a size-prefixed "ftyp" box leads the file.
    if (bytes.mid(4, 4) == QByteArrayLiteral("ftyp"))
        return mime.startsWith(QLatin1String("audio/"))
            ? QStringLiteral("m4a") : QStringLiteral("mp4");
    // Matroska/WebM: EBML magic.
    if (u8(0) == 0x1A && u8(1) == 0x45 && u8(2) == 0xDF && u8(3) == 0xA3)
        return mime.contains(QLatin1String("webm"))
            ? QStringLiteral("webm") : QStringLiteral("mkv");
    // Ogg (Vorbis/Opus).
    if (bytes.startsWith("OggS"))
        return QStringLiteral("ogg");
    // WAV: RIFF….WAVE.
    if (bytes.startsWith("RIFF")
        && bytes.mid(8, 4) == QByteArrayLiteral("WAVE"))
        return QStringLiteral("wav");
    // FLAC.
    if (bytes.startsWith("fLaC"))
        return QStringLiteral("flac");
    // MP3: ID3 tag or a bare MPEG frame sync with non-zero layer bits. ADTS AAC
    // shares the sync but has layer 00.
    if (bytes.startsWith("ID3"))
        return QStringLiteral("mp3");
    if (u8(0) == 0xFF && (u8(1) & 0xE0) == 0xE0 && (u8(1) & 0x06) != 0
        && mime == QLatin1String("audio/mpeg"))
        return QStringLiteral("mp3");
    // Raw AAC in ADTS framing, only when the metadata says AAC.
    if (u8(0) == 0xFF && (u8(1) & 0xF6) == 0xF0
        && (mime == QLatin1String("audio/aac")
            || mime == QLatin1String("audio/aacp")))
        return QStringLiteral("aac");
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
    const QString path = writeAnimatedFile(cacheKey, bytes);
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
    // One canonical fetch per identity; the render size never reaches the cache
    // key. QML scales down.
    Q_UNUSED(size);
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
    const int edge = kAvatarCanonicalEdge;
    const QString cacheKey = mxcCacheKey(mxcUri, edge);
    const QString cached = cachedSource(cacheKey);
    if (!cached.isEmpty()) {
        ++m_statCacheHit;
        qCDebug(lcMediaTrace, "avatar %s edge=%d cache=hit",
                qUtf8Printable(keyTag(cacheKey)), edge);
        return cached;
    }
    if (failureBlocks(cacheKey)) {
        qCDebug(lcMediaTrace, "avatar %s edge=%d suppressed=failure-mark(%s)",
                qUtf8Printable(keyTag(cacheKey)), edge,
                qUtf8Printable(failureCategory(cacheKey)));
        return {};
    }
    if (!alreadyPending(cacheKey)) {
        ++m_statCacheMiss;
        Pending request;
        request.cacheKey = cacheKey;
        request.isMxc = true;
        request.mediaKey = mxcUri;
        request.kind = 2;
        request.size = edge;
        request.priority = 1; // interactive chrome
        qCDebug(lcMediaTrace, "avatar %s edge=%d cache=miss dispatching",
                qUtf8Printable(keyTag(cacheKey)), edge);
        dispatch(request);
    } else {
        promoteQueuedRequest(cacheKey, 1, 0);
        qCDebug(lcMediaTrace, "avatar %s edge=%d cache=miss already-pending",
                qUtf8Printable(keyTag(cacheKey)), edge);
    }
    return {};
}

QString MediaBridge::wideImageSource(const QString &mxcUri)
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
    // Full payload, not a square thumbnail. Edge 0 keeps it apart from the
    // avatar entry for the same mxc.
    const QString cacheKey = mxcCacheKey(mxcUri, 0);
    const QString cached = cachedSource(cacheKey);
    if (!cached.isEmpty()) {
        ++m_statCacheHit;
        return cached;
    }
    if (failureBlocks(cacheKey))
        return {};
    if (!alreadyPending(cacheKey)) {
        ++m_statCacheMiss;
        Pending request;
        request.cacheKey = cacheKey;
        request.isMxc = true;
        request.mediaKey = mxcUri;
        request.kind = 0;
        request.size = 0;
        request.priority = 1; // interactive chrome, like an avatar
        dispatch(request);
    } else {
        promoteQueuedRequest(cacheKey, 1, 0);
    }
    return {};
}

QImage MediaBridge::cachedAvatarImage(const QString &mxcUri) const
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")))
        return {};
    QByteArray bytes = cachedBytes(mxcCacheKey(mxcUri, kAvatarCanonicalEdge));
    if (bytes.isEmpty())
        return {};
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    // Same rule as MediaImageProvider (these are another user's avatar bytes
    // for a notification icon): a recognised raster format is pinned with
    // autodetection off, so the SVG handler is unreachable. An unrecognised
    // format (e.g. HEIC via qmacheif) may autodetect only after the markup/gzip
    // check has refused SVG and SVGZ.
    const lightning::imagefmt::RasterFormat *sniffed =
        lightning::imagefmt::sniffRaster(bytes);
    if (!sniffed && looksLikeMarkupOrCompressed(bytes))
        return {};
    QImageReader reader(&buffer);
    if (sniffed) {
        reader.setAutoDetectImageFormat(false);
        reader.setFormat(QByteArray(sniffed->qtFormat));
    }
    reader.setAutoTransform(true);
    reader.setAllocationLimit(64);
    const QSize natural = reader.size();
    // An unreadable header means the size ceiling cannot apply.
    if (!natural.isValid())
        return {};
    if (natural.width() > 4096 || natural.height() > 4096)
        return {};
    return reader.read();
}

QString MediaBridge::mxcImageSource(const QString &mxcUri, int edge)
{
    // Non-avatar mxc images (link-preview thumbnails): caller's edge, "mxcimg:"
    // prefix, main cache class, so they never evict avatars.
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
    edge = qBound(64, edge, 1024);
    const QString cacheKey =
        QStringLiteral("mxcimg:%1:%2").arg(edge).arg(mxcUri);
    const QString cached = cachedSource(cacheKey);
    if (!cached.isEmpty()) {
        ++m_statCacheHit;
        return cached;
    }
    if (failureBlocks(cacheKey))
        return {};
    if (!alreadyPending(cacheKey)) {
        ++m_statCacheMiss;
        Pending request;
        request.cacheKey = cacheKey;
        request.isMxc = true;
        request.mediaKey = mxcUri;
        request.kind = 2;
        request.size = edge;
        request.priority = 1; // interactive chrome
        dispatch(request);
    } else {
        promoteQueuedRequest(cacheKey, 1, 0);
    }
    return {};
}

int MediaBridge::heavyInflightCount() const
{
    int heavy = 0;
    for (const Pending &p : m_inflight) {
        if (p.priority >= 2)
            ++heavy;
    }
    return heavy;
}

void MediaBridge::dispatch(const Pending &request)
{
    // Heavy work never takes the last slots; playback and chrome need headroom.
    const bool heavyBlocked =
        request.priority >= 2 && heavyInflightCount() >= kMaxHeavyConcurrent;
    if (m_inflight.size() >= kMaxConcurrent || heavyBlocked) {
        Pending queued = request;
        queued.enqueuedAtMs = m_failureClock.elapsed();
        m_queue.enqueue(queued);
        qCDebug(lcMediaTrace, "queue %s prio=%d (inflight=%lld queued=%lld)",
                qUtf8Printable(keyTag(request.cacheKey)), request.priority,
                static_cast<long long>(m_inflight.size()),
                static_cast<long long>(m_queue.size()));
        // Sample the peak where the queue grows; by completion it has drained.
        noteMediaActivity();
        return;
    }
    Pending tracked = request;
    tracked.dispatchedAtMs = m_failureClock.elapsed();
    quint64 opId = 0;
    if (tracked.isMxc)
        opId = m_client->fetchMxcThumbnail(tracked.mediaKey, tracked.size,
                                           tracked.size);
    else
        opId = m_client->fetchMedia(tracked.mediaKey, tracked.kind,
                                    tracked.timeoutClass);
    if (opId == 0) {
        // The backend could not start the fetch (session restoring/switching,
        // or the item is not known yet). Transient: a permanent mark would
        // poison the key for the whole session.
        ++m_statFailed;
        qCWarning(lcMedia, "fetch %s unavailable (backend returned opId=0)",
                  qUtf8Printable(keyTag(tracked.cacheKey)));
        // Save/star failures use their own signals only: they share the "full:"
        // key with the ordinary fetch, and mediaFetchFailed would tell a
        // healthy consumer its fetch failed.
        if (tracked.saveRequest) {
            Q_EMIT saveFinished(false, tr("The file could not be downloaded."),
                                tracked.mediaKey);
        } else if (tracked.starRequest) {
            Q_EMIT mediaBytesForStar(tracked.mediaKey, false, {},
                                     QStringLiteral("unavailable"));
        } else {
            dropInterestSets(tracked.cacheKey);
            markFailed(tracked, QStringLiteral("unavailable"));
            Q_EMIT mediaFetchFailed(tracked.cacheKey,
                                    QStringLiteral("unavailable"));
        }
        return;
    }
    qCDebug(lcMediaTrace, "fetch %s opId=%llu inflight=%lld",
            qUtf8Printable(keyTag(tracked.cacheKey)),
            static_cast<unsigned long long>(opId),
            static_cast<long long>(m_inflight.size() + 1));
    m_inflight.insert(opId, tracked);
}

void MediaBridge::pump()
{
    while (!m_queue.isEmpty() && m_inflight.size() < kMaxConcurrent) {
        const qint64 now = m_failureClock.elapsed();
        const int heavy = heavyInflightCount();
        // Lowest priority value first, FIFO within a class; heavy entries are
        // skipped while heavy slots are full.
        int chosen = -1;
        int oldest = -1;
        for (int i = 0; i < m_queue.size(); ++i) {
            const Pending &p = m_queue.at(i);
            if (p.priority >= 2 && heavy >= kMaxHeavyConcurrent)
                continue;
            if (oldest < 0
                || p.enqueuedAtMs < m_queue.at(oldest).enqueuedAtMs)
                oldest = i;
            if (chosen < 0 || p.priority < m_queue.at(chosen).priority)
                chosen = i;
        }
        // Starvation bound: chrome can delay speculative work but never park
        // it.
        if (oldest >= 0
            && now - m_queue.at(oldest).enqueuedAtMs >= m_starvationMs)
            chosen = oldest;
        if (chosen < 0)
            break; // only heavy-blocked work remains queued
        dispatch(m_queue.takeAt(chosen));
    }
}

void MediaBridge::checkInflightTimeouts()
{
    // Expire failure marks on the same tick: QML does not repoll, so an avatar
    // marked failed has no other recovery path.
    sweepExpiredFailureMarks();
    if (m_inflight.isEmpty())
        return;
    const qint64 now = m_failureClock.elapsed();
    // Collect first: reclaiming mutates m_inflight and pump() may re-enter it.
    QList<quint64> expired;
    for (auto it = m_inflight.constBegin(); it != m_inflight.constEnd(); ++it) {
        const Pending &p = it.value();
        const qint64 limit = (p.saveRequest || p.starRequest) ? m_saveTimeoutMs
                           : p.timeoutClass == 1 ? m_playableTimeoutMs
                                                 : m_inflightTimeoutMs;
        if (p.dispatchedAtMs >= 0 && now - p.dispatchedAtMs >= limit)
            expired.append(it.key());
    }
    if (expired.isEmpty())
        return;
    for (const quint64 opId : std::as_const(expired)) {
        const auto it = m_inflight.find(opId);
        if (it == m_inflight.end())
            continue;
        const Pending request = it.value();
        m_inflight.erase(it);
        ++m_statTimedOut;
        qCWarning(lcMedia, "timeout %s reclaiming slot (inflight now %lld)",
                  qUtf8Printable(keyTag(request.cacheKey)),
                  static_cast<long long>(m_inflight.size()));
        if (request.saveRequest) {
            Q_EMIT saveFinished(false, tr("The download timed out."),
                                request.mediaKey);
        } else if (request.starRequest) {
            Q_EMIT mediaBytesForStar(request.mediaKey, false, {},
                                     QStringLiteral("timeout"));
        } else {
            // Transient: QML shows a fallback now and re-dispatches after the
            // interval. A late completion for this op is now a stale no-op.
            dropInterestSets(request.cacheKey);
            markFailed(request, QStringLiteral("timeout"));
            Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("timeout"));
        }
    }
    pump();
}

QVariantMap MediaBridge::healthSnapshot() const
{
    QVariantMap out;
    out.insert(QStringLiteral("inflight"),
               static_cast<qint64>(m_inflight.size()));
    out.insert(QStringLiteral("queued"),
               static_cast<qint64>(m_queue.size()));
    qint64 oldest = 0;
    const qint64 now = m_failureClock.elapsed();
    for (const Pending &p : m_inflight) {
        if (p.dispatchedAtMs >= 0)
            oldest = qMax(oldest, now - p.dispatchedAtMs);
    }
    out.insert(QStringLiteral("oldestInflightMs"), oldest);
    out.insert(QStringLiteral("completed"), m_statCompleted);
    out.insert(QStringLiteral("failed"), m_statFailed);
    out.insert(QStringLiteral("timedOut"), m_statTimedOut);
    out.insert(QStringLiteral("droppedStale"), m_statDroppedStale);
    out.insert(QStringLiteral("cancelled"), m_statCancelled);
    out.insert(QStringLiteral("cacheHits"), m_statCacheHit);
    out.insert(QStringLiteral("cacheMisses"), m_statCacheMiss);
    out.insert(QStringLiteral("contentHashComputed"), m_statContentHashComputed);
    out.insert(QStringLiteral("failureMarks"),
               static_cast<qint64>(m_failed.size()));
    // A count that never drains means a wedged worker-thread write.
    out.insert(QStringLiteral("pendingPlayableWrites"),
               static_cast<qint64>(m_playableWriting.size()));
    out.insert(QStringLiteral("cacheBytes"), cacheBytesUsed());
    return out;
}

void MediaBridge::onMediaReady(quint64 opId, const QString &mediaKey, int kind,
                               const QByteArray &bytes, const QString &mimetype,
                               const QString &filename)
{
    Q_UNUSED(mediaKey);
    Q_UNUSED(kind);
    Q_UNUSED(filename);
    const auto it = m_inflight.find(opId);
    if (it == m_inflight.end()) {
        // Stale (sign-out, watchdog) or foreign: never repopulate the next
        // account's cache.
        ++m_statDroppedStale;
        qCDebug(lcMedia, "ready opId=%llu suppressed=stale/foreign",
                static_cast<unsigned long long>(opId));
        return;
    }
    const Pending request = it.value();
    m_inflight.erase(it);
    pump();

    const qint64 elapsedMs = request.dispatchedAtMs > 0
        ? m_failureClock.elapsed() - request.dispatchedAtMs : -1;

    if (request.saveRequest) {
        writeSaveFile(request.saveDestination, bytes, request.mediaKey);
        return;
    }
    if (request.starRequest) {
        // Export, not cache, like Save As; GifStarredStore validates.
        Q_EMIT mediaBytesForStar(request.mediaKey, true, bytes, QString());
        return;
    }
    // Markup is refused on every class (CLAUDE.md §6): image rows fall back to
    // "full" when the sender omits a thumbnail, the viewer always asks for
    // "full", and banners are fetched as kind 0. Nothing legitimate here starts
    // with `<` or gzip. Save As and star export return above, so .svg/.tar.gz
    // downloads are unaffected.
    if (looksLikeMarkupOrCompressed(bytes)) {
        ++m_statFailed;
        qCWarning(lcMedia,
                  "ready %s rejected: payload sniffs as markup or compressed "
                  "(%lld bytes)",
                  qUtf8Printable(keyTag(request.cacheKey)),
                  static_cast<long long>(bytes.size()));
        markFailed(request, QStringLiteral("rejected"));
        Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("rejected"));
        return;
    }
    // Thumbnail-class results must be images. A server that cannot thumbnail
    // may return the original, labelled with the parent's mimetype, so the
    // bytes decide. Permanent: the server will answer the same way.
    if ((request.kind == 1 || request.kind == 2)
        && looksLikeAvContainer(bytes)) {
        ++m_statFailed;
        qCWarning(lcMedia,
                  "ready %s rejected: thumbnail payload sniffs as A/V "
                  "container (%lld bytes)",
                  qUtf8Printable(keyTag(request.cacheKey)),
                  static_cast<long long>(bytes.size()));
        markFailed(request, QStringLiteral("rejected"));
        Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("rejected"));
        return;
    }
    ++m_statCompleted;
    m_failed.remove(request.cacheKey);
    // Read, not consumed: materialization runs on the worker thread and a card
    // may still cancel meanwhile. Paths that start no write drop both entries;
    // the write completion drops them otherwise.
    const bool playableWanted = m_playableWanted.contains(request.cacheKey);
    const bool prefetchWanted = m_prefetchWanted.contains(request.cacheKey);
    // Remember the sniffed A/V size so metadata-less events can prefetch (and
    // get posters) in later sessions.
    if (request.kind == 0 && looksLikeAvContainer(bytes))
        Q_EMIT playableSizeLearned(request.mediaKey,
                                   static_cast<qint64>(bytes.size()));
    // Large playables live on disk; caching them would evict every image.
    if (!((playableWanted || prefetchWanted)
          && bytes.size() > kLargeCacheSkipBytes))
        insertCache(request.cacheKey, bytes);
    qCDebug(lcMediaTrace, "ready %s bytes=%lld mime=%s in=%lldms -> mediaCached",
            qUtf8Printable(keyTag(request.cacheKey)),
            static_cast<long long>(bytes.size()),
            qUtf8Printable(mimetype.section(QLatin1Char(';'), 0, 0)),
            static_cast<long long>(elapsedMs));
    ++m_burstCompleted;
    m_burstBytes += static_cast<qint64>(bytes.size());
    noteMediaActivity();
    if (m_animatedWanted.remove(request.cacheKey)) {
        const bool demanded = m_animatedDemanded.remove(request.cacheKey);
        if (!writeAnimatedFile(request.cacheKey, bytes).isEmpty())
            Q_EMIT animatedMediaReady(request.cacheKey);
        else if (demanded)
            Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("invalid_gif"));
        // A speculative asker gets silence; its still Image draws from
        // mediaCached().
    }
    if (playableWanted || prefetchWanted) {
        // playableMediaReady, warm-up and the poster hook fire from the write
        // completion. Only a refusal (unknown container, over the bound) is
        // terminal here.
        if (!beginPlayableWrite(request.cacheKey, request.mediaKey, bytes,
                                mimetype, playableWanted)) {
            m_playableWanted.remove(request.cacheKey);
            m_prefetchWanted.remove(request.cacheKey);
            m_posterWanted.remove(request.cacheKey);
            if (playableWanted)
                Q_EMIT mediaFetchFailed(request.cacheKey,
                                        QStringLiteral("rejected"));
        }
    }
    Q_EMIT mediaCached(request.cacheKey);
}

PlayableFileWriter *MediaBridge::ensurePlayableWriter()
{
    if (!m_playableWriter) {
        m_playableWriter = new PlayableFileWriter(this);
        connect(m_playableWriter, &PlayableFileWriter::writeFinished,
                this, &MediaBridge::onPlayableWriteFinished);
    }
    return m_playableWriter;
}

bool MediaBridge::beginPlayableWrite(const QString &cacheKey,
                                     const QString &mediaKey,
                                     const QByteArray &bytes,
                                     const QString &mimetype,
                                     bool notifyFailure)
{
    // Coalesce: a second caller never starts a second write. The completion
    // broadcasts playableMediaReady, and the failure obligation is the OR of
    // all callers', so a Play joining a prefetch still gets a terminal answer.
    const auto existing = m_playableWriting.find(cacheKey);
    if (existing != m_playableWriting.end()) {
        existing->notifyFailure = existing->notifyFailure || notifyFailure;
        return true;
    }
    // Refusals below happen before any byte reaches the worker, so no file
    // exists.
    if (bytes.isEmpty() || bytes.size() > m_playableMaxBytes
        || !m_animatedDir || !m_animatedDir->isValid())
        return false;
    const QString extension = playableExtensionFor(bytes, mimetype);
    if (extension.isEmpty())
        return false; // unknown container — fail closed, never materialize
    if (m_playableNameSalt.isEmpty())
        m_playableNameSalt = QUuid::createUuid().toString(QUuid::Id128);
    const QString name = QString::fromLatin1(QCryptographicHash::hash(
        (m_playableNameSalt + cacheKey).toUtf8(),
        QCryptographicHash::Sha256).toHex())
        + QLatin1Char('.') + extension;
    const QString path = m_animatedDir->filePath(name);
    const quint64 serial = ensurePlayableWriter()->write(
        cacheKey, path, bytes, m_sessionGeneration);
    if (serial == 0)
        return false; // writer refused; nothing was created
    PendingPlayableWrite pending;
    pending.serial = serial;
    pending.mediaKey = mediaKey;
    pending.path = path;
    pending.bytes = bytes.size();
    pending.generation = m_sessionGeneration;
    pending.notifyFailure = notifyFailure;
    m_playableWriting.insert(cacheKey, pending);
    return true;
}

void MediaBridge::onPlayableWriteFinished(quint64 serial,
                                          const QString &cacheKey,
                                          const QString &path,
                                          quint64 generation, bool ok)
{
    // Isolation by tracking hash and generation, so a previous account's
    // completion publishes nothing. The serial distinguishes a cancelled job
    // from its replacement.
    const auto it = m_playableWriting.find(cacheKey);
    if (it == m_playableWriting.end() || it->serial != serial
        || generation != m_sessionGeneration) {
        if (ok)
            QFile::remove(path); // orphan from a session that is over
        return;
    }
    const PendingPlayableWrite pending = it.value();
    m_playableWriting.erase(it);
    if (!ok) {
        // QSaveFile discarded its temp file. No failure mark: a disk error is
        // not a verdict on the payload, and the consumer may retry.
        m_posterWanted.remove(cacheKey);
        m_playableWanted.remove(cacheKey);
        m_prefetchWanted.remove(cacheKey);
        if (pending.notifyFailure)
            Q_EMIT mediaFetchFailed(cacheKey, QStringLiteral("rejected"));
        return;
    }
    // Only eviction unlinks remain on the GUI thread; keep them attributed.
    stalltrace::Scope stallScope("playable-write");
    registerPlayableFile(cacheKey, path, pending.bytes);
    m_playableWanted.remove(cacheKey);
    m_prefetchWanted.remove(cacheKey);
    Q_EMIT playableMediaReady(cacheKey);
    warmMultimediaBackend();
    if (m_posterWanted.remove(cacheKey))
        startPosterExtraction(pending.mediaKey, path);
}

void MediaBridge::registerPlayableFile(const QString &cacheKey,
                                       const QString &path, qint64 bytes)
{
    m_playableFiles.insert(cacheKey, path);
    m_playableSizes.insert(cacheKey, bytes);
    m_playableLru.removeOne(cacheKey);
    m_playableLru.prepend(cacheKey);
    qint64 total = 0;
    for (qint64 size : std::as_const(m_playableSizes))
        total += size;
    while ((total > m_playableMaxBytes
            || m_playableFiles.size() > m_playableMaxEntries)
           && m_playableLru.size() > 1) {
        // Least-recent unpinned victim. If everything is pinned the cap is
        // exceeded, bounded by the number of live players.
        int victimIndex = -1;
        for (int i = m_playableLru.size() - 1; i >= 1; --i) {
            if (!m_pinnedPlayables.contains(m_playableLru.at(i))) {
                victimIndex = i;
                break;
            }
        }
        if (victimIndex < 1) {
            qCInfo(lcMedia,
                   "playable cache over budget with every entry pinned "
                   "(%lld files, %lld bytes)",
                   static_cast<long long>(m_playableFiles.size()), total);
            break;
        }
        const QString victim = m_playableLru.takeAt(victimIndex);
        total -= m_playableSizes.take(victim);
        QFile::remove(m_playableFiles.take(victim));
    }
}

void MediaBridge::pinPlayable(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    // Refcounted: two cards can pin the same file.
    ++m_pinnedPlayables[mediaCacheKey(mediaKey, 0)];
}

void MediaBridge::unpinPlayable(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const auto it = m_pinnedPlayables.find(cacheKey);
    if (it == m_pinnedPlayables.end())
        return; // unbalanced unpin — floored, never negative
    if (--it.value() <= 0)
        m_pinnedPlayables.erase(it);
}

void MediaBridge::dropQueuedSpeculative()
{
    if (m_queue.isEmpty())
        return;
    int dropped = 0;
    for (int i = m_queue.size() - 1; i >= 0; --i) {
        const Pending &p = m_queue.at(i);
        // Keep entries a playableSource() caller coalesced onto ("full:" is
        // shared by the animated and playable paths); dropping them would
        // strand it.
        if (p.priority == 3 && !p.saveRequest && !p.starRequest
            && !m_playableWanted.contains(p.cacheKey)) {
            m_animatedWanted.remove(p.cacheKey);
            m_animatedDemanded.remove(p.cacheKey);
            // Playable prefetches and poster hooks are equally stale after a
            // room switch.
            m_prefetchWanted.remove(p.cacheKey);
            m_posterWanted.remove(p.cacheKey);
            m_queue.removeAt(i);
            ++dropped;
        }
    }
    if (dropped > 0) {
        qCDebug(lcMedia, "dropped %d queued speculative fetches (room left)",
                dropped);
    }
}

void MediaBridge::promoteQueuedRequest(const QString &cacheKey, int priority,
                                       int timeoutClass)
{
    // alreadyPending() suppresses duplicate requests, so an interactive caller
    // landing on a speculative entry would inherit its class. Raise the entry's
    // priority in place (keeping its starvation age) and widen its timeout
    // class.
    for (int i = 0; i < m_queue.size(); ++i) {
        Pending &p = m_queue[i];
        if (p.cacheKey != cacheKey || p.saveRequest || p.starRequest)
            continue;
        if (priority < p.priority)
            p.priority = priority;
        if (timeoutClass > p.timeoutClass)
            p.timeoutClass = timeoutClass;
        return;
    }
}

QString MediaBridge::animatedExtensionFor(const QByteArray &bytes)
{
    if (bytes.size() < 12)
        return {};
    if (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a"))
        return QStringLiteral("gif");
    // Only animated WebP: a still one must take the Image path. An animation
    // carries a "VP8X" chunk with the animation bit (0x02) set.
    if (bytes.size() >= 21 && bytes.startsWith("RIFF")
        && std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0
        && std::memcmp(bytes.constData() + 12, "VP8X", 4) == 0
        && (static_cast<unsigned char>(bytes.at(20)) & 0x02u) != 0)
        return QStringLiteral("webp");
    // No APNG: Qt's PNG handler does not animate, so an AnimatedImage would
    // show one frame while suppressing the still Image that already does.
    return {};
}

QString MediaBridge::writeAnimatedFile(const QString &cacheKey,
                                       const QByteArray &bytes)
{
    constexpr qsizetype maxAnimatedBytes = 20 * 1024 * 1024;
    const QString extension = animatedExtensionFor(bytes);
    if (extension.isEmpty() || bytes.size() > maxAnimatedBytes
        || !m_animatedDir || !m_animatedDir->isValid())
        return {};
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(cacheKey.toUtf8(), QCryptographicHash::Sha256).toHex())
        + QLatin1Char('.') + extension;
    const QString path = m_animatedDir->filePath(name);
    QSaveFile file(path);
    // 0600 before writing: these are decrypted payloads and must not rely on
    // the 0700 directory alone.
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit())
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

void MediaBridge::dropInterestSets(const QString &cacheKey)
{
    // Terminal outcome: consumers were told, and a retry re-expresses interest.
    m_playableWanted.remove(cacheKey);
    m_animatedWanted.remove(cacheKey);
    m_animatedDemanded.remove(cacheKey);
    m_prefetchWanted.remove(cacheKey);
    m_posterWanted.remove(cacheKey);
}

void MediaBridge::markFailed(const Pending &request, const QString &category)
{
    if (request.saveRequest || request.starRequest)
        return; // Save/star report through their own signal, not source state.
    if (m_failed.size() >= kMaxFailureMarks)
        m_failed.clear(); // defensive bound
    m_failed.insert(request.cacheKey,
                    {category, m_failureClock.elapsed()});
}

void MediaBridge::onMediaFailed(quint64 opId, const QString &mediaKey, int kind,
                                const QString &category)
{
    Q_UNUSED(mediaKey);
    Q_UNUSED(kind);
    const auto it = m_inflight.find(opId);
    if (it == m_inflight.end()) {
        ++m_statDroppedStale;
        qCDebug(lcMedia, "failed opId=%llu suppressed=stale/foreign",
                static_cast<unsigned long long>(opId));
        return;
    }
    const Pending request = it.value();
    m_inflight.erase(it);
    pump();
    if (request.saveRequest) {
        Q_EMIT saveFinished(false, tr("The file could not be downloaded."),
                            request.mediaKey);
        return;
    }
    if (request.starRequest) {
        Q_EMIT mediaBytesForStar(request.mediaKey, false, {}, category);
        return;
    }
    ++m_statFailed;
    ++m_burstFailed;
    noteMediaActivity();
    // Failures keep their own line in the default category: rare and
    // actionable.
    qCWarning(lcMedia, "failed %s category=%s",
              qUtf8Printable(keyTag(request.cacheKey)),
              qUtf8Printable(category));
    dropInterestSets(request.cacheKey);
    markFailed(request, category);
    Q_EMIT mediaFetchFailed(request.cacheKey, category);
}

QString MediaBridge::sanitizedFileName(const QString &name)
{
    // The name is sender-chosen, so reduce it to a single harmless leaf. Split
    // on both separators (QFileInfo only knows the native one, so `..\..\x` is
    // one leaf on Unix) and keep the last component.
    QString out = QFileInfo(name).fileName();
    const QStringList parts = out.split(QRegularExpression(
        QStringLiteral("[\\\\/]")), Qt::SkipEmptyParts);
    if (!parts.isEmpty())
        out = parts.last();
    // Control characters, including NUL.
    for (QChar &c : out) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QLatin1Char('_');
    }
    // Refuse `..` only; a single leading dot is legitimate in a user-typed
    // name.
    while (out == QLatin1String("..") || out.startsWith(QLatin1String("../"))
           || out.startsWith(QLatin1String("..\\")))
        out.remove(0, 2);
    out = out.trimmed();
    // Windows reserved device names, whatever the suffix.
    static const QStringList reserved = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"),
        QStringLiteral("nul"), QStringLiteral("com1"), QStringLiteral("com2"),
        QStringLiteral("com3"), QStringLiteral("com4"), QStringLiteral("com5"),
        QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"),
        QStringLiteral("lpt3"), QStringLiteral("lpt4"), QStringLiteral("lpt5"),
        QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9"), QStringLiteral("conin$"),
        QStringLiteral("conout$"),
    };
    if (reserved.contains(out.section(QLatin1Char('.'), 0, 0).toLower()))
        out.prepend(QStringLiteral("file-"));
    // Bounded length (filesystems cap components at 255 bytes), keeping the
    // suffix.
    if (out.size() > 120) {
        const int dot = out.lastIndexOf(QLatin1Char('.'));
        const QString suffix =
            (dot > 0 && out.size() - dot <= 12) ? out.mid(dot) : QString();
        out = out.left(120 - suffix.size()) + suffix;
    }
    if (out.isEmpty() || out == QLatin1String(".") || out == QLatin1String(".."))
        out = QStringLiteral("download");
    return out;
}

QString MediaBridge::suggestedSaveName(const QString &rawName) const
{
    // Never seed a save dialog's path with a sender-chosen string. Empty means
    // no suggestion.
    if (rawName.trimmed().isEmpty())
        return {};
    const QString leaf = sanitizedFileName(rawName);
    return leaf == QLatin1String("download") ? QString() : leaf;
}

void MediaBridge::saveAs(const QString &mediaKey, const QUrl &destination)
{
    if (mediaKey.isEmpty() || !supported() || !destination.isLocalFile()) {
        Q_EMIT saveFinished(false, tr("No destination selected."), mediaKey);
        return;
    }
    const QByteArray cached = cachedBytes(mediaCacheKey(mediaKey, 0));
    if (!cached.isEmpty()) {
        writeSaveFile(destination, cached, mediaKey);
        return;
    }
    Pending request;
    request.cacheKey = mediaCacheKey(mediaKey, 0);
    request.mediaKey = mediaKey;
    request.kind = 0;
    request.saveRequest = true;
    request.saveDestination = destination;
    request.timeoutClass = 2; // save class (270s Rust / 5min watchdog)
    request.priority = 0;     // explicit user intent
    dispatch(request);
}

void MediaBridge::fetchFullForStar(const QString &mediaKey)
{
    if (mediaKey.isEmpty() || !supported()) {
        Q_EMIT mediaBytesForStar(mediaKey, false, {},
                                 QStringLiteral("unavailable"));
        return;
    }
    // Usually cached already: an inline GIF fetched these exact bytes.
    const QByteArray cached = cachedBytes(mediaCacheKey(mediaKey, 0));
    if (!cached.isEmpty()) {
        Q_EMIT mediaBytesForStar(mediaKey, true, cached, QString());
        return;
    }
    // Drop a duplicate star request (double tap); the one in flight answers.
    for (const Pending &p : m_inflight) {
        if (p.starRequest && p.mediaKey == mediaKey)
            return;
    }
    for (const Pending &p : m_queue) {
        if (p.starRequest && p.mediaKey == mediaKey)
            return;
    }
    Pending request;
    request.cacheKey = mediaCacheKey(mediaKey, 0);
    request.mediaKey = mediaKey;
    request.kind = 0;
    request.starRequest = true;
    request.timeoutClass = 2; // save class — an explicit user export
    request.priority = 0;     // explicit user intent
    dispatch(request);
}

QString MediaBridge::cachedFullContentHash(const QString &mediaKey) const
{
    if (mediaKey.isEmpty())
        return {};
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    QByteArray bytes; // implicitly shared: the copy itself is O(1)
    quint32 rev = 0;
    {
        QMutexLocker lock(&m_cacheMutex);
        const auto it = m_cache.constFind(cacheKey);
        if (it == m_cache.constEnd())
            return {};
        // No touch(): a read-only predicate must not reorder LRU eviction.
        rev = m_revision.value(cacheKey);
        const auto memoized = m_contentHashCache.constFind(cacheKey);
        if (memoized != m_contentHashCache.constEnd() && memoized->revision == rev)
            return memoized->hex;
        bytes = it.value();
    }

    // Hash outside the lock: m_cacheMutex is shared with the image-reader
    // thread, and hashing several MiB would stall it. The COW copy keeps the
    // bytes valid.
    const QString hex = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    QMutexLocker lock(&m_cacheMutex);
    // Memoize only if the same buffer is still cached. Compare data pointers,
    // not revisions: clear() resets m_revision, so a revision can repeat (ABA).
    // Currently all callers run on the GUI thread, but a worker-thread hash
    // would make this reachable.
    const auto after = m_cache.constFind(cacheKey);
    if (after != m_cache.constEnd()
        && after->constData() == bytes.constData()
        && after->size() == bytes.size()) {
        ++m_statContentHashComputed;
        m_contentHashCache.insert(cacheKey, { hex, rev });
    }
    return hex;
}

void MediaBridge::writeSaveFile(const QUrl &destination, const QByteArray &bytes,
                                const QString &mediaKey)
{
    const QFileInfo chosen(destination.toLocalFile());
    // Canonicalize the parent directory on its own, then join the sanitized
    // leaf, so `../` in the name cannot move the target out of the chosen
    // directory.
    const QDir parent(QFileInfo(chosen.absolutePath()).canonicalFilePath());
    if (!parent.exists()) {
        Q_EMIT saveFinished(false, tr("The destination is not writable."),
                            mediaKey);
        return;
    }
    const QString target = parent.filePath(sanitizedFileName(chosen.fileName()));
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        Q_EMIT saveFinished(false, tr("The destination is not writable."),
                            mediaKey);
        return;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        Q_EMIT saveFinished(false, tr("Writing the file failed."), mediaKey);
        return;
    }
    Q_EMIT saveFinished(true, tr("Saved."), mediaKey);
}

void MediaBridge::clear()
{
    {
        QMutexLocker lock(&m_cacheMutex);
        qCDebug(lcMedia, "clear: dropping %lld cached entries, %lld inflight",
                static_cast<long long>(m_cache.size()),
                static_cast<long long>(m_inflight.size()));
        m_cache.clear();
        m_lru.clear();
        m_avatarLru.clear();
        m_cacheBytesMain = 0;
        m_cacheBytesAvatar = 0;
        m_artworkCache.clear();
        m_artworkLru.clear();
        m_artworkBytes = 0;
        // Revisions restart with the session.
        m_revision.clear();
        // Memoized digests refer to bytes that are gone.
        m_contentHashCache.clear();
    }
    m_inflight.clear();
    m_queue.clear();
    m_failed.clear();
    m_animatedFiles.clear();
    m_animatedSizes.clear();
    m_animatedLru.clear();
    m_animatedWanted.clear();
    m_animatedDemanded.clear();
    m_playableFiles.clear();
    m_playableSizes.clear();
    m_playableLru.clear();
    m_playableWanted.clear();
    m_prefetchWanted.clear();
    m_posterWanted.clear();
    m_posterExtracting.clear();
    // An extraction still decoding must not deliver a poster from the previous
    // account. Clearing m_posterExtracting above makes late completions inert;
    // disconnecting and dropping the extractor tears down its decoder.
    if (m_posterExtractor) {
        disconnect(m_posterExtractor, nullptr, this, nullptr);
        // Without waiting for the worker thread: the join can take ~1 s of Qt
        // Multimedia init, and this runs on the GUI thread during an account
        // switch.
        m_posterExtractor->retireWithoutWaiting();
        m_posterExtractor->deleteLater();
        m_posterExtractor = nullptr;
    }
    m_pinnedPlayables.clear(); // the files the pins protected are gone too
    // Session isolation for the write path: clearing the tracking hash makes
    // late completions inert and the generation bump is a second guard.
    // cancelAll() only requests cancellation, checked between chunks, so a
    // final chunk may still land; the m_animatedDir reset below is what removes
    // it. The writer itself is kept: it holds only a thread, no account data.
    m_playableWriting.clear();
    ++m_sessionGeneration;
    if (m_playableWriter)
        m_playableWriter->cancelAll();
    m_playableNameSalt.clear(); // next session gets fresh unguessable names
    // Release the live mark before the directory is removed.
    if (m_animatedDir)
        lightning::portable::releaseScratchDir(m_animatedDir->path());
    m_animatedDir.reset(); // recursively removes decrypted temporary files
    m_animatedDir = std::make_unique<QTemporaryDir>(
        lightning::portable::mediaScratchRoot()
        + QStringLiteral("/lightning-animated-XXXXXX"));
    // The replacement is a new directory and needs its own mark.
    lightning::portable::holdScratchDirLive(m_animatedDir->path());
}

void MediaBridge::onLoggedOut()
{
    // Decrypted media must not outlive the session, and no stale completion may
    // repopulate the cache.
    clear();
}
