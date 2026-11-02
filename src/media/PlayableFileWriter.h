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
    QHash<QString, quint64> live;
    QSet<quint64> cancelled;
};

class PlayableFileWriter : public QObject
{
    Q_OBJECT

public:
    explicit PlayableFileWriter(QObject *parent = nullptr);
    ~PlayableFileWriter() override;

    quint64 write(const QString &cacheKey, const QString &path,
                  const QByteArray &bytes, quint64 generation);

    void cancel(const QString &cacheKey);
    void cancelAll();

    static constexpr qint64 kChunkBytes = 1024 * 1024;

Q_SIGNALS:
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
