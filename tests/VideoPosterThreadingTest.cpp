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
#include <QDataStream>
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
    // AN AUDIO FILE HAS NO VIDEO TRACK, SO IT HAS NO POSTER — AND IT STILL
    // HAS A LENGTH.
    //
    // `bf3893a` shipped "an attached audio file carries its duration" and was
    // a no-op on the real path: the branch that reports an EMPTY poster
    // passed a literal 0 and threw the duration away, so an attached song
    // still went out as "0:00". The header states the contract in as many
    // words — when `jpeg` is empty, `durationMs` IS NOT invalid.
    //
    // Nothing caught it because both existing C++ cases call `applyPoster`
    // directly through the send queue's test hook, bypassing the extractor
    // entirely, and this suite's other cases ignore the duration argument.
    // That is CLAUDE.md §16's recorded lesson verbatim, so this drives the
    // real extractor against a real decodable file.
    void anAudioFileReportsItsLengthWithNoPoster()
    {
        const QString wav = writeSilentWav(
            m_dir.filePath(QStringLiteral("tone.wav")), 3000);
        QVERIFY(!wav.isEmpty());

        VideoPosterExtractor extractor;
        QSignalSpy ready(&extractor, &VideoPosterExtractor::posterReady);
        extractor.requestPoster(QStringLiteral("audio"), wav);
        QVERIFY2(ready.wait(20000), "the extractor never answered for a WAV");

        const QList<QVariant> args = ready.takeFirst();
        QVERIFY2(args.at(1).toByteArray().isEmpty(),
                 "a WAV somehow produced a poster; this case no longer tests "
                 "the empty-poster branch");
        const qint64 duration = args.at(4).toLongLong();
        QVERIFY2(duration > 0,
                 qPrintable(QStringLiteral(
                     "an audio file reported duration %1: the empty-poster "
                     "branch is discarding the value it was given, which is "
                     "what makes an attached song send as 0:00")
                     .arg(duration)));
        // Written as 3000 ms of samples. Generous bounds: decoders round.
        QVERIFY2(duration > 2500 && duration < 3500,
                 qPrintable(QStringLiteral("expected ~3000 ms, got %1")
                                .arg(duration)));
    }

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
    // A REAL, DECODABLE AUDIO FILE, written by hand.
    //
    // The repository ships no audio fixture and adding a binary one to pin a
    // duration would be its own problem. A PCM WAV needs no encoder: a
    // 44-byte canonical header plus silence, and Qt Multimedia decodes it.
    // Sizing it from the sample count is what makes the expected duration a
    // fact rather than a guess.
    static QString writeSilentWav(const QString &path, int milliseconds)
    {
        const int rate = 8000;
        const int channels = 1;
        const int bits = 16;
        const int frames = rate * milliseconds / 1000;
        const int dataBytes = frames * channels * bits / 8;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        QDataStream out(&file);
        out.setByteOrder(QDataStream::LittleEndian);
        file.write("RIFF");
        out << quint32(36 + dataBytes);
        file.write("WAVEfmt ");
        out << quint32(16) << quint16(1) << quint16(channels) << quint32(rate)
            << quint32(rate * channels * bits / 8)
            << quint16(channels * bits / 8) << quint16(bits);
        file.write("data");
        out << quint32(dataBytes);
        file.write(QByteArray(dataBytes, '\0'));
        file.close();
        return path;
    }

    QTemporaryDir m_dir;
    QString m_path;
};

QTEST_MAIN(VideoPosterThreadingTest)
#include "VideoPosterThreadingTest.moc"
