#include "media/PlayableFileWriter.h"

#include <QFileDevice>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QSaveFile>
#include <QThread>

#include <algorithm>

// Counts and outcomes only. The path is a salted hash inside a session
// temp directory and the payload is decrypted media; neither is ever
// logged here.
Q_LOGGING_CATEGORY(lcPlayableWrite, "lightning.media.write")

PlayableFileWriter::PlayableFileWriter(QObject *parent)
    : QObject(parent)
    , m_control(std::make_shared<PlayableWriteControl>())
    , m_thread(new QThread)
    , m_worker(new PlayableWriteWorker(m_control))
{
    // Created here and moved; it owns no timers or children that could keep the
    // old thread affinity.
    m_thread->setObjectName(QStringLiteral("lightning-playable-write"));
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &PlayableWriteWorker::writeFinished,
            this, &PlayableFileWriter::writeFinished);
    m_thread->start();
}

PlayableFileWriter::~PlayableFileWriter()
{
    // Cancel first so the wait is bounded by one chunk.
    cancelAll();
    m_thread->quit();
    m_thread->wait();
    delete m_thread;
}

quint64 PlayableFileWriter::write(const QString &cacheKey, const QString &path,
                                  const QByteArray &bytes, quint64 generation)
{
    if (cacheKey.isEmpty() || path.isEmpty() || bytes.isEmpty())
        return 0;
    quint64 serial = 0;
    {
        QMutexLocker locker(&m_control->mutex);
        if (m_control->live.contains(cacheKey))
            return 0; // one write per key; the caller coalesces claimants
        serial = m_control->nextSerial++;
        m_control->live.insert(cacheKey, serial);
    }
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, cacheKey, path,
                                         bytes, generation, serial] {
        worker->enqueue(cacheKey, path, bytes, generation, serial);
    });
    return serial;
}

void PlayableFileWriter::cancel(const QString &cacheKey)
{
    QMutexLocker locker(&m_control->mutex);
    const auto it = m_control->live.find(cacheKey);
    if (it == m_control->live.end())
        return;
    m_control->cancelled.insert(it.value());
    m_control->live.erase(it);
}

void PlayableFileWriter::cancelAll()
{
    QMutexLocker locker(&m_control->mutex);
    for (auto it = m_control->live.cbegin(), end = m_control->live.cend();
         it != end; ++it)
        m_control->cancelled.insert(it.value());
    m_control->live.clear();
}

PlayableWriteWorker::PlayableWriteWorker(
    std::shared_ptr<PlayableWriteControl> control, QObject *parent)
    : QObject(parent)
    , m_control(std::move(control))
{
}

void PlayableWriteWorker::enqueue(const QString &cacheKey, const QString &path,
                                  const QByteArray &bytes, quint64 generation,
                                  quint64 serial)
{
    if (cancelled(serial)) {
        // Cancelled before it started; nothing was created.
        retire(cacheKey, serial);
        return;
    }

    bool ok = false;
    bool abandoned = false;
    {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            // Owner-only before any byte is written; QSaveFile otherwise
            // honours the umask. A second layer behind the 0700 session
            // directory.
            file.setPermissions(QFileDevice::ReadOwner
                                | QFileDevice::WriteOwner);
            const qint64 total = bytes.size();
            qint64 written = 0;
            bool failed = false;
            while (written < total) {
                if (cancelled(serial)) {
                    abandoned = true;
                    break;
                }
                const qint64 chunk =
                    std::min<qint64>(PlayableFileWriter::kChunkBytes,
                                     total - written);
                const qint64 n =
                    file.write(bytes.constData() + written, chunk);
                if (n != chunk) {
                    failed = true;
                    break;
                }
                written += n;
            }
            if (!abandoned && !failed) {
                // commit() discards its own temporary file when it fails,
                // so a failed commit leaves nothing behind either.
                ok = file.commit();
            } else {
                // Partial write: discard rather than commit a truncated payload
                // that would sniff as valid and play as corrupt.
                file.cancelWriting();
            }
        }
    }

    // Retire before emitting, so a re-request from the queued completion is not
    // refused by a stale mapping.
    retire(cacheKey, serial);
    if (abandoned) {
        qCDebug(lcPlayableWrite, "playable write abandoned (cancelled)");
        return; // the caller stopped caring; no completion is owed
    }
    if (!ok)
        qCWarning(lcPlayableWrite, "playable write failed (%lld bytes)",
                  static_cast<long long>(bytes.size()));
    Q_EMIT writeFinished(serial, cacheKey, path, generation, ok);
}

bool PlayableWriteWorker::cancelled(quint64 serial) const
{
    QMutexLocker locker(&m_control->mutex);
    return m_control->cancelled.contains(serial);
}

void PlayableWriteWorker::retire(const QString &cacheKey, quint64 serial)
{
    QMutexLocker locker(&m_control->mutex);
    m_control->cancelled.remove(serial);
    // Only if the mapping still names this job; a newer one may have claimed
    // the key.
    if (m_control->live.value(cacheKey) == serial)
        m_control->live.remove(cacheKey);
}
