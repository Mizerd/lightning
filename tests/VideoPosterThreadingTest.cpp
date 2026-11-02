
#include "media/VideoPosterExtractor.h"
#include "media/ShutdownThreadJoin.h"

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
#include <QPointer>

#include <atomic>

namespace {

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

}

class VideoPosterThreadingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aBlockedMediaWorkerJoinReturnsAtItsBudgetAndDeletesAfterFinish()
    {
        class SlowWorker final : public QThread {
            void run() override { QThread::msleep(350); }
        };
        auto *worker = new SlowWorker;
        QPointer<QThread> guard(worker);
        worker->start();
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!lightning::media::quitAndJoinWorkerForShutdown(worker, 40));
        QVERIFY2(timer.elapsed() < 250,
                 "a blocked worker held the calling thread past its budget");
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 1500);
    }

    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
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
        QVERIFY2(blockedMs < 250,
                 qPrintable(QStringLiteral("requestPoster() blocked the "
                                           "calling thread for %1 ms")
                                .arg(blockedMs)));
        QSignalSpy spy(&extractor, &VideoPosterExtractor::posterReady);
        QVERIFY(spy.wait(15000));
    }

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
        QVERIFY(jpegEmpty.load());
    }

    void queuedRequestsAreServedInOrder()
    {
        VideoPosterExtractor extractor;
        QSignalSpy spy(&extractor, &VideoPosterExtractor::posterReady);
        extractor.requestPoster(QStringLiteral("first"), m_path);
        extractor.requestPoster(QStringLiteral("second"), m_path);
        extractor.requestPoster(QStringLiteral("first"), m_path);
        while (spy.count() < 2 && spy.wait(15000)) { }
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("first"));
        QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("second"));
    }

    void workerStartsItsWatchdogWithoutACrossThreadWarning()
    {
        for (const QString &message : capturedMessages()) {
            QVERIFY2(!message.contains(QStringLiteral("another thread")),
                     qPrintable(QStringLiteral("cross-thread Qt warning: %1")
                                    .arg(message)));
        }
    }

private:
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
