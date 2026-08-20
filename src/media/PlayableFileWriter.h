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

// Cancellation state shared between the caller's thread and the worker.
//
// It has to be a mutex-guarded block rather than a queued slot call: a
// cancel arrives while the worker is INSIDE the write, where its event
// loop is not running and a queued call could not reach it until the write
// it was meant to stop had already finished.
//
// Keyed on a monotonic job serial, never on the cache key alone. A cancel
// followed by a fresh request for the SAME key must not kill the new job,
// which a key-keyed mark would silently do.
struct PlayableWriteControl
{
    mutable QMutex mutex;
    quint64 nextSerial = 1;
    // cacheKey -> serial of the job currently queued or running for it.
    QHash<QString, quint64> live;
    // Serials cancelled before they finished. The worker retires a mark
    // when its job ends, so this can never outgrow the live job count.
    QSet<quint64> cancelled;
};

// 2026-08-20: the disk half of MediaBridge's playable (video/audio)
// materialization, moved off the GUI thread.
//
// MediaBridge::writePlayableFile() wrote the whole decrypted payload with a
// synchronous QSaveFile on the thread that asked for it — bounded by
// m_playableMaxBytes, which is the 256 MiB playable budget, not the 32 MiB
// speculative prefetch cap. Its own comment admitted the cost, and the
// stalltrace::Scope("playable-write") that wrapped it existed only to
// attribute the resulting freeze.
//
// The pattern is VideoPosterExtractor's, which solved the same problem for
// poster decoding: a private worker thread, work forwarded to it, and a
// reply that arrives back on the caller's thread. Two traps that class
// documents are honoured here as well:
//   1. a member QTimer does not survive moveToThread(), so this class has
//      none — the job is a bounded synchronous loop that always terminates,
//      not a callback-driven state machine needing a watchdog. Nothing to
//      disarm, nothing to make a child of the worker.
//   2. once the reply is QUEUED, disconnect() no longer reliably cancels
//      one already posted. Session isolation therefore lives in MediaBridge,
//      keyed on the pending-write MEMBER its clear path empties (plus the
//      generation carried through below), never on this connection.
//
// What deliberately stays on the caller's thread, in MediaBridge: the size
// bound, container sniffing (so an unknown container fails closed BEFORE any
// file is created), the salted name derivation, the LRU registry and its
// eviction, and publishing the finished path. This class writes bytes it was
// handed to a path it was told, and nothing else — it sees no URLs, no keys,
// and no cache state.
//
// This object stays on its creator's thread; the threading is internal.
class PlayableFileWriter : public QObject
{
    Q_OBJECT

public:
    explicit PlayableFileWriter(QObject *parent = nullptr);
    ~PlayableFileWriter() override;

    // Queue `bytes` to be written to `path`, reported back with `cacheKey`
    // and the caller's lifecycle `generation`. QByteArray is implicitly
    // shared and never modified here, so nothing is copied across the
    // thread boundary. Returns the job serial, or 0 when the request was
    // refused because a write for this key is already live (the caller
    // coalesces claimants; one write services them all).
    quint64 write(const QString &cacheKey, const QString &path,
                  const QByteArray &bytes, quint64 generation);

    // Abandon the queued or in-flight write for `cacheKey`. A partial file
    // is discarded, never committed, and NO completion is emitted for it —
    // the caller cancelled, so it is owed no answer. The key is released
    // immediately so a fresh request is accepted without waiting for the
    // abandoned job to notice; the two cannot collide on disk, because
    // QSaveFile writes to its own temporary name and only a committing job
    // ever renames it into place.
    void cancel(const QString &cacheKey);
    // Abandon every job (sign-out, account switch, exit).
    void cancelAll();

    // Bytes per write() call. Small enough that a cancel is observed
    // promptly, large enough that the syscall count stays trivial.
    static constexpr qint64 kChunkBytes = 1024 * 1024;

Q_SIGNALS:
    // Emitted on this object's thread. `ok` false means the write failed
    // and NOTHING exists at `path`. A cancelled job emits nothing at all.
    void writeFinished(quint64 serial, const QString &cacheKey,
                       const QString &path, quint64 generation, bool ok);

private:
    std::shared_ptr<PlayableWriteControl> m_control;
    QThread *m_thread = nullptr;
    PlayableWriteWorker *m_worker = nullptr;
};

// The writing half, which lives on PlayableFileWriter's worker thread.
// Declared here only because the project builds no per-translation-unit
// moc; nothing outside PlayableFileWriter should construct or call it.
class PlayableWriteWorker : public QObject
{
    Q_OBJECT

public:
    explicit PlayableWriteWorker(
        std::shared_ptr<PlayableWriteControl> control,
        QObject *parent = nullptr);

public Q_SLOTS:
    // Runs on the worker thread (queued from PlayableFileWriter::write).
    // One call is one job: the worker's own event loop serializes them, so
    // there is no queue to own and no dedup to duplicate.
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
