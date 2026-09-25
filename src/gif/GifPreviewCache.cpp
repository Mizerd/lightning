#include "gif/GifPreviewCache.h"

#include "gif/GifResponseParser.h"
#include "gif/GifTransport.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>

GifPreviewCache::GifPreviewCache(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    m_watchdog.setInterval(10 * 1000);
    connect(&m_watchdog, &QTimer::timeout, this,
            &GifPreviewCache::expireStaleFetches);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_active)
            bump();
    });
}

GifPreviewCache::~GifPreviewCache() = default;

void GifPreviewCache::setTransport(GifTransport *transport)
{
    if (m_transport == transport)
        return;
    if (m_transport)
        m_transport->disconnect(this);
    // Results for the old transport's ops can no longer arrive.
    m_inflight.clear();
    m_watchdog.stop();
    m_transport = transport;
    if (m_transport) {
        connect(m_transport, &GifTransport::downloadFinished, this,
                &GifPreviewCache::onDownloadFinished);
        connect(m_transport, &GifTransport::sessionEnded, this,
                &GifPreviewCache::clear);
    }
    pump();
}

bool GifPreviewCache::acceptableUrl(const QString &url)
{
    // The hosts and suffix a send accepts; Rust checks them again.
    return gif::isSendableGifUrlForHosts(
        url, { QStringLiteral(".giphy.com"), QStringLiteral(".klipy.com") });
}

QString GifPreviewCache::source(const QString &url, bool still)
{
    if (url.isEmpty())
        return {};
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // The demo catalogue is bundled into the binary.
    if (url.startsWith(QLatin1String("qrc:/")))
        return url;
#endif
    if (!acceptableUrl(url))
        return {};
    const auto cached = m_files.constFind(url);
    if (cached != m_files.constEnd()) {
        if (m_lru.first() != url) {
            m_lru.removeOne(url);
            m_lru.prepend(url);
        }
        return QUrl::fromLocalFile(cached->path).toString();
    }
    if (!m_active || failureBlocks(url))
        return {};
    if (isQueuedOrInflight(url)) {
        // A still asked for by a preview request that is still queued.
        if (still && m_previewQueue.removeOne(url))
            m_stillQueue.enqueue(url);
        return {};
    }
    if (still)
        m_stillQueue.enqueue(url);
    else
        m_previewQueue.enqueue(url);
    pump();
    return {};
}

void GifPreviewCache::hold(QObject *owner, const QStringList &urls)
{
    if (!owner)
        return;
    if (!m_holders.contains(owner)) {
        connect(owner, &QObject::destroyed, this, [this, owner] {
            setHeld(owner, {});
            m_holders.remove(owner);
        });
    }
    setHeld(owner, urls);
}

void GifPreviewCache::setHeld(QObject *owner, const QStringList &urls)
{
    QStringList next;
    for (const QString &url : urls) {
        if (!url.isEmpty() && !next.contains(url))
            next.append(url);
    }
    const QStringList previous = m_holders.value(owner);
    for (const QString &url : next)
        ++m_holds[url];
    for (const QString &url : previous) {
        auto it = m_holds.find(url);
        if (it == m_holds.end() || --it.value() > 0)
            continue;
        m_holds.erase(it);
        m_stillQueue.removeOne(url);
        m_previewQueue.removeOne(url);
    }
    // An owner with nothing held stays registered: its destroyed()
    // connection is still live.
    m_holders.insert(owner, next);
    evict();
}

void GifPreviewCache::dropQueued()
{
    m_stillQueue.clear();
    m_previewQueue.clear();
}

void GifPreviewCache::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (!m_active) {
        dropQueued();
        return;
    }
    // Tiles asked while closed and got nothing queued; ask again.
    bump();
}

void GifPreviewCache::expireStaleFetches()
{
    const qint64 now = m_clock.elapsed();
    for (auto it = m_inflight.begin(); it != m_inflight.end();) {
        if (now - it->startedMs < m_fetchTimeoutMs) {
            ++it;
            continue;
        }
        markFailed(it->url, false);
        it = m_inflight.erase(it);
    }
    if (m_inflight.isEmpty())
        m_watchdog.stop();
    pump();
}

void GifPreviewCache::clear()
{
    // Holds survive: the tiles still exist and ask again.
    dropQueued();
    // Late results for these ops are ignored.
    m_inflight.clear();
    m_watchdog.stop();
    m_failed.clear();
    m_files.clear();
    m_lru.clear();
    m_totalBytes = 0;
    m_dir.reset(); // removes the files
    bump();
}

bool GifPreviewCache::isQueuedOrInflight(const QString &url) const
{
    if (m_stillQueue.contains(url) || m_previewQueue.contains(url))
        return true;
    for (const Fetch &pending : m_inflight) {
        if (pending.url == url)
            return true;
    }
    return false;
}

bool GifPreviewCache::failureBlocks(const QString &url)
{
    const auto it = m_failed.constFind(url);
    if (it == m_failed.constEnd())
        return false;
    if (it->permanent || m_clock.elapsed() - it->atMs < m_retryAfterMs)
        return true;
    m_failed.erase(it);
    return false;
}

void GifPreviewCache::markFailed(const QString &url, bool permanent)
{
    if (m_failed.size() >= kMaxFailureMarks)
        m_failed.clear();
    m_failed.insert(url, { m_clock.elapsed(), permanent });
    if (!permanent && !m_retryTimer.isActive())
        m_retryTimer.start(static_cast<int>(m_retryAfterMs + 1000));
}

void GifPreviewCache::pump()
{
    while (m_transport && m_inflight.size() < kMaxConcurrent
           && (!m_stillQueue.isEmpty() || !m_previewQueue.isEmpty())) {
        const QString url = !m_stillQueue.isEmpty() ? m_stillQueue.dequeue()
                                                    : m_previewQueue.dequeue();
        const quint64 op = m_transport->download(url);
        if (op == 0) {
            markFailed(url, false);
            continue;
        }
        m_inflight.insert(op, { url, m_clock.elapsed() });
        if (!m_watchdog.isActive())
            m_watchdog.start();
    }
}

void GifPreviewCache::onDownloadFinished(quint64 opId, bool ok,
                                         const QByteArray &bytes,
                                         const QString &category)
{
    const auto it = m_inflight.find(opId);
    if (it == m_inflight.end())
        return; // a send's download, or dropped by clear()
    const QString url = it->url;
    m_inflight.erase(it);
    if (m_inflight.isEmpty())
        m_watchdog.stop();
    if (!ok) {
        // "gone": the backend had no bytes to hand over for the op.
        const bool transient = category == QLatin1String("timeout")
            || category == QLatin1String("network")
            || category == QLatin1String("gone")
            || category == QLatin1String("provider_error");
        markFailed(url, !transient);
    } else if (bytes.size() > kMaxPreviewBytes
               || !gif::validateGifBytes(bytes).ok) {
        markFailed(url, true);
    } else if (store(url, bytes).isEmpty()) {
        markFailed(url, false);
    } else {
        bump();
    }
    pump();
}

QString GifPreviewCache::store(const QString &url, const QByteArray &bytes)
{
    if (m_files.contains(url))
        return m_files.value(url).path;
    if (!m_dir) {
        m_dir = std::make_unique<QTemporaryDir>();
        if (!m_dir->isValid()) {
            m_dir.reset();
            return {};
        }
    }
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha256)
            .toHex()) + QStringLiteral(".gif");
    const QString path = m_dir->filePath(name);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return {};
    m_files.insert(url, { path, bytes.size() });
    m_totalBytes += bytes.size();
    m_lru.prepend(url);
    evict();
    return path;
}

void GifPreviewCache::evict()
{
    // Held copies are on screen and stay, so the bound can be exceeded by
    // what the grid shows. Evicting them would refetch them in a loop.
    for (qsizetype i = m_lru.size() - 1;
         i >= 0 && (m_files.size() > m_maxEntries || m_totalBytes > m_maxBytes);
         --i) {
        const QString victim = m_lru.at(i);
        if (m_holds.contains(victim))
            continue;
        m_lru.removeAt(i);
        const Entry entry = m_files.take(victim);
        m_totalBytes -= entry.bytes;
        QFile::remove(entry.path);
    }
}

void GifPreviewCache::bump()
{
    ++m_revision;
    Q_EMIT revisionChanged();
}
