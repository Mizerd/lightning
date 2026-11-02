#include "media/PlayableFileWriter.h"
#include "storage/ScratchFile.h"
#include "media/ShutdownThreadJoin.h"

#include <QFileDevice>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>

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
    cancelAll();
    if (!lightning::media::quitAndJoinWorkerForShutdown(m_thread, 2000)) {
        qCWarning(lcPlayableWrite)
            << "quit: playable writer exceeded 2000 ms; detaching thread";
        return;
    }
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
            return 0;
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
        retire(cacheKey, serial);
        return;
    }

    bool ok = false;
    bool abandoned = false;
    {
        lightning::scratch::ScratchFile file(path);
        if (file.open()) {
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
                ok = file.commit();
            } else {
                file.cancelWriting();
            }
        }
    }

    retire(cacheKey, serial);
    if (abandoned) {
        qCDebug(lcPlayableWrite, "playable write abandoned (cancelled)");
        return;
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
    if (m_control->live.value(cacheKey) == serial)
        m_control->live.remove(cacheKey);
}
