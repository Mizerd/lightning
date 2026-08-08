#include "media/MediaBridge.h"

#include "matrix/MatrixClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QMutexLocker>
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
    return (kind == 1 ? QStringLiteral("thumb:") : QStringLiteral("full:"))
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
    return false;
}
} // namespace

MediaBridge::MediaBridge(QObject *parent)
    : QObject(parent)
{
    m_failureClock.start();
    m_animatedDir = std::make_unique<QTemporaryDir>(
        QDir::tempPath() + QStringLiteral("/lightning-animated-XXXXXX"));
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
    QList<QString> &lru = isAvatarClassKey(cacheKey) ? m_avatarLru : m_lru;
    lru.removeOne(cacheKey);
    lru.prepend(cacheKey);
}

void MediaBridge::insertCache(const QString &cacheKey, const QByteArray &bytes)
{
    QMutexLocker lock(&m_cacheMutex);
    m_cache.insert(cacheKey, bytes);
    // An actual byte insert is the ONLY revision bump: cache hits keep an
    // identical provider URL (pixmap-cache dedup survives), a re-fetch
    // after eviction or replacement produces a new one.
    ++m_revision[cacheKey];
    touch(cacheKey);
    // Evict least-recently-used entries beyond the inserted key's class
    // budget. Classes are disjoint and each bounded, so an avatar insert
    // can never evict timeline media and timeline churn can never evict
    // avatars; total memory stays bounded by the sum of both caps.
    const bool avatarClass = isAvatarClassKey(cacheKey);
    QList<QString> &lru = avatarClass ? m_avatarLru : m_lru;
    const qint64 limit = avatarClass ? m_avatarCacheLimit : m_cacheLimit;
    qint64 total = 0;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it) {
        if (isAvatarClassKey(it.key()) == avatarClass)
            total += it.value().size();
    }
    while (total > limit && lru.size() > 1) {
        const QString victim = lru.takeLast();
        if (victim == cacheKey)
            continue;
        total -= m_cache.value(victim).size();
        m_cache.remove(victim);
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
        qCDebug(lcMedia, "retryable %s (transient mark expired)",
                qUtf8Printable(keyTag(cacheKey)));
        Q_EMIT mediaRetryable(cacheKey);
    }
}

QString MediaBridge::mediaSource(const QString &mediaKey, const QString &kind)
{
    if (mediaKey.isEmpty() || !supported())
        return {};
    const int kindValue = kind == QLatin1String("thumb") ? 1 : 0;
    const QString cacheKey = mediaCacheKey(mediaKey, kindValue);
    const QString cached = cachedSource(cacheKey);
    if (!cached.isEmpty()) {
        ++m_statCacheHit;
        qCDebug(lcMediaTrace, "media %s cache=hit",
                qUtf8Printable(keyTag(cacheKey)));
        return cached;
    }
    // A marked failure blocks re-dispatch (transient marks expire; see
    // failureBlocks) — QML repolling a broken source must not turn into a
    // request loop.
    if (failureBlocks(cacheKey)) {
        qCDebug(lcMedia, "media %s suppressed=failure-mark(%s)",
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
        qCDebug(lcMedia, "media %s cache=miss dispatching",
                qUtf8Printable(keyTag(cacheKey)));
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
    m_playableWanted.insert(cacheKey);
    if (failureBlocks(cacheKey))
        return {};
    const QByteArray cached = cachedBytes(cacheKey);
    if (!cached.isEmpty()) {
        // Mimetype intentionally empty: the container magic decides.
        const QString written = writePlayableFile(cacheKey, cached, {});
        if (!written.isEmpty())
            return QUrl::fromLocalFile(written).toString();
    }
    if (!alreadyPending(cacheKey)) {
        Pending request;
        request.cacheKey = cacheKey;
        request.mediaKey = mediaKey;
        request.kind = 0;
        request.timeoutClass = 1; // playable class (90s Rust / 100s watchdog)
        dispatch(request);
    }
    return {};
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
        qCDebug(lcMedia, "avatar %s edge=%d suppressed=failure-mark(%s)",
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
        qCDebug(lcMedia, "avatar %s edge=%d cache=miss dispatching",
                qUtf8Printable(keyTag(cacheKey)), edge);
        dispatch(request);
    } else {
        qCDebug(lcMedia, "avatar %s edge=%d cache=miss already-pending",
                qUtf8Printable(keyTag(cacheKey)), edge);
    }
    return {};
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
        dispatch(request);
    }
    return {};
}

void MediaBridge::dispatch(const Pending &request)
{
    if (m_inflight.size() >= kMaxConcurrent) {
        m_queue.enqueue(request);
        qCDebug(lcMedia, "queue %s (inflight=%lld queued=%lld)",
                qUtf8Printable(keyTag(request.cacheKey)),
                static_cast<long long>(m_inflight.size()),
                static_cast<long long>(m_queue.size()));
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
            markFailed(tracked, QStringLiteral("unavailable"));
            Q_EMIT mediaFetchFailed(tracked.cacheKey,
                                    QStringLiteral("unavailable"));
        }
        return;
    }
    qCDebug(lcMedia, "fetch %s opId=%llu inflight=%lld",
            qUtf8Printable(keyTag(tracked.cacheKey)),
            static_cast<unsigned long long>(opId),
            static_cast<long long>(m_inflight.size() + 1));
    m_inflight.insert(opId, tracked);
}

void MediaBridge::pump()
{
    while (!m_queue.isEmpty() && m_inflight.size() < kMaxConcurrent)
        dispatch(m_queue.dequeue());
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
    out.insert(QStringLiteral("cacheHits"), m_statCacheHit);
    out.insert(QStringLiteral("cacheMisses"), m_statCacheMiss);
    out.insert(QStringLiteral("failureMarks"),
               static_cast<qint64>(m_failed.size()));
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
    ++m_statCompleted;
    m_failed.remove(request.cacheKey);
    // v0.7: large playable payloads live on disk for the in-process player;
    // pushing them through the RAM LRU would evict the entire image cache
    // for one video. Smaller payloads (thumbnails, images, short audio)
    // keep the existing in-memory path.
    const bool playableWanted = m_playableWanted.remove(request.cacheKey);
    if (!(playableWanted && bytes.size() > kLargeCacheSkipBytes))
        insertCache(request.cacheKey, bytes);
    qCDebug(lcMedia, "ready %s bytes=%lld mime=%s in=%lldms -> mediaCached",
            qUtf8Printable(keyTag(request.cacheKey)),
            static_cast<long long>(bytes.size()),
            qUtf8Printable(mimetype.section(QLatin1Char(';'), 0, 0)),
            static_cast<long long>(elapsedMs));
    if (m_animatedWanted.remove(request.cacheKey)) {
        if (!writeAnimatedFile(request.cacheKey, bytes, mimetype).isEmpty())
            Q_EMIT animatedMediaReady(request.cacheKey);
        else
            Q_EMIT mediaFetchFailed(request.cacheKey, QStringLiteral("invalid_gif"));
    }
    if (playableWanted) {
        if (!writePlayableFile(request.cacheKey, bytes, mimetype).isEmpty())
            Q_EMIT playableMediaReady(request.cacheKey);
        else
            Q_EMIT mediaFetchFailed(request.cacheKey,
                                    QStringLiteral("rejected"));
    }
    Q_EMIT mediaCached(request.cacheKey);
}

QString MediaBridge::writePlayableFile(const QString &cacheKey,
                                       const QByteArray &bytes,
                                       const QString &mimetype)
{
    if (bytes.isEmpty() || bytes.size() > kPlayableCacheBytes
        || !m_animatedDir || !m_animatedDir->isValid())
        return {};
    const QString extension = playableExtensionFor(bytes, mimetype);
    if (extension.isEmpty())
        return {}; // unknown container — fail closed, never materialize
    if (m_playableNameSalt.isEmpty())
        m_playableNameSalt = QUuid::createUuid().toString(QUuid::Id128);
    const QString name = QString::fromLatin1(QCryptographicHash::hash(
        (m_playableNameSalt + cacheKey).toUtf8(),
        QCryptographicHash::Sha256).toHex())
        + QLatin1Char('.') + extension;
    const QString path = m_animatedDir->filePath(name);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    // Owner-only, explicitly — QSaveFile otherwise honors the umask. The
    // 0700 parent directory already blocks traversal; this is the second
    // layer for decrypted payloads.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return {};
    m_playableFiles.insert(cacheKey, path);
    m_playableSizes.insert(cacheKey, bytes.size());
    m_playableLru.removeOne(cacheKey);
    m_playableLru.prepend(cacheKey);
    qint64 total = 0;
    for (qint64 size : std::as_const(m_playableSizes))
        total += size;
    while ((total > kPlayableCacheBytes
            || m_playableFiles.size() > kPlayableCacheEntries)
           && m_playableLru.size() > 1) {
        const QString victim = m_playableLru.takeLast();
        total -= m_playableSizes.take(victim);
        QFile::remove(m_playableFiles.take(victim));
    }
    return path;
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
    qCWarning(lcMedia, "failed %s category=%s",
              qUtf8Printable(keyTag(request.cacheKey)),
              qUtf8Printable(category));
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
    dispatch(request);
}

void MediaBridge::writeSaveFile(const QUrl &destination, const QByteArray &bytes,
                                const QString &mediaKey)
{
    const QFileInfo chosen(destination.toLocalFile());
    // The user picked the directory; the file name is re-sanitized so a
    // hostile attachment name can never traverse out of it.
    const QString target =
        chosen.dir().filePath(sanitizedFileName(chosen.fileName()));
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
        // Account isolation + bounded memory: revisions restart with the
        // session (a fresh session's first insert is revision 1 again).
        m_revision.clear();
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
    m_playableNameSalt.clear(); // next session gets fresh unguessable names
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
