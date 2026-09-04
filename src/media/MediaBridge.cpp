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

// v0.7: media/avatar pipeline diagnostics. Enabled with
//   QT_LOGGING_RULES="lightning.media.debug=true"
// (offscreen/smoke runs force these to stderr — see main.cpp). The category
// logs request reasons, cache hit/miss, stale-generation suppression,
// resolution timing, and fetch results. It NEVER logs decrypted bytes,
// bodies, authenticated download URLs, tokens, or provider keys — only the
// sanitized cache-key tag below, byte counts, coarse MIME, and timings.
Q_LOGGING_CATEGORY(lcMedia, "lightning.media")

// Cache-HIT trace — the hot path. mediaSource()/avatarSource() are called
// from QML delegate bindings and re-run on every pooled-delegate rebind while
// scrolling, so a cache hit (the boring, expected case) logged one line per
// media row PER SCROLL FRAME. Since `lightning.media` is not a `qt.*` category
// its debug output is ON by default, so a plain source run (run-dev.sh, no
// QT_LOGGING_RULES) emitted that storm on the GUI thread during every scroll —
// string formatting + journal/stderr I/O competing with the frame, and the
// "cache=hit log storm" the touchpad pass had to eliminate. Cache MISS,
// dispatch, failure and retry stay on `lightning.media` (bounded: one per real
// fetch). Only the high-frequency hit is demoted to this default-OFF category
// (QtWarningMsg minimum ⇒ qCDebug suppressed unless explicitly enabled with
//   QT_LOGGING_RULES="lightning.media.trace.debug=true").
Q_LOGGING_CATEGORY(lcMediaTrace, "lightning.media.trace", QtWarningMsg)

namespace {
QString mediaCacheKey(const QString &mediaKey, int kind)
{
    return (kind == 1 ? QStringLiteral("thumb:")
                      : kind == 2 ? QStringLiteral("listthumb:")
                                  : QStringLiteral("full:"))
        + mediaKey;
}

// A log-safe, stable tag for a cache key. mxc:// URIs are public content
// identifiers, but SDK media keys can embed room/event structure, so the
// opaque tail of every key is reduced to a short SHA-256 prefix. The reason
// prefix (avatar/thumb/full/…) and, for avatars, the mxc server name are
// kept because they are the useful, non-sensitive parts for debugging.
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
    // Unknown mime stays FALSE: this function is fail-closed by design, so an
    // unrecognised type is refused rather than trusted.
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
    // Marked LIVE, so a second Lightning instance's stale-scratch sweep
    // cannot delete this one's decrypted payloads out from under a playing
    // card. Without the mark these directories are protected only by the
    // sweep's one-hour mtime floor, which a long session outlives.
    lightning::portable::holdScratchDirLive(m_animatedDir->path());
    // The watchdog reclaims concurrency slots pinned by ops the backend never
    // completes; without it a handful of orphaned fetches permanently stalls
    // the pipeline. A 5s cadence bounds the extra latency to reclaim a stuck
    // slot; the timeout itself (m_inflightTimeoutMs) is what a healthy fetch
    // never reaches.
    m_watchdog.setInterval(5000);
    m_watchdog.setTimerType(Qt::CoarseTimer);
    connect(&m_watchdog, &QTimer::timeout,
            this, &MediaBridge::checkInflightTimeouts);
    m_watchdog.start();

    // One line per BURST rather than three per request. 900ms of quiet is
    // well past the tail of a room's avatar fan-out and short enough that the
    // summary still reads as belonging to what just happened.
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
    // CLAUDE.md §6 keeps SVG out of the inline preview/media paths. The
    // declared mimetype cannot carry that rule on its own: a sticker pack is
    // ROOM STATE any member can write, and MSC2545 lets an entry omit
    // `mimetype` entirely — which stickers.rs deliberately allows, so a pack
    // from a future client does not go invisible. So the BYTES decide here,
    // the same argument the A/V sniff above already makes.
    //
    // Every raster format this client accepts opens with binary magic, so no
    // legitimate image-class payload can begin with `<` or with gzip. That
    // makes this refusal cheap AND free of false positives, rather than a
    // list of SVG's spellings (`<svg`, `<?xml`, `<!DOCTYPE`, a comment first)
    // that a hostile file only has to differ from.
    qsizetype i = 0;
    // UTF-8 BOM, then leading whitespace: both are legal before an XML
    // declaration and neither changes what the payload is.
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes.at(0)) == 0xEF
        && static_cast<unsigned char>(bytes.at(1)) == 0xBB
        && static_cast<unsigned char>(bytes.at(2)) == 0xBF)
        i = 3;
    // Explicit, not std::isspace: that is locale-dependent and takes an int
    // whose negative values are UB for a signed char.
    const auto isXmlSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (i < bytes.size() && isXmlSpace(bytes.at(i)))
        ++i;
    if (i < bytes.size() && bytes.at(i) == '<')
        return true;
    // SVGZ: Qt's SVG handler decompresses gzip, so refusing only the plain
    // spelling would leave the same file reachable under another name.
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
        // ISO BMFF — but HEIC/AVIF IMAGES share the container. Qt decodes
        // neither by default today; excluding their brands keeps this
        // honest if an image plugin ever appears.
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
    // The "?r=<revision>" suffix (bumped only on an actual byte insert)
    // guarantees a re-cached payload produces a DIFFERENT source string, so
    // a QML Image that reached Error on the previous string reloads;
    // MediaImageProvider strips it before the key lookup.
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
            // A single accepted image always fits the byte cap. If the entry
            // cap was exhausted, the eviction loop above made a slot.
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
    // Deliberately NO touch(): this is the path Qt's image-decode thread
    // takes through MediaImageProvider, and the LRU reorder is an O(n) list
    // scan under the mutex the GUI thread contends for. Recency is already
    // recorded by the cachedSource() call that produced the provider URL,
    // so skipping it here costs only approximate LRU accuracy.
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
    // Running per-class byte totals replace the previous full-cache
    // iteration on every insert (O(n) with a string-prefix test per entry).
    // An overwrite must retire the old payload's bytes first.
    if (const auto existing = m_cache.constFind(cacheKey);
        existing != m_cache.constEnd())
        classTotal -= existing.value().size();
    m_cache.insert(cacheKey, bytes);
    classTotal += bytes.size();
    // An actual byte insert is the ONLY revision bump: cache hits keep an
    // identical provider URL (pixmap-cache dedup survives), a re-fetch
    // after eviction or replacement produces a new one.
    ++m_revision[cacheKey];
    touch(cacheKey);
    // Evict least-recently-used entries beyond the inserted key's class
    // budget. Classes are disjoint and each bounded, so an avatar insert
    // can never evict timeline media and timeline churn can never evict
    // avatars; total memory stays bounded by the sum of both caps.
    QList<QString> &lru = avatarClass ? m_avatarLru : m_lru;
    const qint64 limit = avatarClass ? m_avatarCacheLimit : m_cacheLimit;
    while (classTotal > limit && lru.size() > 1) {
        const QString victim = lru.takeLast();
        if (victim == cacheKey)
            continue;
        classTotal -= m_cache.value(victim).size();
        m_cache.remove(victim);
        // review H1b: the memoized content hash for an evicted key is dead
        // weight (cachedFullContentHash() can never return it once the
        // bytes are gone) — drop it here rather than leaving it to be
        // silently superseded only if the same key is ever re-inserted.
        m_contentHashCache.remove(victim);
    }
}

bool MediaBridge::alreadyPending(const QString &cacheKey) const
{
    // Save/star requests never satisfy an ordinary caller: onMediaReady's
    // save/star branches return before inserting into the cache or emitting
    // mediaCached(), so an ordinary mediaSource()/animatedSource() call that
    // treated one of them as "already in flight" would wait for a signal
    // that never comes for that key.
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
    // Only validation failures the backend actually reported are permanent
    // ("rejected" media, "invalid_gif" payloads): they never fix
    // themselves, so only an explicit retry() may re-dispatch them.
    // Everything else — network, timeout, and the local "unavailable"
    // dispatch failure (opId==0 while the session restores/switches or the
    // media item is not known yet) — is transient and expires.
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
    // Collect first: the mediaRetryable handlers re-enter the bridge
    // (refresh → avatarSource → dispatch → markFailed on a re-failure),
    // which mutates m_failed.
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
    // READ-THROUGH across the classes, before dispatching anything.
    //
    // The three classes exist so the smaller ones can REFUSE to create an
    // expensive fetch: a list thumbnail never substitutes a full encrypted
    // attachment merely to fill a 42x34 tile. That is about what a class may
    // ASK FOR, not about what it may reuse — and reading bytes another class
    // already holds creates no fetch at all.
    //
    // Without this, opening Room Information -> Media re-fetched and
    // re-cached every item the timeline had already fetched: a live capture
    // showed SEVENTEEN payloads fetched twice in one short session, several
    // of them 500-950 KB, each one a second write through the SDK's media
    // store. Same bytes, different key prefix.
    //
    // Only the SMALLEST class borrows, and only from a larger one.
    //
    // The direction is the whole safety argument. The 42x34 list tile can
    // render anything at least its own size, so reusing a timeline thumbnail
    // or a full payload costs nothing and loses nothing. The reverse is a
    // silent downgrade: serving a timeline row the list tile's bytes renders
    // a blurry image at the wrong natural size.
    //
    // My first version of this had `thumb` borrow from `listthumb` too, and
    // timeline-pane-qml caught it immediately —
    // `topEdgePrependKeepsReaderOnTheSameRowMidGesture` went from flaky to
    // failing 2/2, because rows that used to resolve asynchronously at their
    // real size now resolved synchronously at the wrong one and moved the
    // reader mid-prepend.
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
    // A marked failure blocks re-dispatch (transient marks expire; see
    // failureBlocks) — QML repolling a broken source must not turn into a
    // request loop.
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
        // Thumbnails are visible chrome; a full static payload is heavier
        // and can wait behind them.
        request.priority = kindValue != 0 ? 1 : 2;
        qCDebug(lcMediaTrace, "media %s cache=miss dispatching",
                qUtf8Printable(keyTag(cacheKey)));
        dispatch(request);
    } else {
        promoteQueuedRequest(cacheKey, kindValue != 0 ? 1 : 2, 0);
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
        // Speculative: the full-GIF prefetch for autoplay. Never allowed to
        // starve visible chrome or explicit playback.
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
    ++m_playableWanted[cacheKey]; // refcounted (review M1)
    if (failureBlocks(cacheKey)) {
        // Blocked: no fetch will run for this call — do not leave a
        // phantom interest count behind.
        if (--m_playableWanted[cacheKey] <= 0)
            m_playableWanted.remove(cacheKey);
        return {};
    }
    const auto writing = m_playableWriting.find(cacheKey);
    if (writing != m_playableWriting.end()) {
        // A write for these bytes is already on the worker thread: this
        // caller is coalesced onto it and answered by the single
        // playableMediaReady broadcast, and it upgrades that write to one
        // that owes a terminal answer if it fails. The interest count
        // stays — it is what a cancel aborts and what the completion
        // retires. Dispatching a second fetch here would download the same
        // payload twice.
        writing->notifyFailure = true;
        return {};
    }
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        // Mimetype intentionally empty: the container magic decides.
        if (beginPlayableWrite(cacheKey, mediaKey, cached, {}, true))
            return {}; // materializing off-thread; QML re-asks on the signal
        // Refused before any file was created (unknown container, over the
        // size bound). Fall through to a fetch exactly as before, KEEPING
        // the interest count: the dispatched fetch is what will consume it,
        // and dropping it here would make onMediaReady see no playable
        // consumer and never materialize the payload it just downloaded.
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
        // A speculative prefetch may already hold this key in the queue;
        // the pressed-play caller must not inherit its class.
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
    // Fail-safe bound: only a declared, in-cap size is worth speculative
    // bandwidth. Unknown sizes wait for explicit Play.
    const qint64 declared = static_cast<qint64>(sizeBytes);
    if (declared <= 0 || declared > kSpeculativePlayableMaxBytes)
        return;
    const QString cacheKey = mediaCacheKey(mediaKey, 0);
    const QString path = m_playableFiles.value(cacheKey);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        // Already materialized — only a pending poster hook may remain.
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
        // playableMediaReady and the poster hook now fire from the write
        // completion, on this thread. A speculative prefetch is owed no
        // terminal failure signal, hence notifyFailure = false.
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
    // Materialize first (bounded by the speculative cap), then extract when
    // onMediaReady writes the file. An over-cap or unknown-size video keeps
    // the styled placeholder until it is actually played — at which point
    // the materialized file exists and the next poster request succeeds.
    m_posterWanted.insert(playableKey);
    prefetchPlayable(mediaKey, sizeBytes);
    // The prefetch may decline (over-cap or unknown declared size, failure
    // mark, unsupported): a hook with no materialization path would leak
    // AND veto later cancels (review H1/M3). Keep it only while something
    // can actually deliver the file — which now includes a write already
    // running on the worker thread, whose completion fires the hook.
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
    // Refcounted (review M1): another card still waits on the same bytes.
    if (--wanted.value() > 0)
        return;
    m_playableWanted.erase(wanted);
    // A GIF row wanting the same bytes keeps the fetch alive. A pending
    // POSTER hook or speculative prefetch does NOT veto a user cancel
    // (review H1): the poster is a derivative nicety that can be
    // re-derived whenever the file is next materialized, while the cancel
    // frees a live multi-hundred-MB transfer now.
    if (m_animatedWanted.contains(cacheKey)) {
        m_prefetchWanted.remove(cacheKey);
        m_posterWanted.remove(cacheKey);
        return;
    }
    m_prefetchWanted.remove(cacheKey);
    m_posterWanted.remove(cacheKey);
    // A write already handed to the worker thread is abandoned too: the
    // bytes are downloaded, but a multi-hundred-megabyte write into the
    // session temp directory is real disk churn for a card that is gone.
    // QSaveFile discards the partial file and no completion is emitted, so
    // erasing the tracking entry here leaves nothing pending — and a fresh
    // Play re-materializes from the cached bytes.
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
    // Called once a playable A/V payload actually exists on disk, which is
    // the first moment this session is known to need a decoder. The FIRST
    // QVideoSink in a process costs ~931 ms (lazy Qt Multimedia backend
    // initialization plus a hardware-decoder probe); paying it here, on
    // the extractor's worker thread, keeps it off the click that starts
    // inline playback — QML builds that sink on the GUI thread, so the
    // cost can only be avoided by having already paid it elsewhere.
    // Not reset by clear(): the initialization is process-global, so a
    // later session would find nothing left to do.
    if (m_multimediaWarmed)
        return;
    // Inline playback needs a GUI application, so under a guiless one
    // there is nothing to warm FOR — and the guiless media suites, which
    // materialize playable payloads dozens of times, must keep their
    // promise of never constructing a decoder. Checked by name so this
    // file takes no dependency on QtGui.
    const QCoreApplication *app = QCoreApplication::instance();
    if (!app || !app->inherits("QGuiApplication"))
        return;
    m_multimediaWarmed = true;
    ensurePosterExtractor()->warmUp();
}

void MediaBridge::onPosterReady(const QString &mediaKey,
                                const QByteArray &jpeg)
{
    // Session isolation. The extractor decodes on its own thread now, so
    // its completion reaches us as a QUEUED call, and one already posted
    // to this thread's event queue can outlive the disconnect in clear()
    // and land in the next account's cache. The tracking set is therefore
    // the authority, not the connection — clearing it makes any late
    // delivery inert by construction.
    if (!m_posterExtracting.remove(mediaKey))
        return;
    const QString posterKey = mediaCacheKey(mediaKey, 1);
    if (jpeg.isEmpty()) {
        // Permanent for this session: re-decoding the same file would fail
        // the same way. An explicit retry() (the cover tap) clears it.
        m_failed.insert(posterKey, {QStringLiteral("rejected"),
                                    m_failureClock.elapsed()});
        Q_EMIT mediaFetchFailed(posterKey, QStringLiteral("rejected"));
        return;
    }
    insertCache(posterKey, jpeg);
    qCDebug(lcMediaTrace, "poster %s bytes=%lld",
            qUtf8Printable(keyTag(posterKey)),
            static_cast<long long>(jpeg.size()));
    // Header-only decode: the poster's dimensions carry the video's true
    // display shape (the extractor works on rendered frames, so rotation
    // is already applied). The scaled size is fine — the card consumes
    // the RATIO and caps the magnitude anyway.
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
    // MP3: ID3 tag or a bare MPEG audio frame sync. The layer bits must be
    // non-zero — ADTS AAC shares the 0xFFE sync but always has layer 00,
    // and a mislabeled ADTS stream must not sniff as MP3.
    if (bytes.startsWith("ID3"))
        return QStringLiteral("mp3");
    if (u8(0) == 0xFF && (u8(1) & 0xE0) == 0xE0 && (u8(1) & 0x06) != 0
        && mime == QLatin1String("audio/mpeg"))
        return QStringLiteral("mp3");
    // Raw AAC in ADTS framing — accepted only when the metadata says AAC.
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
    // One canonical fetch per identity: the requested render size never
    // reaches the cache key, so every surface (room list, header, timeline,
    // popover, rail) shares a single request, entry, and failure mark, and
    // is consistent by construction. QML scales down at render time.
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
    // kind 0 = the full payload, not a thumbnail: a banner is 3:1 and a
    // square thumbnail of one is not a banner. Its own cache key (edge 0) so
    // it can never collide with the avatar entry for the same mxc.
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
    // Same discipline as MediaImageProvider: the format is decided by the
    // bytes, from the known raster table, with autodetection OFF and a bounded
    // allocation. This feeds notification icons, so the bytes are another
    // user's avatar — and `sniffRaster`'s table has no SVG entry, so an
    // unrecognised payload cannot reach the SVG handler here either.
    // A FORMAT WE RECOGNISE IS PINNED; ONE WE DO NOT IS STILL REFUSED IF IT
    // COULD BE ACTIVE CONTENT.
    //
    // sniffRaster's table is deliberately narrow — it is the ACCEPT list, and
    // HEIF, AVIF and TIFF are intentionally absent from it while
    // looksLikeAvContainer lets those brands through "if an image plugin ever
    // appears". Refusing everything the table does not name would therefore
    // have blanked a HEIC on macOS, where qmacheif exists and it used to
    // render, with no diagnostic at all.
    //
    // So: a recognised format is pinned with autodetection OFF, which is what
    // keeps an unrecognised payload away from the SVG handler. An
    // unrecognised one falls back to autodetection ONLY after the markup and
    // compressed check has refused it a second time, so SVG and SVGZ cannot
    // reach a decoder either way.
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
    // An unreadable header means the ceiling below never applies.
    if (!natural.isValid())
        return {};
    if (natural.width() > 4096 || natural.height() > 4096)
        return {};
    return reader.read();
}

QString MediaBridge::mxcImageSource(const QString &mxcUri, int edge)
{
    // Non-avatar mxc images (link-preview thumbnails): honors the caller's
    // edge and uses the "mxcimg:" prefix, so these larger bitmaps live in
    // the MAIN cache class — they must never churn real avatars out of the
    // reserved avatar budget, and they render at full quality instead of
    // the 224px avatar canonical edge.
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
    // Heavy work (full static media, speculative prefetch) never takes the
    // last two slots: explicit playback and visible chrome must always find
    // immediate headroom.
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
        // Sampled where the queue GROWS. Sampling only on completion misses
        // the peak entirely — by the time a fetch finishes the queue has
        // already drained past its high-water mark, and the peak is the one
        // number the removed per-request "queued=N" line was worth keeping.
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
        // The backend could not even start the fetch — typically the
        // session is restoring/switching or the media item is not known
        // yet. That is a TRANSIENT condition: marking it permanent would
        // poison the key for the whole account session (the "room-header
        // avatar skeleton forever" failure). The normal retry window plus
        // the watchdog sweep recover it without interaction.
        ++m_statFailed;
        qCWarning(lcMedia, "fetch %s unavailable (backend returned opId=0)",
                  qUtf8Printable(keyTag(tracked.cacheKey)));
        // A save/star dispatch failure is reported ONLY through its own
        // signal, never through mediaFetchFailed(cacheKey) — a save/star
        // request shares its cacheKey ("full:<mediaKey>") with the ORDINARY
        // fetch for the same media (e.g. an inline GIF preview already on
        // screen), so an unguarded mediaFetchFailed here would tell that
        // unrelated, still-healthy consumer its OWN fetch failed. Mirrors
        // markFailed()'s own save/star exemption below.
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
        // Best eligible entry: lowest priority value, FIFO within a class.
        // Heavy entries are ineligible while the heavy slots are full.
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
        // Bounded starvation: an entry that has waited past the guard
        // dispatches ahead of higher-priority newcomers, so a constant
        // stream of chrome fetches can delay speculative work but never
        // park it forever.
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
    // Active failure-mark expiry rides the same tick: without it, an
    // avatar whose key was failure-marked when its ONLY consumer called
    // avatarSource() has no recovery channel once the app quiesces (QML
    // does not repoll on its own — the old "click to make avatars appear"
    // behaviour was new Avatar instances passively expiring marks).
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
            // Transient category: expires like a network failure, so QML
            // surfaces a fallback immediately and re-dispatches once the
            // interval elapses — never an indefinite loading state. A late
            // real completion for this op is now a stale/foreign no-op.
            dropInterestSets(request.cacheKey);
            markFailed(request, QStringLiteral("timeout"));
            Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("timeout"));
        }
    }
    // Reclaimed slots let queued work proceed — the essential recovery.
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
    // Playable payloads materialize on a worker thread; a count that never
    // drains is the signature of a wedged write.
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
        // stale (cleared on sign-out, or reclaimed by the watchdog) or a
        // foreign op — suppressed so a late completion can never repopulate
        // the next account's cache. QML re-dispatches once the transient
        // timeout mark expires.
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
        // Same "export, not cache" treatment as Save As — never inserted
        // into the shared RAM cache, GIF-specific validation happens
        // downstream (GifStarredStore), never here.
        Q_EMIT mediaBytesForStar(request.mediaKey, true, bytes, QString());
        return;
    }
    // MARKUP IS REFUSED ON EVERY CLASS, and it used to be refused only on the
    // thumbnail ones. §6 says untrusted SVG is never rendered as active
    // content, and this sniff is the choke point that enforces it — but it
    // sat behind `kind == 1 || kind == 2`, so the whole `full:` class walked
    // past it into insertCache() and then into QImageReader. Three live ways
    // in: an image row takes the "full" branch whenever the SENDER simply
    // omits info.thumbnail_url, the full-screen viewer always asks for
    // "full", and wideImageSource fetches profile and Space banners as
    // kind 0. `stickers.rs` even cites this sniff as the reason it may allow
    // an absent mimetype through; that argument only held for two thirds of
    // the paths.
    //
    // Safe to apply everywhere: every raster format this client accepts opens
    // with binary magic, and so does every A/V container, so nothing
    // legitimate on these paths begins with `<` (after BOM and whitespace) or
    // with gzip. Save As and the star export return ABOVE this point, so a
    // user downloading an .svg or a .tar.gz attachment is unaffected — those
    // never reach a decoder.
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
    // Thumbnail-class results must be images. A homeserver that cannot
    // thumbnail may return the ORIGINAL payload (and the Rust bridge labels
    // thumbnail results with the parent's mimetype regardless — a video's
    // "thumb" arrives tagged video/mp4), so the BYTES decide: a payload
    // that sniffs as an A/V container never enters the image cache or the
    // image-decode path. Permanent category — the server will keep
    // answering the same way.
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
    // 2026-08-20: READ, not consumed. Materialization is a second phase
    // now, running on the worker thread, and a card that closes during it
    // must still be able to cancel — which cancelPlayable can only do
    // while the refcounted interest still exists. Every path below that
    // does NOT start a write drops both entries itself, and the write
    // completion drops them when it does.
    const bool playableWanted = m_playableWanted.contains(request.cacheKey);
    const bool prefetchWanted = m_prefetchWanted.contains(request.cacheKey);
    // Remember the real payload size of A/V media (sniffed from bytes, not
    // trusted labels): metadata-less events can then prefetch — and so
    // poster — on every later session after one fetch.
    if (request.kind == 0 && looksLikeAvContainer(bytes))
        Q_EMIT playableSizeLearned(request.mediaKey,
                                   static_cast<qint64>(bytes.size()));
    // v0.7: large playable payloads live on disk for the in-process player;
    // pushing them through the RAM LRU would evict the entire image cache
    // for one video. Smaller payloads (thumbnails, images, short audio)
    // keep the existing in-memory path.
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
        if (!writeAnimatedFile(request.cacheKey, bytes, mimetype).isEmpty())
            Q_EMIT animatedMediaReady(request.cacheKey);
        else
            Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("invalid_gif"));
    }
    if (playableWanted || prefetchWanted) {
        // The write runs on the worker thread; playableMediaReady, the
        // multimedia warm-up and the poster hook all fire from its
        // completion (onPlayableWriteFinished). Only a REFUSAL — an
        // unknown container or an over-bound payload, decided before any
        // file is created — is terminal here, exactly as before.
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
    // Coalescing, the documented "keyed dedup must service all claimants"
    // rule (a star and a copy racing on one image once stranded the star
    // forever): a second caller for the same key never starts a second
    // write. The one completion broadcasts playableMediaReady, which every
    // claimant already listens for, and the failure obligation is the OR of
    // the claimants' — an explicit Play joining a speculative prefetch must
    // still get its terminal answer.
    const auto existing = m_playableWriting.find(cacheKey);
    if (existing != m_playableWriting.end()) {
        existing->notifyFailure = existing->notifyFailure || notifyFailure;
        return true;
    }
    // Everything below runs BEFORE any byte is handed to the worker, so a
    // refusal is guaranteed to have created no file.
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
    // Session isolation keyed on the tracking hash rather than on the
    // connection (see m_playableWriting's comment) AND on the generation
    // token, so a completion belonging to a previous account publishes
    // nothing even if the next session happens to want the same key. The
    // serial disambiguates a cancelled job from the fresh one that
    // replaced it under the same key.
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
        // QSaveFile discarded its temporary file, so nothing exists at
        // `path`. No failure MARK is set — a write failure is a local disk
        // condition, not a verdict on the payload, and the old synchronous
        // path did not mark one either; the consumer may retry.
        m_posterWanted.remove(cacheKey);
        m_playableWanted.remove(cacheKey);
        m_prefetchWanted.remove(cacheKey);
        if (pending.notifyFailure)
            Q_EMIT mediaFetchFailed(cacheKey, QStringLiteral("rejected"));
        return;
    }
    // The remaining GUI-thread disk work is unlinking evicted files; keep
    // the stall attribution pointed at it, so a stall still logged under
    // this category means eviction, never the payload write.
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
        // Least-recent UNPINNED victim: a live player's open file is never
        // deleted under it. When everything else is pinned the cap is
        // temporarily exceeded — bounded by the number of live players.
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
    // Refcounted (review L1): two cards can pin the same event's file.
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
        // review L3: a playableSource() caller may have coalesced onto this
        // queued entry ("full:<key>" is shared by the animated and playable
        // paths) — dropping it then would strand that caller with no fetch,
        // no terminal signal. Such an entry is no longer purely speculative;
        // keep it.
        if (p.priority == 3 && !p.saveRequest && !p.starRequest
            && !m_playableWanted.contains(p.cacheKey)) {
            m_animatedWanted.remove(p.cacheKey);
            // Speculative playable prefetches (and their poster hooks) are
            // exactly as irrelevant after a room switch as GIF prefetches.
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
    // review L4: alreadyPending() suppresses a second request for the same
    // key outright, so an explicit/interactive caller landing on an entry
    // queued by a speculative one would otherwise inherit the speculative
    // class and wait behind chrome. Lower the queued entry's priority in
    // place (its enqueuedAtMs — and so its starvation age — is preserved)
    // and widen its timeout class upward so a playable caller's longer
    // Rust/watchdog budget applies.
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
    // 0600 BEFORE the bytes are written, like PlayableFileWriter and the
    // starred-GIF store. These are DECRYPTED payloads from an encrypted room;
    // the containing directory is 0700, but the file itself was inheriting
    // the umask, so this was the one materialization path that relied on the
    // directory alone.
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
    // Terminal outcome for this key: every interest class is void. The
    // consumers were told (mediaFetchFailed / their own signals) and a
    // retry re-expresses interest from scratch.
    m_playableWanted.remove(cacheKey);
    m_animatedWanted.remove(cacheKey);
    m_prefetchWanted.remove(cacheKey);
    m_posterWanted.remove(cacheKey);
}

void MediaBridge::markFailed(const Pending &request, const QString &category)
{
    if (request.saveRequest || request.starRequest)
        return; // Save/star report through their own signal, not source state.
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
    // A failure keeps its own line in the default category: it is rare, it
    // names a category the user can act on, and burying it in a count would
    // make a broken avatar indistinguishable from a slow one.
    qCWarning(lcMedia, "failed %s category=%s",
              qUtf8Printable(keyTag(request.cacheKey)),
              qUtf8Printable(category));
    dropInterestSets(request.cacheKey);
    markFailed(request, category);
    Q_EMIT mediaFetchFailed(request.cacheKey, category);
}

QString MediaBridge::sanitizedFileName(const QString &name)
{
    // A LEAF, and nothing that can act like a path. The name comes from the
    // SENDER of an attachment, so it is chosen by someone else entirely.
    // QFileInfo::fileName() alone was not enough: it strips a native path but
    // leaves the foreign separator (a Windows-style `..\..\x` is one leaf on
    // Unix), and it happily returns a name that begins with a dot or is a
    // Windows reserved device.
    // BOTH separators are separators, and the LAST component is the name.
    // QFileInfo::fileName() only knows the native one, so on Unix a
    // Windows-style `..\..\evil.exe` arrives as a single leaf. Splitting on
    // both and taking the last non-empty part gives the name the sender
    // actually meant, rather than a mangled `_.._evil.exe`, and it cannot
    // traverse because only one component survives.
    QString out = QFileInfo(name).fileName();
    const QStringList parts = out.split(QRegularExpression(
        QStringLiteral("[\\\\/]")), Qt::SkipEmptyParts);
    if (!parts.isEmpty())
        out = parts.last();
    // Control characters, including the NUL that used to be handled alone.
    for (QChar &c : out) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QLatin1Char('_');
    }
    // `..` traverses. A SINGLE leading dot does not, and this function also
    // runs over the name the USER typed into the save dialog, where
    // `.hidden.png` is a deliberate choice — stripping every leading dot
    // rewrote their filename. Only the traversal spellings are refused.
    while (out == QLatin1String("..") || out.startsWith(QLatin1String("../"))
           || out.startsWith(QLatin1String("..\\")))
        out.remove(0, 2);
    out = out.trimmed();
    // Windows reserved device names, which are refused whatever the suffix.
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
    // Bounded: some filesystems cap a component at 255 bytes. The SUFFIX is
    // preserved, because this also truncates a user-typed name and cutting
    // ".png" off the end changes what the file is.
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
    // For a save dialog's default. The QML used to seed
    // `currentFile: "file:///" + <sender-chosen name>`, which puts an
    // attacker-chosen string into the path the dialog opens on. Empty means
    // "no suggestion" so the caller can let the dialog choose.
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
    // Serve from cache when the full payload is already in memory.
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
    // Serve from cache when the full payload is already in memory — the
    // common case: a GIF already rendered inline (animatedSource()) already
    // fetched these exact bytes.
    const QByteArray cached = cachedBytes(mediaCacheKey(mediaKey, 0));
    if (!cached.isEmpty()) {
        Q_EMIT mediaBytesForStar(mediaKey, true, cached, QString());
        return;
    }
    // Dedup: a rapid double-activation of the hover star on the same row
    // (e.g. two taps landing inside the platform's double-click window)
    // must not dispatch a second identical fetch — the second call is
    // dropped silently; the first, already in flight, will resolve both.
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
        // review L1: deliberately does NOT call touch() the way
        // cachedBytes() does — this is a read-only "is this starred?"
        // predicate fired from several QML triggers per row, not an actual
        // display fetch, and it must never reorder LRU eviction ahead of a
        // genuine read.
        rev = m_revision.value(cacheKey);
        const auto memoized = m_contentHashCache.constFind(cacheKey);
        if (memoized != m_contentHashCache.constEnd() && memoized->revision == rev)
            return memoized->hex;
        bytes = it.value();
    }

    // review L-a: hash OUTSIDE the lock. m_cacheMutex is shared with
    // MediaImageProvider::requestImage, which runs on Qt's pixmap-reader
    // thread; SHA-256 measures ~149 MB/s here, so hashing a multi-MiB
    // payload under the lock would stall an unrelated image load for tens
    // of milliseconds. The COW copy above makes releasing the lock free,
    // and the bytes stay valid even if the entry is evicted meanwhile.
    const QString hex = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    QMutexLocker lock(&m_cacheMutex);
    // Only memoize if nothing replaced or dropped the payload while we
    // hashed — otherwise this digest describes bytes that are no longer
    // under this key, and caching it would serve a stale answer. Identity
    // of the shared buffer, NOT the revision counter: clear() resets
    // m_revision, so a key at revision 1 before a clear() is at revision 1
    // again after re-insertion — an ABA the counter cannot see. Under COW
    // the data pointer is exact and free to compare. (Unreachable today —
    // cachedFullContentHash, clear() and insertCache all run on the GUI
    // thread — but moving the hash to a worker thread, which is the
    // obvious next optimization, would make it live.)
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
    // THE DIRECTORY IS RESOLVED FIRST, THEN THE LEAF IS REATTACHED.
    //
    // The old comment claimed "a hostile attachment name can never traverse
    // out of it", and that did not hold: `chosen.dir()` is derived from the
    // WHOLE destination, so any `../` in the name had already been absorbed
    // into the directory before the leaf was sanitized. Sanitizing the leaf
    // after the damage is done protects nothing.
    //
    // Now the parent is canonicalized on its own and must be an existing
    // directory, and the sanitized leaf is joined to THAT. A name that tried
    // to traverse lands in the directory the dialog reported, under a
    // harmless leaf, instead of somewhere else entirely.
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
        // Account isolation + bounded memory: revisions restart with the
        // session (a fresh session's first insert is revision 1 again).
        m_revision.clear();
        // review H1b: every memoized digest refers to bytes that no longer
        // exist past this point (sign-out/account switch) — drop them all.
        m_contentHashCache.clear();
    }
    m_inflight.clear();
    m_queue.clear();
    m_failed.clear();
    m_animatedFiles.clear();
    m_animatedSizes.clear();
    m_animatedLru.clear();
    m_animatedWanted.clear();
    m_playableFiles.clear();
    m_playableSizes.clear();
    m_playableLru.clear();
    m_playableWanted.clear();
    m_prefetchWanted.clear();
    m_posterWanted.clear();
    m_posterExtracting.clear();
    // Session isolation (review H2): an extraction still decoding must not
    // deliver a poster derived from the PREVIOUS account's decrypted video
    // into the next session's cache. Disconnect first, then let the
    // extractor die with its decoder; the next request lazily recreates it.
    // m_posterExtracting was cleared just above, which is what actually
    // makes a late completion inert — see onPosterReady. Deleting the
    // extractor also joins its worker thread, so no decoder outlives the
    // account whose file it was reading.
    if (m_posterExtractor) {
        disconnect(m_posterExtractor, nullptr, this, nullptr);
        // ...and it gives up its worker thread WITHOUT waiting for it. The
        // destructor's join is bounded by ~931 ms of Qt Multimedia backend
        // initialisation, and this runs on an ACCOUNT SWITCH — on the GUI
        // thread, during exactly the operation that must not block. The
        // decoder is still torn down on its own thread; nothing about the
        // isolation above changes.
        m_posterExtractor->retireWithoutWaiting();
        m_posterExtractor->deleteLater();
        m_posterExtractor = nullptr;
    }
    m_pinnedPlayables.clear(); // the files the pins protected are gone too
    // Session isolation for the write path. Clearing the tracking hash is
    // what actually makes a late completion inert (the same rule as
    // m_posterExtracting above); the generation bump is the independent
    // second guard, and cancelAll() REQUESTS cancellation of a write that is
    // still running.
    //
    // Be precise about that last one, because the difference matters for the
    // security claim: cancellation is observed between chunks, so a write
    // already inside its final chunk can still complete and leave bytes on
    // disk for a moment. What guarantees the previous account's decrypted
    // payload does not SURVIVE is the m_animatedDir reset below — the
    // QTemporaryDir destructor removes the directory recursively, including
    // anything a racing write just finished. Saying cancelAll() prevents the
    // bytes from ever landing would be a comment asserting an invariant the
    // code does not hold, which in this codebase becomes the next round's
    // evidence base.
    // Unlike the poster extractor the writer is KEPT: it holds no decoder
    // and no reference to the account's data, only a thread, so recreating
    // it per account switch would buy nothing.
    m_playableWriting.clear();
    ++m_sessionGeneration;
    if (m_playableWriter)
        m_playableWriter->cancelAll();
    m_playableNameSalt.clear(); // next session gets fresh unguessable names
    // Release the live mark BEFORE the directory goes, or the held lock
    // outlives the directory it names — this runs on every sign-out and
    // account switch, so it is once per switch for the life of the process.
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
    // Decrypted media must not outlive the session in memory, and no stale
    // completion may repopulate the cache for the next account.
    clear();
}
