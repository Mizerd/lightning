#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <memory>

class QThread;
class PlayableWriteWorker;

// Cancellation state shared between the caller's thread and the worker. A
// mutex rather than a queued call: cancels arrive while the worker is inside a
// write and its event loop is not running. Keyed on a job serial, so a cancel
// followed by a new request for the same key cannot kill the new job.
struct PlayableWriteControl
{
    mutable QMutex mutex;
    quint64 nextSerial = 1;
    // cacheKey -> serial of the job currently queued or running for it.
    QHash<QString, quint64> live;
    // Serials cancelled before finishing; bounded by the live job count.
    QSet<quint64> cancelled;
};

// Writes MediaBridge's playable (video/audio) payloads, up to 256 MiB, off
// the GUI thread. Same pattern as VideoPosterExtractor (private worker thread,
// reply queued back to the caller's thread), and the same two rules:
//   1. no member QTimer (it would not survive moveToThread()); a job is a
//      bounded synchronous loop that always terminates;
//   2. a queued reply can outlive disconnect(), so session isolation lives in
//      MediaBridge's pending-write map and the generation below, not in this
//      connection.
//
// MediaBridge keeps the size bound, container sniff (fails closed before any
// file exists), name derivation, LRU and publishing. This class only writes
// the given bytes to the given path.
class PlayableFileWriter : public QObject
{
    Q_OBJECT

public:
    explicit PlayableFileWriter(QObject *parent = nullptr);
    ~PlayableFileWriter() override;

    // Queues `bytes` for `path`, reported with `cacheKey` and the caller's
    // `generation`. The QByteArray is shared, not copied. Returns the job
    // serial, or 0 when a write for this key is already live (the caller
    // coalesces).
    quint64 write(const QString &cacheKey, const QString &path,
                  const QByteArray &bytes, quint64 generation);

    // Abandons the job for `cacheKey`: the partial file is discarded and no
    // completion is emitted. The key is released at once; a new job cannot
    // collide on disk because only a committing QSaveFile renames into place.
    void cancel(const QString &cacheKey);
    // Abandon every job (sign-out, account switch, exit).
    void cancelAll();

    // Small enough to notice a cancel promptly.
    static constexpr qint64 kChunkBytes = 1024 * 1024;

Q_SIGNALS:
    // On this object's thread. `ok` false means nothing exists at `path`.
    // Cancelled jobs emit nothing.
    void writeFinished(quint64 serial, const QString &cacheKey,
                       const QString &path, quint64 generation, bool ok);

private:
    std::shared_ptr<PlayableWriteControl> m_control;
    QThread *m_thread = nullptr;
    PlayableWriteWorker *m_worker = nullptr;
};

// The writing half, on the worker thread. Declared here only because the
// build has no per-translation-unit moc; internal to PlayableFileWriter.
class PlayableWriteWorker : public QObject
{
    Q_OBJECT

public:
    explicit PlayableWriteWorker(
        std::shared_ptr<PlayableWriteControl> control,
        QObject *parent = nullptr);

public Q_SLOTS:
    // Worker thread. One call is one job; the event loop serializes them.
    void enqueue(const QString &cacheKey, const QString &path,
                 const QByteArray &bytes, quint64 generation, quint64 serial);

Q_SIGNALS:
    void writeFinished(quint64 serial, const QString &cacheKey,
                       const QString &path, quint64 generation, bool ok);

private:
    bool cancelled(quint64 serial) const;
    void retire(const QString &cacheKey, quint64 serial);

    std::shared_ptr<PlayableWriteControl> m_control;
};
