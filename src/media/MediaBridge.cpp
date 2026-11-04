#include "media/MediaBridge.h"
#include "app/SaveNaming.h"

#include "media/AnimatedImageSniff.h"
#include "media/ImageFormatSupport.h"

#include "storage/PortableMode.h"
#include "storage/ScratchFile.h"

#include "app/GuiStallTracer.h"

#include "matrix/MatrixClient.h"
#include "matrix/PresentableName.h"
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
    if (mimetype == QLatin1String("image/jxl")) {
        return bytes.startsWith(
                   QByteArrayLiteral("\x00\x00\x00\x0CJXL \r\n\x87\n"))
            || bytes.startsWith(QByteArrayLiteral("\xff\x0a"));
    }
    return false;
}
}

MediaBridge::MediaBridge(QObject *parent)
    : QObject(parent)
    , m_playableMaxEntries(kPlayableCacheEntries)
    , m_playableMaxBytes(kPlayableCacheBytes)
{
    m_failureClock.start();
    m_handoutClock.start();
    connect(this, &MediaBridge::saveFinished, this,
            [this](bool, const QString &, const QString &mediaKey) {
                removeActiveSave(mediaKey);
            });
    m_animatedDir = std::make_unique<QTemporaryDir>(
        lightning::portable::mediaScratchRoot()
        + QStringLiteral("/lightning-animated-XXXXXX"));
    lightning::portable::holdScratchDirLive(m_animatedDir->path());
    m_watchdog.setInterval(5000);
    m_watchdog.setTimerType(Qt::CoarseTimer);
    connect(&m_watchdog, &QTimer::timeout,
            this, &MediaBridge::checkInflightTimeouts);
    m_watchdog.start();

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
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes.at(0)) == 0xEF
        && static_cast<unsigned char>(bytes.at(1)) == 0xBB
        && static_cast<unsigned char>(bytes.at(2)) == 0xBF)
        i = 3;
    const auto isXmlSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (i < bytes.size() && isXmlSpace(bytes.at(i)))
        ++i;
    if (i < bytes.size() && bytes.at(i) == '<')
        return true;
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
        const QByteArray brand = bytes.mid(8, 4);
        if (brand == QByteArrayLiteral("avif")
            || brand == QByteArrayLiteral("avis")
            || brand == QByteArrayLiteral("heic")
            || brand == QByteArrayLiteral("heix")
            || brand == QByteArrayLiteral("mif1"))
            return false;
        return true;
    }
    if (u8(0) == 0x1A && u8(1) == 0x45 && u8(2) == 0xDF && u8(3) == 0xA3)
        return true;
    if (bytes.startsWith("OggS"))
        return true;
    if (bytes.startsWith("RIFF")) {
        const QByteArray form = bytes.mid(8, 4);
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
    m_awaitingDecode.insert(cacheKey, m_handoutClock.elapsed());
    return QStringLiteral("image://lightning-media/")
        + QString::fromUtf8(QUrl::toPercentEncoding(cacheKey))
        + QStringLiteral("?r=")
        + QString::number(m_revision.value(cacheKey, 1));
}

QSize MediaBridge::cachedImageSize(const QString &source) const
{
    static const QString prefix = QStringLiteral("image://lightning-media/");
    if (!source.startsWith(prefix))
        return {};
    QString encoded = source.mid(prefix.size());
    const qsizetype query = encoded.indexOf(QLatin1Char('?'));
    if (query >= 0)
        encoded.truncate(query);
    const QString cacheKey = QUrl::fromPercentEncoding(encoded.toUtf8());
    QByteArray bytes;
    {
        QMutexLocker lock(&m_cacheMutex);
        const auto it = m_cache.constFind(cacheKey);
        if (it == m_cache.constEnd())
            return {};
        bytes = it.value();
    }
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    return QImageReader(&buffer).size();
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

QByteArray MediaBridge::bytesForDecode(const QString &cacheKey)
{
    QMutexLocker lock(&m_cacheMutex);
    const auto it = m_cache.constFind(cacheKey);
    if (it == m_cache.constEnd()) {
        m_windowDecodeMisses.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const QByteArray bytes = it.value();
    if (m_awaitingDecode.remove(cacheKey) > 0)
        trimClassLocked(isAvatarClassKey(cacheKey), cacheKey);
    return bytes;
}

qint64 MediaBridge::cacheBytesUsed() const
{
    QMutexLocker lock(&m_cacheMutex);
    return m_cacheBytesMain + m_cacheBytesAvatar;
}

void MediaBridge::touch(const QString &cacheKey) const
{
    QList<QString> &lru = isAvatarClassKey(cacheKey) ? m_avatarLru : m_lru;
    lru.removeOne(cacheKey);
    lru.prepend(cacheKey);
}

void MediaBridge::insertCache(const QString &cacheKey, const QByteArray &bytes)
{
    QMutexLocker lock(&m_cacheMutex);
    const bool avatarClass = isAvatarClassKey(cacheKey);
    qint64 &classTotal = avatarClass ? m_cacheBytesAvatar : m_cacheBytesMain;
    if (const auto existing = m_cache.constFind(cacheKey);
        existing != m_cache.constEnd())
        classTotal -= existing.value().size();
    m_cache.insert(cacheKey, bytes);
    classTotal += bytes.size();
    ++m_revision[cacheKey];
    touch(cacheKey);
    trimClassLocked(avatarClass, cacheKey);
}

void MediaBridge::trimClassLocked(bool avatarClass, const QString &keep)
{
    qint64 &classTotal = avatarClass ? m_cacheBytesAvatar : m_cacheBytesMain;
    QList<QString> &lru = avatarClass ? m_avatarLru : m_lru;
    const qint64 limit = avatarClass ? m_avatarCacheLimit : m_cacheLimit;
    const qint64 now = m_handoutClock.elapsed();
    // The second pass ignores the decode shield: shielded bytes may hold at
    // most one more cache's worth.
    for (int pass = 0; pass < 2; ++pass) {
        const bool shielded = pass == 0;
        const qint64 ceiling = shielded ? limit : limit * 2;
        for (qsizetype i = lru.size() - 1; i >= 0 && classTotal > ceiling; --i) {
            const QString victim = lru.at(i);
            if (victim == keep)
                continue;
            const auto mark = m_awaitingDecode.constFind(victim);
            if (mark != m_awaitingDecode.constEnd()) {
                if (shielded && now - mark.value() < m_decodeGraceMs)
                    continue;
                m_awaitingDecode.erase(mark);
            }
            classTotal -= m_cache.value(victim).size();
            m_cache.remove(victim);
            m_contentHashCache.remove(victim);
            lru.removeAt(i);
            ++m_windowEvictions;
        }
    }
}

bool MediaBridge::alreadyPending(const QString &cacheKey) const
{
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

qint64 MediaBridge::markExpiryMs(const QString &category,
                                 qint64 retryAfterMs) const
{
    if (category == QLatin1String("server_unreachable") && retryAfterMs > 0)
        return retryAfterMs;
    return m_failureRetryMs;
}

int MediaBridge::failureRetrySeconds(const QString &cacheKey) const
{
    const auto it = m_failed.constFind(cacheKey);
    if (it == m_failed.constEnd()
        || it->category != QLatin1String("server_unreachable"))
        return 0;
    const qint64 left = it->markedAtMs
        + markExpiryMs(it->category, it->retryAfterMs)
        - m_failureClock.elapsed();
    return left <= 0 ? 0 : static_cast<int>((left + 999) / 1000);
}

bool MediaBridge::isPermanentCategory(const QString &category)
{
    // Permanent for the session: backend-reported validation failures, and a
    // 404 ("not_found"), where the server has answered that the file does not
    // exist; a tester's log re-requested one such avatar 29 times in a session.
    // Network, timeout, "server_error" (5xx) and the local "unavailable"
    // dispatch failure (opId 0 during restore/switch) are transient.
    return category == QLatin1String("rejected")
        || category == QLatin1String("invalid_gif")
        || category == QLatin1String("not_found");
}

bool MediaBridge::failureBlocks(const QString &cacheKey)
{
    const auto it = m_failed.find(cacheKey);
    if (it == m_failed.end())
        return false;
    if (isPermanentCategory(it->category))
        return true;
    if (m_failureClock.elapsed() - it->markedAtMs
        >= markExpiryMs(it->category, it->retryAfterMs)) {
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
    QStringList retryable;
    for (auto it = m_failed.begin(); it != m_failed.end();) {
        if (!isPermanentCategory(it->category)
            && now - it->markedAtMs
                >= markExpiryMs(it->category, it->retryAfterMs)) {
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
    if (kindValue != 0 && isLinkMediaKey(mediaKey))
        return {};
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
    if (failureBlocks(cacheKey))
        return {};
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        const QString written = writeAnimatedFile(cacheKey, cached);
        if (written.isEmpty()) {
            if (!speculative)
                Q_EMIT mediaFetchFailed(cacheKey, QStringLiteral("invalid_gif"));
            return {};
        }
        return QUrl::fromLocalFile(written).toString();
    }
    m_animatedWanted.insert(cacheKey);
    if (!speculative)
        m_animatedDemanded.insert(cacheKey);
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = 0;
        request.priority = 3;
        dispatch(request);
    }
    return {};
}

QString MediaBridge::mxcAnimatedSource(const QString &mxcUri)
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
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
        const QString written = writeAnimatedFile(cacheKey, cached);
        return written.isEmpty() ? QString{} : QUrl::fromLocalFile(written).toString();
    }
    m_animatedWanted.insert(cacheKey);
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.isMxc = true;
        request.mediaKey = mxcUri;
        request.kind = 2;
        request.size = 0;
        request.priority = 3;
        dispatch(request);
    }
    return {};
}

QString MediaBridge::motionFileUrl(const QString &cacheKey, qint64 maxPixels)
{
    if (m_motionVerdict.value(cacheKey, 0) > maxPixels)
        return {};
    const QString path = m_animatedFiles.value(cacheKey);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return {};
    m_animatedLru.removeOne(cacheKey);
    m_animatedLru.prepend(cacheKey);
    return QUrl::fromLocalFile(path).toString();
}

QString MediaBridge::writeMotionFile(const QString &cacheKey,
                                     const QByteArray &bytes, qint64 maxPixels)
{
    namespace sniff = lightning::animsniff;
    qint64 pixels = 0;
    if (bytes.size() <= kMotionMaxBytes && sniff::isAnimation(bytes)) {
        const QSize canvas = sniff::canvasSize(bytes);
        if (canvas.isValid() && canvas.width() > 0 && canvas.height() > 0)
            pixels = qint64(canvas.width()) * canvas.height();
    }
    if (m_motionVerdict.size() >= kMaxMotionVerdicts)
        m_motionVerdict.clear();
    m_motionVerdict.insert(cacheKey, pixels);
    if (pixels <= 0 || pixels > maxPixels)
        return {};
    return writeAnimatedFile(cacheKey, bytes);
}

QString MediaBridge::avatarAnimationSource(const QString &mxcUri,
                                           bool explicitIntent)
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
    constexpr qint64 maxPixels = lightning::animsniff::kAvatarMaxPixels;
    const QString cacheKey = QStringLiteral("motion:") + mxcUri;
    const QString url = motionFileUrl(cacheKey, maxPixels);
    if (!url.isEmpty())
        return url;
    const auto verdict = m_motionVerdict.constFind(cacheKey);
    if (verdict != m_motionVerdict.constEnd()
        && (verdict.value() <= 0 || verdict.value() > maxPixels))
        return {};
    if (failureBlocks(cacheKey))
        return {};
    const QByteArray thumb =
        cachedBytes(mxcCacheKey(mxcUri, kAvatarCanonicalEdge));
    if (lightning::animsniff::isAnimation(thumb)) {
        const QString written = writeMotionFile(cacheKey, thumb, maxPixels);
        return written.isEmpty() ? QString{}
                                 : QUrl::fromLocalFile(written).toString();
    }
    if (!explicitIntent) {
        const bool jpegThumb = thumb.size() >= 3
            && static_cast<unsigned char>(thumb.at(0)) == 0xff
            && static_cast<unsigned char>(thumb.at(1)) == 0xd8
            && static_cast<unsigned char>(thumb.at(2)) == 0xff;
        if (thumb.isEmpty() || jpegThumb)
            return {};
    }
    const int priority = explicitIntent ? 2 : 3;
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.isMxc = true;
        request.mediaKey = mxcUri;
        request.kind = 2;
        request.size = 0;
        request.priority = priority;
        qCDebug(lcMediaTrace, "motion %s probe explicit=%d",
                qUtf8Printable(keyTag(cacheKey)), explicitIntent ? 1 : 0);
        dispatch(request);
    } else if (explicitIntent) {
        promoteQueuedRequest(cacheKey, priority, 0);
    }
    return {};
}

QString MediaBridge::wideAnimationSource(const QString &mxcUri)
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
    constexpr qint64 maxPixels = lightning::animsniff::kBannerMaxPixels;
    const QString cacheKey = QStringLiteral("motion:") + mxcUri;
    const QString url = motionFileUrl(cacheKey, maxPixels);
    if (!url.isEmpty())
        return url;
    const auto verdict = m_motionVerdict.constFind(cacheKey);
    if (verdict != m_motionVerdict.constEnd()
        && (verdict.value() <= 0 || verdict.value() > maxPixels))
        return {};
    const QByteArray bytes = cachedBytes(mxcCacheKey(mxcUri, 0));
    if (bytes.isEmpty())
        return {};
    const QString written = writeMotionFile(cacheKey, bytes, maxPixels);
    return written.isEmpty() ? QString{}
                             : QUrl::fromLocalFile(written).toString();
}

QString MediaBridge::stagedAnimationSource(const QString &stagedUrl)
{
    static const QString prefix = QStringLiteral("image://lightning-staged/");
    if (!m_stagedBytes || !stagedUrl.startsWith(prefix))
        return {};
    const QString token = stagedUrl.mid(prefix.size());
    constexpr qint64 maxPixels = lightning::animsniff::kBannerMaxPixels;
    QString cacheKey = m_stagedKeys.value(token);
    if (!cacheKey.isEmpty()) {
        const QString known = motionFileUrl(cacheKey, maxPixels);
        if (!known.isEmpty())
            return known;
    }
    const QByteArray bytes = m_stagedBytes(token);
    if (bytes.isEmpty() || bytes.size() > kMotionMaxBytes)
        return {};
    if (cacheKey.isEmpty()) {
        cacheKey = QStringLiteral("motion:staged:") + QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        if (m_stagedKeys.size() >= 64)
            m_stagedKeys.clear();
        m_stagedKeys.insert(token, cacheKey);
    }
    const QString url = motionFileUrl(cacheKey, maxPixels);
    if (!url.isEmpty())
        return url;
    const QString written = writeMotionFile(cacheKey, bytes, maxPixels);
    return written.isEmpty() ? QString{}
                             : QUrl::fromLocalFile(written).toString();
}

QString MediaBridge::backdropAnimationSource(const QString &mxcUri)
{
    if (!mxcUri.startsWith(QLatin1String("mxc://")) || !supported())
        return {};
    const QByteArray bytes = cachedBytes(mxcCacheKey(mxcUri, 0));
    if (bytes.isEmpty())
        return {};
    if (bytes.size() <= kMotionMaxBytes
        && lightning::animsniff::timingTooBusy(
               lightning::animsniff::animationTiming(bytes)))
        return {};
    return wideAnimationSource(mxcUri);
}

int MediaBridge::passiveMotionSlots() const
{
    int passive = 0;
    for (const MotionHolder &holder : m_motionOwners) {
        if (!holder.intent)
            ++passive;
    }
    return passive;
}

bool MediaBridge::claimMotionSlot(QObject *owner, bool intent)
{
    if (!owner)
        return false;
    const auto held = m_motionOwners.find(owner);
    if (held != m_motionOwners.end()) {
        if (intent || !held->intent)
            return true;
        if (passiveMotionSlots() < kMaxMotionSlots) {
            held->intent = false;
            return true;
        }
        releaseMotionSlot(owner);
        return false;
    }
    const int total = static_cast<int>(m_motionOwners.size());
    if (total >= kMaxMotionSlots + kMotionIntentReserve)
        return false;
    if (!intent && passiveMotionSlots() >= kMaxMotionSlots)
        return false;
    MotionHolder holder;
    holder.intent = intent;
    holder.destroyed = connect(owner, &QObject::destroyed, this,
                               [this, owner] { releaseMotionSlot(owner); });
    m_motionOwners.insert(owner, holder);
    return true;
}

void MediaBridge::releaseMotionSlot(QObject *owner)
{
    const auto it = m_motionOwners.find(owner);
    if (it == m_motionOwners.end())
        return;
    disconnect(it->destroyed);
    m_motionOwners.erase(it);
    if (m_motionFreedPending)
        return;
    m_motionFreedPending = true;
    QMetaObject::invokeMethod(this, [this] {
        m_motionFreedPending = false;
        Q_EMIT motionSlotFreed();
    }, Qt::QueuedConnection);
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
    ++m_playableWanted[cacheKey];
    if (failureBlocks(cacheKey)) {
        if (--m_playableWanted[cacheKey] <= 0)
            m_playableWanted.remove(cacheKey);
        return {};
    }
    const auto writing = m_playableWriting.find(cacheKey);
    if (writing != m_playableWriting.end()) {
        writing->notifyFailure = true;
        return {};
    }
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        if (beginPlayableWrite(cacheKey, mediaKey, cached, {}, true))
            return {};
    }
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = 0;
        request.timeoutClass = 1;
        request.priority = 0;
        dispatch(request);
    } else {
        promoteQueuedRequest(cacheKey, 0, 1);
    }
    return {};
}

void MediaBridge::prefetchPlayable(const QString &mediaKey, double sizeBytes,
                                   double capBytes)
{
    if (mediaKey.isEmpty()
        || mediaKey.contains(QLatin1String("send-queue.localhost"))
        || !supported())
        return;
    const qint64 declared = static_cast<qint64>(sizeBytes);
    const qint64 cap = capBytes > 0 ? static_cast<qint64>(capBytes)
                                    : kSpeculativePlayableMaxBytes;
    if (declared <= 0 || declared > cap)
        return;
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const QString path = m_playableFiles.value(cacheKey);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        if (m_posterWanted.remove(cacheKey))
            startPosterExtraction(mediaKey, path);
        return;
    }
    if (failureBlocks(cacheKey))
        return;
    if (m_playableWriting.contains(cacheKey))
        return;
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
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
    request.timeoutClass = 1;
    request.priority = 3;
    qCDebug(lcMediaTrace, "prefetch %s (declared %lld bytes)",
            qUtf8Printable(keyTag(cacheKey)),
            static_cast<long long>(declared));
    dispatch(request);
}

QString MediaBridge::videoPosterSource(const QString &mediaKey,
                                       double sizeBytes, double capBytes)
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
    m_posterWanted.insert(playableKey);
    prefetchPlayable(mediaKey, sizeBytes, capBytes);
    if (!alreadyPending(playableKey) && !m_playableFiles.contains(playableKey)
        && !m_playableWriting.contains(playableKey))
        m_posterWanted.remove(playableKey);
    return {};
}

void MediaBridge::abortFetch(const QString &cacheKey)
{
    m_prefetchWanted.remove(cacheKey);
    m_posterWanted.remove(cacheKey);
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
    pump();
}

bool MediaBridge::hasPrefetch(const QString &mediaKey) const
{
    return !mediaKey.isEmpty()
           && m_prefetchWanted.contains(mediaCacheKey(mediaKey, 0));
}

void MediaBridge::cancelPrefetch(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    if (m_playableWanted.value(cacheKey, 0) > 0
        || m_animatedWanted.contains(cacheKey))
        return;
    if (!m_prefetchWanted.contains(cacheKey))
        return;
    abortFetch(cacheKey);
}

void MediaBridge::cancelPlayable(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const auto wanted = m_playableWanted.find(cacheKey);
    if (wanted == m_playableWanted.end())
        return;
    if (--wanted.value() > 0)
        return;
    m_playableWanted.erase(wanted);
    if (m_animatedWanted.contains(cacheKey)) {
        m_prefetchWanted.remove(cacheKey);
        m_posterWanted.remove(cacheKey);
        return;
    }
    abortFetch(cacheKey);
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
    if (m_multimediaWarmed)
        return;
    const QCoreApplication *app = QCoreApplication::instance();
    if (!app || !app->inherits("QGuiApplication"))
        return;
    m_multimediaWarmed = true;
    ensurePosterExtractor()->warmUp();
}

void MediaBridge::onPosterReady(const QString &mediaKey,
                                const QByteArray &jpeg)
{
    if (!m_posterExtracting.remove(mediaKey))
        return;
    const QString posterKey = mediaCacheKey(mediaKey, 1);
    if (jpeg.isEmpty()) {
        m_failed.insert(posterKey, {QStringLiteral("rejected"),
                                    m_failureClock.elapsed()});
        Q_EMIT mediaFetchFailed(posterKey, QStringLiteral("rejected"));
        return;
    }
    insertCache(posterKey, jpeg);
    qCDebug(lcMediaTrace, "poster %s bytes=%lld",
            qUtf8Printable(keyTag(posterKey)),
            static_cast<long long>(jpeg.size()));
    {
        QBuffer buffer;
        buffer.setData(jpeg);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        if (size.isValid() && size.width() > 0 && size.height() > 0
            && !isLinkMediaKey(mediaKey))
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
    if (bytes.mid(4, 4) == QByteArrayLiteral("ftyp"))
        return mime.startsWith(QLatin1String("audio/"))
            ? QStringLiteral("m4a") : QStringLiteral("mp4");
    if (u8(0) == 0x1A && u8(1) == 0x45 && u8(2) == 0xDF && u8(3) == 0xA3)
        return mime.contains(QLatin1String("webm"))
            ? QStringLiteral("webm") : QStringLiteral("mkv");
    if (bytes.startsWith("OggS"))
        return QStringLiteral("ogg");
    if (bytes.startsWith("RIFF")
        && bytes.mid(8, 4) == QByteArrayLiteral("WAVE"))
        return QStringLiteral("wav");
    if (bytes.startsWith("fLaC"))
        return QStringLiteral("flac");
    if (bytes.startsWith("ID3"))
        return QStringLiteral("mp3");
    if (u8(0) == 0xFF && (u8(1) & 0xE0) == 0xE0 && (u8(1) & 0x06) != 0
        && mime == QLatin1String("audio/mpeg"))
        return QStringLiteral("mp3");
    if (u8(0) == 0xFF && (u8(1) & 0xF6) == 0xF0
        && (mime == QLatin1String("audio/aac")
            || mime == QLatin1String("audio/aacp")))
        return QStringLiteral("aac");
    return {};
}

QString MediaBridge::previewAnimatedSource(const QString &dataSource,
                                           const QString &mimetype)
{
    if ((mimetype != QLatin1String("image/gif")
         && mimetype != QLatin1String("image/webp"))
        || !dataSource.startsWith(QStringLiteral("data:") + mimetype
                                  + QStringLiteral(";base64,")))
        return {};
    const QPair<qsizetype, size_t> memoKey(
        dataSource.size(), qHash(dataSource, QHashSeed::globalSeed()));
    const auto memo = m_previewAnimMemo.constFind(memoKey);
    if (memo != m_previewAnimMemo.constEnd()) {
        const QString &url = memo->first;
        if (url.isEmpty())
            return url;
        if (QFileInfo::exists(QUrl(url).toLocalFile())) {
            m_animatedLru.removeOne(memo->second);
            m_animatedLru.prepend(memo->second);
            return url;
        }
    }
    ++m_previewDecodes;
    if (m_previewAnimMemo.size() >= 256)
        m_previewAnimMemo.clear();
    const QByteArray bytes = QByteArray::fromBase64(
        dataSource.mid(dataSource.indexOf(QLatin1Char(',')) + 1).toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (bytes.isEmpty())
        return {};
    const QString cacheKey = QStringLiteral("preview:") + QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    QString result;
    const QString existing = m_animatedFiles.value(cacheKey);
    if (!existing.isEmpty() && QFileInfo::exists(existing)) {
        m_animatedLru.removeOne(cacheKey);
        m_animatedLru.prepend(cacheKey);
        result = QUrl::fromLocalFile(existing).toString();
    } else {
        const QString path = writeAnimatedFile(cacheKey, bytes);
        if (!path.isEmpty()) {
            result = QUrl::fromLocalFile(path).toString();
        } else if (!animatedExtensionFor(bytes).isEmpty()
                   && bytes.size() <= kMaxAnimatedBytes) {
            return {};
        }
    }
    m_previewAnimMemo.insert(memoKey, {result, cacheKey});
    return result;
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
        request.priority = 1;
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
        request.priority = 1;
        dispatch(request);
    } else {
        promoteQueuedRequest(cacheKey, 1, 0);
    }
    return {};
}

QString MediaBridge::wideImageCacheKey(const QString &mxcUri)
{
    return mxcCacheKey(mxcUri, 0);
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
    if (!natural.isValid())
        return {};
    if (natural.width() > 4096 || natural.height() > 4096)
        return {};
    return reader.read();
}

QString MediaBridge::mxcImageSource(const QString &mxcUri, int edge)
{
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
        request.priority = 1;
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
        noteMediaActivity();
        return;
    }
    Pending tracked = request;
    tracked.dispatchedAtMs = m_failureClock.elapsed();
    quint64 opId = 0;
    if (tracked.isMxc
        && tracked.cacheKey.startsWith(QLatin1String("motion:")))
        opId = m_client->fetchMxcThumbnail(tracked.mediaKey, 0,
                                           kMotionProbeHeight);
    else if (tracked.isMxc)
        opId = m_client->fetchMxcThumbnail(tracked.mediaKey, tracked.size,
                                           tracked.size);
    else if (isLinkMediaKey(tracked.mediaKey))
        opId = dispatchLinkMedia(tracked);
    else
        opId = m_client->fetchMedia(tracked.mediaKey, tracked.kind,
                                    tracked.timeoutClass);
    if (opId == 0) {
        ++m_statFailed;
        qCWarning(lcMedia, "fetch %s unavailable (backend returned opId=0)",
                  qUtf8Printable(keyTag(tracked.cacheKey)));
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
    if (!tracked.saveRequest && !tracked.starRequest)
        noteDispatched(tracked.cacheKey);
    qCDebug(lcMediaTrace, "fetch %s opId=%llu inflight=%lld",
            qUtf8Printable(keyTag(tracked.cacheKey)),
            static_cast<unsigned long long>(opId),
            static_cast<long long>(m_inflight.size() + 1));
    m_inflight.insert(opId, tracked);
}

void MediaBridge::noteDispatched(const QString &cacheKey)
{
    ++m_windowFetches;
    if (m_fetchCounts.size() >= 4096)
        m_fetchCounts.clear();
    if (++m_fetchCounts[cacheKey] < 2)
        return;
    ++m_windowRefetchTotal;
    if (m_windowRefetches.size() < 256 || m_windowRefetches.contains(cacheKey))
        ++m_windowRefetches[cacheKey];
}

QVariantMap MediaBridge::takeTraceWindow()
{
    QString top;
    int topCount = 0;
    for (auto it = m_windowRefetches.constBegin();
         it != m_windowRefetches.constEnd(); ++it) {
        if (it.value() > topCount) {
            topCount = it.value();
            top = keyTag(it.key());
        }
    }
    QVariantMap out;
    out.insert(QStringLiteral("fetches"), m_windowFetches);
    out.insert(QStringLiteral("refetches"), m_windowRefetchTotal);
    out.insert(QStringLiteral("refetchKeys"),
               static_cast<qint64>(m_windowRefetches.size()));
    out.insert(QStringLiteral("topRefetch"),
               topCount > 0
                   ? QString(top + QLatin1Char('x') + QString::number(topCount))
                   : QStringLiteral("-"));
    {
        QMutexLocker lock(&m_cacheMutex);
        out.insert(QStringLiteral("evictions"), m_windowEvictions);
        out.insert(QStringLiteral("cacheBytes"),
                   m_cacheBytesMain + m_cacheBytesAvatar);
        m_windowEvictions = 0;
    }
    out.insert(QStringLiteral("decodeMisses"),
               m_windowDecodeMisses.exchange(0, std::memory_order_relaxed));
    m_windowFetches = 0;
    m_windowRefetchTotal = 0;
    m_windowRefetches.clear();
    return out;
}

quint64 MediaBridge::dispatchLinkMedia(Pending &request)
{
    if (request.kind != 0 || !m_linkResolver)
        return 0;
    const QVariantMap target = m_linkResolver(request.mediaKey);
    const QByteArray held = target.value(QStringLiteral("bytes")).toByteArray();
    if (!held.isEmpty()) {
        const quint64 opId = m_nextLocalOpId++;
        const QString mediaKey = request.mediaKey;
        const QString mime = target.value(QStringLiteral("mime")).toString();
        QMetaObject::invokeMethod(
            this,
            [this, opId, mediaKey, held, mime] {
                onMediaReady(opId, mediaKey, 0, held, mime, QString());
            },
            Qt::QueuedConnection);
        return opId;
    }
    if (!m_client)
        return 0;
    const QString mxc = target.value(QStringLiteral("mxc")).toString();
    if (mxc.startsWith(QLatin1String("mxc://")))
        return m_client->fetchMxcThumbnail(mxc, 0, 0);
    const QString url = target.value(QStringLiteral("url")).toString();
    if (url.isEmpty())
        return 0;
    request.timeoutClass = qMax(request.timeoutClass, 1);
    return m_client->fetchLinkMedia(
        url, request.mediaKey,
        target.value(QStringLiteral("expect")).toInt(), request.timeoutClass);
}

void MediaBridge::pump()
{
    while (!m_queue.isEmpty() && m_inflight.size() < kMaxConcurrent) {
        const qint64 now = m_failureClock.elapsed();
        const int heavy = heavyInflightCount();
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
        if (oldest >= 0
            && now - m_queue.at(oldest).enqueuedAtMs >= m_starvationMs)
            chosen = oldest;
        if (chosen < 0)
            break;
        dispatch(m_queue.takeAt(chosen));
    }
}

void MediaBridge::checkInflightTimeouts()
{
    sweepExpiredFailureMarks();
    if (m_inflight.isEmpty())
        return;
    const qint64 now = m_failureClock.elapsed();
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
        writeSaveFile(request.saveDestination, bytes, request.mediaKey,
                      request.saveUniquify);
        return;
    }
    if (request.starRequest) {
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
    const bool playableWanted = m_playableWanted.contains(request.cacheKey);
    const bool prefetchWanted = m_prefetchWanted.contains(request.cacheKey);
    if (request.kind == 0 && !isLinkMediaKey(request.mediaKey)
        && looksLikeAvContainer(bytes))
        Q_EMIT playableSizeLearned(request.mediaKey,
                                   static_cast<qint64>(bytes.size()));
    const bool motionProbe =
        request.cacheKey.startsWith(QLatin1String("motion:"));
    // A speculative A/V payload is served from its playable file; in the byte
    // cache it would only evict pictures that are on screen.
    const bool speculativeAv = prefetchWanted && !playableWanted
        && !m_animatedWanted.contains(request.cacheKey)
        && looksLikeAvContainer(bytes);
    if (!motionProbe && !speculativeAv
        && !((playableWanted || prefetchWanted)
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
    if (motionProbe) {
        if (!writeMotionFile(request.cacheKey, bytes,
                             lightning::animsniff::kAvatarMaxPixels)
                 .isEmpty())
            Q_EMIT animatedMediaReady(request.cacheKey);
        return;
    }
    if (m_animatedWanted.remove(request.cacheKey)) {
        const bool demanded = m_animatedDemanded.remove(request.cacheKey);
        if (!writeAnimatedFile(request.cacheKey, bytes).isEmpty())
            Q_EMIT animatedMediaReady(request.cacheKey);
        else if (demanded)
            Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("invalid_gif"));
    }
    if (playableWanted || prefetchWanted) {
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
    const auto existing = m_playableWriting.find(cacheKey);
    if (existing != m_playableWriting.end()) {
        existing->notifyFailure = existing->notifyFailure || notifyFailure;
        return true;
    }
    if (bytes.isEmpty() || bytes.size() > m_playableMaxBytes
        || !m_animatedDir || !m_animatedDir->isValid())
        return false;
    const QString extension = playableExtensionFor(bytes, mimetype);
    if (extension.isEmpty())
        return false;
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
        return false;
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
    const auto it = m_playableWriting.find(cacheKey);
    if (it == m_playableWriting.end() || it->serial != serial
        || generation != m_sessionGeneration) {
        if (ok)
            QFile::remove(path);
        return;
    }
    const PendingPlayableWrite pending = it.value();
    m_playableWriting.erase(it);
    if (!ok) {
        m_posterWanted.remove(cacheKey);
        m_playableWanted.remove(cacheKey);
        m_prefetchWanted.remove(cacheKey);
        if (pending.notifyFailure)
            Q_EMIT mediaFetchFailed(cacheKey, QStringLiteral("rejected"));
        return;
    }
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
    ++m_pinnedPlayables[mediaCacheKey(mediaKey, 0)];
}

void MediaBridge::unpinPlayable(const QString &mediaKey)
{
    if (mediaKey.isEmpty())
        return;
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const auto it = m_pinnedPlayables.find(cacheKey);
    if (it == m_pinnedPlayables.end())
        return;
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
        if (p.priority == 3 && !p.saveRequest && !p.starRequest
            && !m_playableWanted.contains(p.cacheKey)) {
            m_animatedWanted.remove(p.cacheKey);
            m_animatedDemanded.remove(p.cacheKey);
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
    if (bytes.size() >= 21 && bytes.startsWith("RIFF")
        && std::memcmp(bytes.constData() + 8, "WEBP", 4) == 0
        && std::memcmp(bytes.constData() + 12, "VP8X", 4) == 0
        && (static_cast<unsigned char>(bytes.at(20)) & 0x02u) != 0)
        return QStringLiteral("webp");
    return {};
}

QString MediaBridge::writeAnimatedFile(const QString &cacheKey,
                                       const QByteArray &bytes)
{
    constexpr qsizetype maxAnimatedBytes = kMaxAnimatedBytes;
    const QString extension = animatedExtensionFor(bytes);
    if (extension.isEmpty() || bytes.size() > maxAnimatedBytes
        || !m_animatedDir || !m_animatedDir->isValid())
        return {};
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(cacheKey.toUtf8(), QCryptographicHash::Sha256).toHex())
        + QLatin1Char('-') + QString::number(++m_animatedWriteSerial)
        + QLatin1Char('.') + extension;
    const QString path = m_animatedDir->filePath(name);
    // 0600 before writing (ScratchFile::open): these are decrypted payloads
    // and must not rely on the 0700 directory alone. Session scratch, so no
    // fsync (storage/ScratchFile.h).
    lightning::scratch::ScratchFile file(path);
    if (!file.open())
        return {};
    if (file.write(bytes) != bytes.size() || !file.commit())
        return {};
    const QString previous = m_animatedFiles.value(cacheKey);
    if (!previous.isEmpty() && previous != path && !isAnimationHeld(cacheKey))
        QFile::remove(previous);
    m_animatedFiles.insert(cacheKey, path);
    m_animatedSizes.insert(cacheKey, bytes.size());
    m_animatedLru.removeOne(cacheKey);
    m_animatedLru.prepend(cacheKey);
    qint64 total = 0;
    for (qint64 size : std::as_const(m_animatedSizes))
        total += size;
    // Oldest first, never the newest and never a held file: deleting a file
    // an AnimatedImage is playing freezes it for good.
    for (qsizetype i = m_animatedLru.size() - 1;
         i > 0
         && (total > kAnimatedCacheBytes
             || m_animatedFiles.size() > kAnimatedCacheEntries);
         --i) {
        const QString victim = m_animatedLru.at(i);
        if (isAnimationHeld(victim))
            continue;
        m_animatedLru.removeAt(i);
        total -= m_animatedSizes.take(victim);
        QFile::remove(m_animatedFiles.take(victim));
    }
    for (qsizetype i = m_animatedLru.size() - 1;
         i > 0
         && (total > kAnimatedHardCapBytes
             || m_animatedFiles.size() > kAnimatedHardCapEntries);
         --i) {
        const QString victim = m_animatedLru.takeAt(i);
        total -= m_animatedSizes.take(victim);
        QFile::remove(m_animatedFiles.take(victim));
    }
    return path;
}

bool MediaBridge::isAnimationHeld(const QString &cacheKey) const
{
    for (const AnimationHold &hold : m_animationHolds) {
        if (hold.cacheKey == cacheKey)
            return true;
    }
    return false;
}

bool MediaBridge::holdAnimation(QObject *owner, const QString &fileUrl)
{
    if (!owner)
        return false;
    const QString path = QUrl(fileUrl).toLocalFile();
    QString cacheKey;
    if (!path.isEmpty()) {
        for (auto it = m_animatedFiles.cbegin(); it != m_animatedFiles.cend();
             ++it) {
            if (it.value() == path) {
                cacheKey = it.key();
                break;
            }
        }
    }
    if (cacheKey.isEmpty()) {
        releaseAnimation(owner);
        return false;
    }
    const auto held = m_animationHolds.find(owner);
    if (held != m_animationHolds.end()) {
        held->cacheKey = cacheKey;
        return true;
    }
    AnimationHold hold;
    hold.cacheKey = cacheKey;
    hold.destroyed = connect(owner, &QObject::destroyed, this,
                             [this, owner] { releaseAnimation(owner); });
    m_animationHolds.insert(owner, hold);
    return true;
}

void MediaBridge::releaseAnimation(QObject *owner)
{
    const auto it = m_animationHolds.find(owner);
    if (it == m_animationHolds.end())
        return;
    disconnect(it->destroyed);
    m_animationHolds.erase(it);
}

void MediaBridge::dropInterestSets(const QString &cacheKey)
{
    m_playableWanted.remove(cacheKey);
    m_animatedWanted.remove(cacheKey);
    m_animatedDemanded.remove(cacheKey);
    m_prefetchWanted.remove(cacheKey);
    m_posterWanted.remove(cacheKey);
}

void MediaBridge::markFailed(const Pending &request, const QString &category,
                             qint64 retryAfterMs)
{
    if (request.saveRequest || request.starRequest)
        return;
    if (m_failed.size() >= kMaxFailureMarks)
        m_failed.clear();
    m_failed.insert(request.cacheKey,
                    {category, m_failureClock.elapsed(), retryAfterMs});
}

void MediaBridge::onMediaFailed(quint64 opId, const QString &mediaKey, int kind,
                                const QString &rawCategory)
{
    QString category = rawCategory;
    qint64 retryAfterMs = 0;
    static const QLatin1String kUnreachable("server_unreachable");
    if (rawCategory.startsWith(kUnreachable)) {
        category = kUnreachable;
        bool ok = false;
        const qint64 ms = rawCategory.mid(kUnreachable.size() + 1).toLongLong(&ok);
        if (ok && ms > 0)
            retryAfterMs = qMin<qint64>(ms, 10 * 60 * 1000);
    }
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
        Q_EMIT saveFinished(
            false,
            category == kUnreachable
                ? tr("The media server is unreachable. Try again in a few "
                     "minutes.")
                : tr("The file could not be downloaded."),
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
    if (category == kUnreachable)
        qCDebug(lcMediaTrace, "failed %s category=%s retry_after_ms=%lld",
                qUtf8Printable(keyTag(request.cacheKey)),
                qUtf8Printable(category),
                static_cast<long long>(retryAfterMs));
    else
        qCWarning(lcMedia, "failed %s category=%s",
                  qUtf8Printable(keyTag(request.cacheKey)),
                  qUtf8Printable(category));
    dropInterestSets(request.cacheKey);
    if (category == QLatin1String("too_large")
        && request.cacheKey.startsWith(QLatin1String("motion:"))) {
        if (m_motionVerdict.size() >= kMaxMotionVerdicts)
            m_motionVerdict.clear();
        m_motionVerdict.insert(request.cacheKey, 0);
    }
    markFailed(request, category, retryAfterMs);
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
    out = matrix::presentable_name::withoutFormatCharacters(out);
    for (QChar &c : out) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QLatin1Char('_');
    }
    while (out == QLatin1String("..") || out.startsWith(QLatin1String("../"))
           || out.startsWith(QLatin1String("..\\")))
        out.remove(0, 2);
    out = out.trimmed();
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
    if (rawName.trimmed().isEmpty())
        return {};
    const QString leaf = sanitizedFileName(rawName);
    return leaf == QLatin1String("download") ? QString() : leaf;
}

void MediaBridge::saveAs(const QString &mediaKey, const QUrl &destination)
{
    startSave(mediaKey, destination, false);
}

void MediaBridge::saveInto(const QString &mediaKey, const QString &folder,
                           const QString &leaf)
{
    if (folder.isEmpty() || leaf.isEmpty()) {
        Q_EMIT saveFinished(false, tr("No destination selected."), mediaKey);
        return;
    }
    startSave(mediaKey, QUrl::fromLocalFile(QDir(folder).filePath(leaf)), true);
}

void MediaBridge::startSave(const QString &mediaKey, const QUrl &destination,
                            bool uniquify)
{
    if (mediaKey.isEmpty() || !supported() || !destination.isLocalFile()) {
        Q_EMIT saveFinished(false, tr("No destination selected."), mediaKey);
        return;
    }
    const QByteArray cached = cachedBytes(mediaCacheKey(mediaKey, 0));
    if (!cached.isEmpty()) {
        writeSaveFile(destination, cached, mediaKey, uniquify);
        return;
    }
    Pending request;
    request.cacheKey = mediaCacheKey(mediaKey, 0);
    request.mediaKey = mediaKey;
    request.kind = 0;
    request.saveRequest = true;
    request.saveDestination = destination;
    request.saveUniquify = uniquify;
    request.timeoutClass = 2;
    request.priority = 0;
    m_activeSaves.append({mediaKey,
                          sanitizedFileName(destination.fileName())});
    Q_EMIT activeSavesChanged();
    dispatch(request);
}

QVariantList MediaBridge::activeSaves() const
{
    QVariantList out;
    for (const ActiveSave &s : m_activeSaves) {
        out.append(QVariantMap{{QStringLiteral("mediaKey"), s.mediaKey},
                               {QStringLiteral("fileName"), s.fileName}});
    }
    return out;
}

QStringList MediaBridge::savingKeys() const
{
    QStringList out;
    for (const ActiveSave &s : m_activeSaves)
        out.append(s.mediaKey);
    return out;
}

void MediaBridge::removeActiveSave(const QString &mediaKey)
{
    for (int i = 0; i < m_activeSaves.size(); ++i) {
        if (m_activeSaves.at(i).mediaKey == mediaKey) {
            m_activeSaves.removeAt(i);
            Q_EMIT activeSavesChanged();
            return;
        }
    }
}

void MediaBridge::cancelSave(const QString &mediaKey)
{
    bool tracked = false;
    for (const ActiveSave &s : std::as_const(m_activeSaves))
        tracked = tracked || s.mediaKey == mediaKey;
    if (!tracked)
        return;
    for (int i = m_queue.size() - 1; i >= 0; --i) {
        const Pending &p = m_queue.at(i);
        if (p.saveRequest && p.mediaKey == mediaKey)
            m_queue.removeAt(i);
    }
    for (auto it = m_inflight.begin(); it != m_inflight.end();) {
        if (it->saveRequest && it->mediaKey == mediaKey) {
            const quint64 opId = it.key();
            it = m_inflight.erase(it);
            ++m_statCancelled;
            if (m_client)
                m_client->cancelMediaFetch(opId);
        } else {
            ++it;
        }
    }
    m_activeSaves.removeIf(
        [&](const ActiveSave &s) { return s.mediaKey == mediaKey; });
    Q_EMIT activeSavesChanged();
    Q_EMIT saveCancelled(mediaKey);
    Q_EMIT saveFinished(false, tr("Download cancelled."), mediaKey);
    pump();
}

void MediaBridge::fetchFullForStar(const QString &mediaKey)
{
    if (mediaKey.isEmpty() || !supported()) {
        Q_EMIT mediaBytesForStar(mediaKey, false, {},
                                 QStringLiteral("unavailable"));
        return;
    }
    const QByteArray cached = cachedBytes(mediaCacheKey(mediaKey, 0));
    if (!cached.isEmpty()) {
        Q_EMIT mediaBytesForStar(mediaKey, true, cached, QString());
        return;
    }
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
    request.timeoutClass = 2;
    request.priority = 0;
    dispatch(request);
}

QString MediaBridge::cachedFullContentHash(const QString &mediaKey) const
{
    if (mediaKey.isEmpty())
        return {};
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    QByteArray bytes;
    quint32 rev = 0;
    {
        QMutexLocker lock(&m_cacheMutex);
        const auto it = m_cache.constFind(cacheKey);
        if (it == m_cache.constEnd())
            return {};
        rev = m_revision.value(cacheKey);
        const auto memoized = m_contentHashCache.constFind(cacheKey);
        if (memoized != m_contentHashCache.constEnd() && memoized->revision == rev)
            return memoized->hex;
        bytes = it.value();
    }

    const QString hex = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    QMutexLocker lock(&m_cacheMutex);
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
                                const QString &mediaKey, bool uniquify)
{
    const QFileInfo chosen(destination.toLocalFile());
    const QDir parent(QFileInfo(chosen.absolutePath()).canonicalFilePath());
    if (!parent.exists()) {
        Q_EMIT saveFinished(false, tr("The destination is not writable."),
                            mediaKey);
        return;
    }
    const QString leaf = sanitizedFileName(chosen.fileName());
    if (uniquify) {
        for (int n = 0; n < 1000; ++n) {
            const QString candidate =
                parent.filePath(savenaming::numberedName(leaf, n));
            QFile file(candidate);
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                if (QFileInfo::exists(candidate))
                    continue;
                Q_EMIT saveFinished(false,
                                    tr("The destination is not writable."),
                                    mediaKey);
                return;
            }
            if (file.write(bytes) != bytes.size() || !file.flush()) {
                file.close();
                file.remove();
                Q_EMIT saveFinished(false, tr("Writing the file failed."),
                                    mediaKey);
                return;
            }
            file.close();
            savenaming::markAsDownloaded(candidate);
            Q_EMIT saveCompleted(mediaKey,
                                 QFileInfo(candidate).absoluteFilePath());
            Q_EMIT saveFinished(true, tr("Saved."), mediaKey);
            return;
        }
        Q_EMIT saveFinished(false, tr("The destination is not writable."),
                            mediaKey);
        return;
    }
    const QString target = parent.filePath(leaf);
    QSaveFile file(target);
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        Q_EMIT saveFinished(false, tr("The destination is not writable."),
                            mediaKey);
        return;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        Q_EMIT saveFinished(false, tr("Writing the file failed."), mediaKey);
        return;
    }
    savenaming::markAsDownloaded(target);
    Q_EMIT saveCompleted(mediaKey, QFileInfo(target).absoluteFilePath());
    Q_EMIT saveFinished(true, tr("Saved."), mediaKey);
}

void MediaBridge::clear()
{
    stalltrace::Scope stallScope("media-clear");
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
        m_revision.clear();
        m_contentHashCache.clear();
        m_awaitingDecode.clear();
    }
    m_fetchCounts.clear();
    m_windowRefetches.clear();
    m_inflight.clear();
    m_queue.clear();
    if (!m_activeSaves.isEmpty()) {
        m_activeSaves.clear();
        Q_EMIT activeSavesChanged();
    }
    m_failed.clear();
    m_animatedFiles.clear();
    m_animatedSizes.clear();
    m_animatedLru.clear();
    m_previewAnimMemo.clear();
    for (const AnimationHold &hold : std::as_const(m_animationHolds))
        disconnect(hold.destroyed);
    m_animationHolds.clear();
    m_animatedWanted.clear();
    m_animatedDemanded.clear();
    m_motionVerdict.clear();
    m_stagedKeys.clear();
    m_playableFiles.clear();
    m_playableSizes.clear();
    m_playableLru.clear();
    m_playableWanted.clear();
    m_prefetchWanted.clear();
    m_posterWanted.clear();
    m_posterExtracting.clear();
    if (m_posterExtractor) {
        disconnect(m_posterExtractor, nullptr, this, nullptr);
        m_posterExtractor->retireWithoutWaiting();
        m_posterExtractor->deleteLater();
        m_posterExtractor = nullptr;
    }
    m_pinnedPlayables.clear();
    m_playableWriting.clear();
    ++m_sessionGeneration;
    if (m_playableWriter)
        m_playableWriter->cancelAll();
    m_playableNameSalt.clear();
    stalltrace::Scope scratchScope("media-scratch-dir");
    if (m_animatedDir)
        lightning::portable::releaseScratchDir(m_animatedDir->path());
    m_animatedDir.reset();
    m_animatedDir = std::make_unique<QTemporaryDir>(
        lightning::portable::mediaScratchRoot()
        + QStringLiteral("/lightning-animated-XXXXXX"));
    lightning::portable::holdScratchDirLive(m_animatedDir->path());
}

void MediaBridge::retryAvatar(const QString &mxcUri)
{
    if (mxcUri.isEmpty())
        return;
    const QString cacheKey = mxcCacheKey(mxcUri, kAvatarCanonicalEdge);
    m_failed.remove(cacheKey);
    Q_EMIT mediaRetryable(cacheKey);
}

void MediaBridge::onLoggedOut()
{
    clear();
}
