// v0.7.2: poster extraction must not run on the thread that asks for it.
//
// Measured cause (2026-08-17, maintainer's clip: 1254x1254, 59.94 fps,
// 3.0 s): the FIRST QVideoSink constructed in a process costs ~931 ms —
// lazy Qt Multimedia backend initialization including a hardware-decoder
// probe — and VideoPosterExtractor built it inline, on the GUI thread, the
// moment a video scrolled into view. A heartbeat timer on the GUI thread
// measured a 937 ms stall at t+0 of the request plus ~185 ms more, against
// a 1 ms idle baseline. Moving the decoder to a private worker thread took
// the worst GUI-thread stall to 1 ms.
//
// These cases pin the three properties that fix depends on, all of which
// are invisible to a source scan: the call returns immediately, the reply
// still arrives on the caller's thread, and the worker's watchdog timer is
// not silently disarmed by wrong thread affinity.

#include "media/VideoPosterExtractor.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>

namespace {

// Captured so the cross-thread timer warning ("Timers cannot be started
// from another thread") can be asserted absent. A worker whose QTimer keeps
// the creating thread's affinity still extracts posters — it just loses the
// watchdog that stops a hostile file wedging the single extraction slot,
// which no functional assertion would notice.
QtMessageHandler g_previousHandler = nullptr;
QStringList g_messages;
QMutex g_messagesMutex;

void captureMessages(QtMsgType type, const QMessageLogContext &context,
                     const QString &message)
{
    {
        QMutexLocker locker(&g_messagesMutex);
        g_messages.append(message);
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

QStringList capturedMessages()
{
    QMutexLocker locker(&g_messagesMutex);
    return g_messages;
}

} // namespace

class VideoPosterThreadingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // Any existing file drives the decoder far enough to construct the
        // sink and player; a real clip is not needed to prove where that
        // work happens, and the repository ships no video fixture.
        m_path = m_dir.filePath(QStringLiteral("not-really-a-video.mp4"));
        QFile file(m_path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(4096, '\x01')), 4096);
        file.close();
        g_previousHandler = qInstallMessageHandler(captureMessages);
    }

    void cleanupTestCase() { qInstallMessageHandler(g_previousHandler); }

    // MUST run first: it is only honest while the Qt Multimedia backend is
    // still cold in this process. Once any extraction has run, the ~931 ms
    // initialization is paid and even the unfixed code returns quickly.
    void requestPosterDoesNotBlockTheCallingThread()
    {
        VideoPosterExtractor extractor;
        QElapsedTimer timer;
        timer.start();
        extractor.requestPoster(QStringLiteral("cold-start"), m_path);
        const qint64 blockedMs = timer.elapsed();
        // Unfixed: ~931 ms (the first QVideoSink) on this very thread.
        QVERIFY2(blockedMs < 250,
                 qPrintable(QStringLiteral("requestPoster() blocked the "
                                           "calling thread for %1 ms")
                                .arg(blockedMs)));
        // Drain the job so the extraction does not outlive the test.
        QSignalSpy spy(&extractor, &VideoPosterExtractor::posterReady);
        QVERIFY(spy.wait(15000));
    }

    // Both call sites (MediaBridge's cache, AttachmentQueueModel's send
    // queue) touch objects owned by their own thread inside this slot.
    void posterReadyArrivesOnTheCallersThread()
    {
        VideoPosterExtractor extractor;
        std::atomic<QThread *> deliveredOn{nullptr};
        std::atomic<bool> jpegEmpty{false};
        connect(&extractor, &VideoPosterExtractor::posterReady, this,
                [&](const QString &, const QByteArray &jpeg, const QSize &,
                    const QSize &, qint64) {
                    deliveredOn = QThread::currentThread();
                    jpegEmpty = jpeg.isEmpty();
                });
        QSignalSpy spy(&extractor, &VideoPosterExtractor::posterReady);
        extractor.requestPoster(QStringLiteral("delivery"), m_path);
        QVERIFY(spy.wait(15000));
        QCOMPARE(deliveredOn.load(), QThread::currentThread());
        // A file that is not a video is a terminal failure, reported as an
        // empty poster rather than left to hold the extraction slot.
        QVERIFY(jpegEmpty.load());
    }

    // A queued request that arrives while a job is running must still be
    // deduplicated and served, entirely on the worker thread.
    void queuedRequestsAreServedInOrder()
    {
        VideoPosterExtractor extractor;
        QSignalSpy spy(&extractor, &VideoPosterExtractor::posterReady);
        extractor.requestPoster(QStringLiteral("first"), m_path);
        extractor.requestPoster(QStringLiteral("second"), m_path);
        extractor.requestPoster(QStringLiteral("first"), m_path); // duplicate
        while (spy.count() < 2 && spy.wait(15000)) { }
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("first"));
        QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("second"));
    }

    // The watchdog lives on the worker thread and must be startable there.
    void workerStartsItsWatchdogWithoutACrossThreadWarning()
    {
        for (const QString &message : capturedMessages()) {
            QVERIFY2(!message.contains(QStringLiteral("another thread")),
                     qPrintable(QStringLiteral("cross-thread Qt warning: %1")
                                    .arg(message)));
        }
    }

private:
    QTemporaryDir m_dir;
    QString m_path;
};

QTEST_MAIN(VideoPosterThreadingTest)
#include "VideoPosterThreadingTest.moc"
