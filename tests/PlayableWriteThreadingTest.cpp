// 2026-08-20 (contract C8): materializing a playable payload must not run
// on the thread that asks for it.
//
// MediaBridge::writePlayableFile() wrote the whole decrypted video/audio
// payload with a synchronous QSaveFile, bounded by m_playableMaxBytes —
// the 256 MiB playable budget, not the 32 MiB speculative prefetch cap. Its
// own comment admitted the cost ("Known residual synchronous cost: up to
// 32 MiB written on the GUI thread") and the stalltrace::Scope that wrapped
// it existed only to attribute the freeze it caused. This is the same shape
// of defect the video poster round measured at 937 ms, and it is fixed the
// same way: a private worker thread.
//
// These cases pin the properties that fix depends on, none of which a
// source scan can see: the call returns before the bytes are on disk, the
// completion still arrives on the caller's thread, a heartbeat on the
// caller's thread keeps ticking through a large write, a cancelled job
// leaves NOTHING behind (not even QSaveFile's temporary), and two writes
// for one key collapse into one.
//
// Every case measures its own SYNCHRONOUS baseline with the same payload,
// on the same filesystem, in the same run — the number the old code paid.
// Hardcoding a baseline would make these assertions dishonest on a disk
// slower or faster than the one they were written on.

#include "media/PlayableFileWriter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
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

// Large enough that a synchronous write is unmistakably a stall, and well
// past the 32 MiB speculative cap so it represents a real pressed-play
// video rather than a best case.
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

} // namespace

class PlayableWriteThreadingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_payload = makePayload();
        QCOMPARE(static_cast<qint64>(m_payload.size()), kPayloadBytes);

        // The baseline: exactly what MediaBridge used to do, on this
        // thread, with this payload, on this filesystem. Measured once and
        // reported by every case below.
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

    // The whole point. Handing a QByteArray to another thread is a
    // refcount bump and an event post; the old path wrote the payload
    // first and returned afterwards.
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
        // Nothing is published until the completion: the file is either
        // absent or still QSaveFile's temporary, never the final path.
        QVERIFY(!QFileInfo::exists(path));

        QVERIFY(waitForCount(finished, 1));
        QCOMPARE(finished.at(0).at(0).toULongLong(), serial);
        QCOMPARE(finished.at(0).at(1).toString(),
                 QStringLiteral("full:$clip"));
        QCOMPARE(finished.at(0).at(2).toString(), path);
        // The lifecycle token is carried through untouched — MediaBridge
        // compares it against its own session generation.
        QCOMPARE(finished.at(0).at(3).toULongLong(), quint64(7));
        QVERIFY(finished.at(0).at(4).toBool());
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(QFileInfo(path).size(), kPayloadBytes);
        // Owner-only, set before any byte was written.
        const auto perms = QFileInfo(path).permissions();
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));
    }

    // The heartbeat measurement. MediaBridge's completion handler touches
    // GUI-owned state, so the reply has to land on the caller's thread —
    // and that thread has to have kept running while the payload was
    // written.
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
        // Absolute, not a fraction of the baseline: the caller's thread is
        // supposed to be idle, so any gap here is scheduler noise. The old
        // inline write showed up as one gap of the whole baseline.
        QVERIFY2(worstGapMs < 100,
                 qPrintable(QStringLiteral("caller thread stalled %1 ms "
                                           "(baseline %2 ms)")
                                .arg(worstGapMs)
                                .arg(m_syncMs)));
        QCOMPARE(deliveredOn.load(), QThread::currentThread());
    }

    // A cancelled job must leave NOTHING — not the final file and not
    // QSaveFile's temporary — and must owe no completion: the caller
    // already dropped its own tracking entry when it cancelled, so a late
    // "done" would publish a path nobody is waiting for.
    void aCancelledWriteLeavesNothingBehindAndNoCompletion()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlayableFileWriter writer;
        QSignalSpy finished(&writer, &PlayableFileWriter::writeFinished);
        const QString busyPath = dir.filePath(QStringLiteral("busy.mp4"));
        const QString doomedPath = dir.filePath(QStringLiteral("doomed.mp4"));

        // The worker's event loop serializes jobs, so queueing a long job
        // first makes this deterministic rather than a race: the doomed
        // job provably has not started when it is cancelled.
        QVERIFY(writer.write(QStringLiteral("busy"), busyPath, m_payload, 1)
                != 0);
        QVERIFY(writer.write(QStringLiteral("doomed"), doomedPath, m_payload,
                             1) != 0);
        writer.cancel(QStringLiteral("doomed"));

        QVERIFY(waitForCount(finished, 1));
        QTest::qWait(200); // a second completion would have landed by now
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(1).toString(), QStringLiteral("busy"));
        QStringList leftovers =
            QDir(dir.path()).entryList(QDir::Files | QDir::Hidden);
        leftovers.sort();
        // Exactly the committed file: a partial write, or QSaveFile's
        // temporary, would show up as a second entry here.
        QCOMPARE(leftovers, QStringList{QStringLiteral("busy.mp4")});
        QVERIFY(!QFileInfo::exists(doomedPath));
    }

    // A cancelled key is released immediately, so the caller can re-ask
    // without waiting for the abandoned job to notice. Keying cancellation
    // on the cache key alone would kill this second job too.
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

    // Coalescing, the documented "keyed dedup must service all claimants"
    // rule: one key is one write. The single completion is what services
    // every claimant, so a second job here would mean two writes to one
    // path and two publications.
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

        // Once the job has been retired the key is free again.
        QVERIFY(writer.write(QStringLiteral("full:$shared"), path, m_payload,
                             1) != 0);
        QVERIFY(waitForCount(finished, 2));
    }

    // Destruction cancels everything outstanding, so tearing the writer
    // down (sign-out, account switch, exit) cannot block on a
    // multi-hundred-megabyte write nobody is left to receive.
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
        } // ~PlayableFileWriter: cancelAll(), then join
        const qint64 teardownMs = clock.elapsed();
        qInfo("teardown with two %lld MiB writes queued: %lld ms "
              "(synchronous baseline for ONE of them %lld ms)",
              static_cast<long long>(kPayloadBytes / (1024 * 1024)),
              static_cast<long long>(teardownMs),
              static_cast<long long>(m_syncMs));
        // Bounded by one chunk plus the join, never by the queued payload.
        QVERIFY2(teardownMs < 5000,
                 qPrintable(QStringLiteral("teardown took %1 ms")
                                .arg(teardownMs)));
        // The second job never ran; the first was abandoned mid-chunk at
        // the latest. Neither may be left half-written under its real name.
        QVERIFY(!QFileInfo::exists(pathB));
    }

private:
    QTemporaryDir m_dir;
    QByteArray m_payload;
    qint64 m_syncMs = 0;
};

QTEST_GUILESS_MAIN(PlayableWriteThreadingTest)
#include "PlayableWriteThreadingTest.moc"
