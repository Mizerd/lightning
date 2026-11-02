#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <memory>

class QMediaPlayer;
class QThread;
class QTimer;
class QVideoSink;
class VideoPosterWorker;

class VideoPosterExtractor : public QObject
{
    Q_OBJECT

public:
    explicit VideoPosterExtractor(QObject *parent = nullptr);
    ~VideoPosterExtractor() override;

    void retireWithoutWaiting();

    void requestPoster(const QString &tag, const QString &filePath);

    void warmUp();

    static constexpr int kMaxEdge = 640;
    static constexpr int kJpegQuality = 85;

Q_SIGNALS:
    void posterReady(const QString &tag, const QByteArray &jpeg,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

private:
    QThread *m_thread = nullptr;
    VideoPosterWorker *m_worker = nullptr;
};

// The decoding half, living on the worker thread. Declared here only because
// the build has no per-translation-unit moc; internal to VideoPosterExtractor.
class VideoPosterWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoPosterWorker(QObject *parent = nullptr);
    ~VideoPosterWorker() override;

public Q_SLOTS:
    void enqueue(const QString &tag, const QString &filePath);
    void warmUp();

Q_SIGNALS:
    void posterReady(const QString &tag, const QByteArray &jpeg,
                     const QSize &posterSize, const QSize &sourceSize,
                     qint64 durationMs);

private:
    void startNext();
    void finishActive(const QByteArray &jpeg, const QSize &posterSize,
                      const QSize &sourceSize, qint64 durationMs);
    void finishWithBestAvailable();
    void teardownPlayer();

    struct Job {
        QString tag;
        QString path;
    };
    QList<Job> m_queue;
    bool m_active = false;
    QString m_activeTag;
    bool m_frameSeen = false;
    int m_skippedFrames = 0;
    QImage m_fallbackFrame;
    QImage m_bestFrame;
    qint64 m_durationMs = 0;
    std::unique_ptr<QMediaPlayer> m_player;
    std::unique_ptr<QVideoSink> m_sink;
    // A child, so moveToThread() carries it along; a plain member keeps the old
    // affinity and cannot be started, silently disabling the timeout.
    QTimer *m_timeout = nullptr;
    bool m_warmed = false;
};
