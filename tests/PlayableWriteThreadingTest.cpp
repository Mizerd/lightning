
#include "media/PlayableFileWriter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>

namespace {

constexpr qint64 kPayloadBytes = 48 * 1024 * 1024;

// Deliberately not a uniform fill: a compressing filesystem would write a
// constant buffer far faster than a real payload, deflating the very
// baseline these cases compare against.
QByteArray makePayload()
{
    QByteArray block(1024 * 1024, '\0');
    quint32 state = 0x9e3779b9u;
    for (qsizetype i = 0; i < block.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        block[i] = static_cast<char>((state >> 24) & 0xff);
    }
    QByteArray payload;
    payload.reserve(kPayloadBytes);
    while (payload.size() < kPayloadBytes)
        payload.append(block);
    payload.resize(kPayloadBytes);
    return payload;
}

bool waitForCount(QSignalSpy &spy, int expected, int timeoutMs = 30000)
{
    QElapsedTimer clock;
    clock.start();
    while (spy.count() < expected && clock.elapsed() < timeoutMs)
        spy.wait(25);
    return spy.count() >= expected;
}

}

class PlayableWriteThreadingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_payload = makePayload();
        QCOMPARE(static_cast<qint64>(m_payload.size()), kPayloadBytes);

        QElapsedTimer clock;
        clock.start();
        {
            QSaveFile control(m_dir.filePath(QStringLiteral("baseline.bin")));
            QVERIFY(control.open(QIODevice::WriteOnly));
            QCOMPARE(control.write(m_payload),
                     static_cast<qint64>(m_payload.size()));
            QVERIFY(control.commit());
        }
        m_syncMs = clock.elapsed();
        qInfo("synchronous baseline: %lld MiB written inline in %lld ms",
              static_cast<long long>(kPayloadBytes / (1024 * 1024)),
              static_cast<long long>(m_syncMs));
    }

    void writeReturnsBeforeTheBytesAreOnDisk()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlayableFileWriter writer;
        const QString path = dir.filePath(QStringLiteral("clip.mp4"));
        QSignalSpy finished(&writer, &PlayableFileWriter::writeFinished);

        QElapsedTimer clock;
        clock.start();
        const quint64 serial =
            writer.write(QStringLiteral("full:$clip"), path, m_payload, 7);
        const qint64 blockedMs = clock.elapsed();
        QVERIFY(serial != 0);
        qInfo("write() blocked the caller for %lld ms (baseline %lld ms)",
              static_cast<long long>(blockedMs),
              static_cast<long long>(m_syncMs));
        QVERIFY2(blockedMs < 25,
                 qPrintable(QStringLiteral("write() blocked the calling "
                                           "thread for %1 ms")
                                .arg(blockedMs)));
        QVERIFY(!QFileInfo::exists(path));

        QVERIFY(waitForCount(finished, 1));
        QCOMPARE(finished.at(0).at(0).toULongLong(), serial);
        QCOMPARE(finished.at(0).at(1).toString(),
                 QStringLiteral("full:$clip"));
        QCOMPARE(finished.at(0).at(2).toString(), path);
        QCOMPARE(finished.at(0).at(3).toULongLong(), quint64(7));
        QVERIFY(finished.at(0).at(4).toBool());
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(QFileInfo(path).size(), kPayloadBytes);
        const auto perms = QFileInfo(path).permissions();
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));
    }

    void theCallersThreadKeepsBeatingThroughALargeWrite()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlayableFileWriter writer;
        QSignalSpy finished(&writer, &PlayableFileWriter::writeFinished);

        std::atomic<QThread *> deliveredOn{nullptr};
        connect(&writer, &PlayableFileWriter::writeFinished, this,
                [&](quint64, const QString &, const QString &, quint64, bool) {
                    deliveredOn = QThread::currentThread();
                });

        qint64 worstGapMs = 0;
        QElapsedTimer beat;
        QTimer heartbeat;
        heartbeat.setInterval(1);
        heartbeat.setTimerType(Qt::PreciseTimer);
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            worstGapMs = std::max(worstGapMs, beat.restart());
        });
        beat.start();
        heartbeat.start();

        writer.write(QStringLiteral("full:$big"),
                     dir.filePath(QStringLiteral("big.mp4")), m_payload, 1);
        QVERIFY(waitForCount(finished, 1));
        heartbeat.stop();

        qInfo("worst caller-thread gap %lld ms during a %lld MiB write "
              "(synchronous baseline %lld ms)",
              static_cast<long long>(worstGapMs),
              static_cast<long long>(kPayloadBytes / (1024 * 1024)),
              static_cast<long long>(m_syncMs));
        QVERIFY2(worstGapMs < 100,
                 qPrintable(QStringLiteral("caller thread stalled %1 ms "
                                           "(baseline %2 ms)")
                                .arg(worstGapMs)
                                .arg(m_syncMs)));
        QCOMPARE(deliveredOn.load(), QThread::currentThread());
    }

    void aCancelledWriteLeavesNothingBehindAndNoCompletion()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlayableFileWriter writer;
        QSignalSpy finished(&writer, &PlayableFileWriter::writeFinished);
        const QString busyPath = dir.filePath(QStringLiteral("busy.mp4"));
        const QString doomedPath = dir.filePath(QStringLiteral("doomed.mp4"));

        QVERIFY(writer.write(QStringLiteral("busy"), busyPath, m_payload, 1)
                != 0);
        QVERIFY(writer.write(QStringLiteral("doomed"), doomedPath, m_payload,
                             1) != 0);
        writer.cancel(QStringLiteral("doomed"));

        QVERIFY(waitForCount(finished, 1));
        QTest::qWait(200);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("busy"));
        QStringList leftovers =
            QDir(dir.path()).entryList(QDir::Files | QDir::Hidden);
        leftovers.sort();
        QCOMPARE(leftovers, QStringList{QStringLiteral("busy.mp4")});
        QVERIFY(!QFileInfo::exists(doomedPath));
    }

    void aWriteOverAnExistingFileReplacesItAndLeavesNoTemporary()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("again.mp4"));
        {
            QFile stale(path);
            QVERIFY(stale.open(QIODevice::WriteOnly));
            QVERIFY(stale.write("an older, longer payload that must not survive") > 0);
        }
        const QByteArray bytes(64 * 1024, 'n');
        PlayableFileWriter writer;
        QSignalSpy finished(&writer, &PlayableFileWriter::writeFinished);
        QVERIFY(writer.write(QStringLiteral("again"), path, bytes, 1) != 0);
        QVERIFY(waitForCount(finished, 1));
        QVERIFY(finished.at(0).at(4).toBool());
        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(written.readAll(), bytes);
        const auto perms = QFileInfo(path).permissions();
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden),
                 QStringList{QStringLiteral("again.mp4")});
    }

    void aReRequestAfterACancelIsAccepted()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlayableFileWriter writer;
        QSignalSpy finished(&writer, &PlayableFileWriter::writeFinished);
        const QString path = dir.filePath(QStringLiteral("again.mp4"));

        const quint64 first =
            writer.write(QStringLiteral("full:$again"), path, m_payload, 1);
        writer.cancel(QStringLiteral("full:$again"));
        const quint64 second =
            writer.write(QStringLiteral("full:$again"), path, m_payload, 1);
        QVERIFY(second != 0);
        QVERIFY(second != first);

        QVERIFY(waitForCount(finished, 1));
        QTest::qWait(200);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toULongLong(), second);
        QVERIFY(finished.at(0).at(4).toBool());
        QVERIFY(QFileInfo::exists(path));
    }

    void aSecondWriteForOneKeyIsRefused()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlayableFileWriter writer;
        QSignalSpy finished(&writer, &PlayableFileWriter::writeFinished);
        const QString path = dir.filePath(QStringLiteral("shared.mp4"));

        QVERIFY(writer.write(QStringLiteral("full:$shared"), path, m_payload,
                             1) != 0);
        QCOMPARE(writer.write(QStringLiteral("full:$shared"), path, m_payload,
                              1),
                 quint64(0));
        QVERIFY(waitForCount(finished, 1));
        QTest::qWait(200);
        QCOMPARE(finished.count(), 1);

        QVERIFY(writer.write(QStringLiteral("full:$shared"), path, m_payload,
                             1) != 0);
        QVERIFY(waitForCount(finished, 2));
    }

    void destructionAbandonsOutstandingWork()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pathA = dir.filePath(QStringLiteral("a.mp4"));
        const QString pathB = dir.filePath(QStringLiteral("b.mp4"));
        QElapsedTimer clock;
        {
            PlayableFileWriter writer;
            writer.write(QStringLiteral("a"), pathA, m_payload, 1);
            writer.write(QStringLiteral("b"), pathB, m_payload, 1);
            clock.start();
        }
        const qint64 teardownMs = clock.elapsed();
        qInfo("teardown with two %lld MiB writes queued: %lld ms "
              "(synchronous baseline for ONE of them %lld ms)",
              static_cast<long long>(kPayloadBytes / (1024 * 1024)),
              static_cast<long long>(teardownMs),
              static_cast<long long>(m_syncMs));
        QVERIFY2(teardownMs < 5000,
                 qPrintable(QStringLiteral("teardown took %1 ms")
                                .arg(teardownMs)));
        QVERIFY(!QFileInfo::exists(pathB));
    }

private:
    QTemporaryDir m_dir;
    QByteArray m_payload;
    qint64 m_syncMs = 0;
};

QTEST_GUILESS_MAIN(PlayableWriteThreadingTest)
#include "PlayableWriteThreadingTest.moc"
