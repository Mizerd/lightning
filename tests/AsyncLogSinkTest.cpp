// AsyncLogSink: the --log-file writer must never block the thread that logs.
//
// A stalled log pipe used to silence call audio both ways, because the
// handler wrote and flushed the file on the calling thread and GStreamer
// streaming threads log. Every case drives a device whose writeData() blocks
// on a gate the test holds, and every call that could block runs on a helper
// thread with a bound, so a regression FAILS instead of hanging the suite.
#include <QtTest/QtTest>

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QIODevice>
#include <QList>
#include <QSemaphore>
#include <QThread>
#include <QTimeZone>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "app/AsyncLogSink.h"

using lightning::logging::AsyncLogSink;

namespace {

// What the device saw, shared so it outlives a device a detached writer keeps.
struct Tap {
    std::mutex mutex;
    QByteArray data;
    std::atomic<bool> gated{false};
    QSemaphore gate;      // one permit per write while gated
    QSemaphore entered;   // released as each write starts
    std::atomic<int> writes{0};   // writes that got past the gate
    std::function<void()> onWrite;   // runs inside writeData(), on the writer
};

class GatedDevice : public QIODevice
{
public:
    explicit GatedDevice(std::shared_ptr<Tap> tap) : m_tap(std::move(tap))
    {
        open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }

    bool isSequential() const override { return true; }   // like a pipe

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 size) override
    {
        m_tap->entered.release();
        if (m_tap->gated.load())
            m_tap->gate.acquire();
        if (m_tap->onWrite)
            m_tap->onWrite();
        {
            std::lock_guard<std::mutex> lock(m_tap->mutex);
            m_tap->data.append(data, size);
        }
        ++m_tap->writes;
        return size;
    }

private:
    std::shared_ptr<Tap> m_tap;
};

// Owns the sink and every helper thread. Tear-down opens the gate first, so
// a thread stuck in the device (the regression under test) can finish and be
// joined before the sink goes.
struct Fixture {
    std::shared_ptr<Tap> tap = std::make_shared<Tap>();
    std::unique_ptr<AsyncLogSink> sink;
    std::vector<std::thread> threads;

    explicit Fixture(bool gated, AsyncLogSink::Limits limits = {})
    {
        tap->gated.store(gated);
        sink = std::make_unique<AsyncLogSink>(
            std::make_unique<GatedDevice>(tap), limits);
    }
    ~Fixture()
    {
        openGate();
        for (std::thread &t : threads) {
            if (t.joinable())
                t.join();
        }
        sink.reset();
    }

    void openGate()
    {
        tap->gated.store(false);
        tap->gate.release(1 << 20);
    }

    // Runs fn on a helper thread; true when it returned within boundMs.
    bool call(int boundMs, std::function<void()> fn, qint64 *tookMs = nullptr)
    {
        auto done = std::make_shared<QSemaphore>();
        auto took = std::make_shared<std::atomic<qint64>>(-1);
        threads.emplace_back([fn = std::move(fn), done, took] {
            QElapsedTimer timer;
            timer.start();
            fn();
            took->store(timer.elapsed());
            done->release();
        });
        const bool returned = done->tryAcquire(1, boundMs);
        if (tookMs)
            *tookMs = took->load();
        return returned;
    }

    // Logs `text` and waits until the writer is inside the device with it,
    // where the closed gate keeps it.
    bool holdTheWriterIn(const QString &text)
    {
        const bool returned = call(2000, [this, text] {
            sink->log(QtInfoMsg, "test", text);
        });
        return tap->entered.tryAcquire(1, 5000) && returned;
    }

    QList<QByteArray> lines()
    {
        QByteArray copy;
        {
            std::lock_guard<std::mutex> lock(tap->mutex);
            copy = tap->data;
        }
        QList<QByteArray> out = copy.split('\n');
        if (!out.isEmpty() && out.last().isEmpty())
            out.removeLast();
        return out;
    }
};

// The message after "<stamp> <level> <category>: ".
QByteArray messageOf(const QByteArray &line)
{
    const int at = line.indexOf(": ");
    return at < 0 ? line : line.mid(at + 2);
}

QByteArray numbered(const char *prefix, int i)
{
    return QByteArray(prefix) + ' ' + QByteArray::number(i);
}

} // namespace

class AsyncLogSinkTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The defect: a caller must return while the writer is stuck in the
    // device. A handler that writes on the calling thread blocks here.
    void aLogCallReturnsWhileTheDeviceIsBlocked()
    {
        Fixture f(/*gated=*/true);
        QVERIFY2(f.holdTheWriterIn(QStringLiteral("held")),
                 "the first log() did not return while the device was "
                 "blocked: it writes on the calling thread");

        qint64 took = -1;
        const bool returned = f.call(2000, [&f] {
            for (int i = 0; i < 200; ++i)
                f.sink->log(QtInfoMsg, "test", QString::fromLatin1(numbered("line", i)));
        }, &took);
        QVERIFY2(returned,
                 "200 log() calls did not return within 2 s while the "
                 "device was blocked: a caller waits for the log file");
        QVERIFY2(took < 1000,
                 qPrintable(QStringLiteral("200 queued lines took %1 ms").arg(took)));
        // Still blocked the whole time: nothing got past the gate.
        QCOMPARE(f.tap->writes.load(), 0);

        f.openGate();
        QVERIFY(f.sink->flush(5000));
        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 201);
        QCOMPARE(messageOf(lines.first()), QByteArray("held"));
        for (int i = 0; i < 200; ++i)
            QCOMPARE(messageOf(lines.at(i + 1)), numbered("line", i));
        QCOMPARE(f.sink->droppedTotal(), quint64(0));
    }

    // A full queue drops and counts, never waits, and the count is written
    // exactly where the gap is once the writer is back.
    void aFullQueueDropsCountsAndReportsTheGap()
    {
        // Declared before the fixture: a thread it joins may still write them.
        std::atomic<int> keptAccepted{0};
        std::atomic<int> lostAccepted{0};
        AsyncLogSink::Limits limits;
        limits.maxLines = 8;
        Fixture f(/*gated=*/true, limits);
        QVERIFY(f.holdTheWriterIn(QStringLiteral("held")));

        qint64 took = -1;
        const bool returned = f.call(2000, [&] {
            for (int i = 0; i < 8; ++i) {
                if (f.sink->log(QtInfoMsg, "test",
                                QString::fromLatin1(numbered("kept", i))))
                    ++keptAccepted;
            }
            for (int i = 0; i < 10; ++i) {
                if (f.sink->log(QtInfoMsg, "test",
                                QString::fromLatin1(numbered("lost", i))))
                    ++lostAccepted;
            }
        }, &took);
        QVERIFY2(returned,
                 "log() into a full queue did not return: it waits for room");
        QVERIFY2(took < 1000,
                 qPrintable(QStringLiteral("18 calls took %1 ms").arg(took)));
        QCOMPARE(keptAccepted.load(), 8);
        QCOMPARE(lostAccepted.load(), 0);
        QCOMPARE(f.sink->droppedTotal(), quint64(10));

        f.openGate();
        QVERIFY(f.sink->flush(5000));
        QVERIFY(f.sink->log(QtInfoMsg, "test", QStringLiteral("after")));
        QVERIFY(f.sink->flush(5000));

        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 1 + 8 + 1 + 1);
        QCOMPARE(messageOf(lines.at(0)), QByteArray("held"));
        for (int i = 0; i < 8; ++i)
            QCOMPARE(messageOf(lines.at(i + 1)), numbered("kept", i));
        const QByteArray notice = lines.at(9);
        QVERIFY2(notice.contains(" warning lightning.log: dropped 10 log lines "),
                 notice.constData());
        QCOMPARE(messageOf(lines.at(10)), QByteArray("after"));
        for (const QByteArray &line : lines)
            QVERIFY2(!line.contains("lost"), line.constData());
    }

    // One line larger than the byte bound is dropped and reported too, with
    // nothing else queued to carry the notice.
    void aLineOverTheByteBoundIsDroppedAndReported()
    {
        AsyncLogSink::Limits limits;
        limits.maxBytes = 64;
        Fixture f(/*gated=*/false, limits);
        QVERIFY(!f.sink->log(QtInfoMsg, "test", QString(200, QLatin1Char('x'))));
        QVERIFY(f.sink->flush(5000));
        QVERIFY(f.sink->log(QtInfoMsg, "test", QStringLiteral("small")));
        QVERIFY(f.sink->flush(5000));
        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 2);
        QVERIFY2(lines.at(0).contains("dropped 1 log lines"), lines.at(0).constData());
        QCOMPARE(messageOf(lines.at(1)), QByteArray("small"));
    }

    // Per thread, lines reach the device in the order they were logged,
    // across many batches; verbatim output interleaves in place.
    void linesKeepTheirOrder()
    {
        Fixture f(/*gated=*/false);
        constexpr int kThreads = 4;
        constexpr int kLines = 2000;
        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t) {
            producers.emplace_back([&f, t] {
                for (int i = 0; i < kLines; ++i)
                    f.sink->log(QtInfoMsg, "test",
                                QStringLiteral("t%1 %2").arg(t).arg(i));
            });
        }
        for (std::thread &p : producers)
            p.join();
        QVERIFY(f.sink->log(QtInfoMsg, "test", QStringLiteral("A")));
        QVERIFY(f.sink->writeVerbatim(QStringLiteral("B verbatim\n")));
        QVERIFY(f.sink->log(QtInfoMsg, "test", QStringLiteral("C")));
        QVERIFY(f.sink->flush(5000));
        QCOMPARE(f.sink->droppedTotal(), quint64(0));

        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), kThreads * kLines + 3);
        int next[kThreads] = {};
        for (int n = 0; n < kThreads * kLines; ++n) {
            const QList<QByteArray> parts = messageOf(lines.at(n)).split(' ');
            QCOMPARE(parts.size(), 2);
            const int t = parts.at(0).mid(1).toInt();
            QVERIFY(t >= 0 && t < kThreads);
            QCOMPARE(parts.at(1).toInt(), next[t]);
            ++next[t];
        }
        for (int t = 0; t < kThreads; ++t)
            QCOMPARE(next[t], kLines);
        QCOMPARE(messageOf(lines.at(kThreads * kLines)), QByteArray("A"));
        QCOMPARE(lines.at(kThreads * kLines + 1), QByteArray("B verbatim"));
        QCOMPARE(messageOf(lines.at(kThreads * kLines + 2)), QByteArray("C"));
    }

    // A line logged during a stall carries the time it was logged, not the
    // time the writer got to it.
    void theTimestampIsTakenAtTheCall()
    {
        Fixture f(/*gated=*/true);
        QVERIFY(f.holdTheWriterIn(QStringLiteral("held")));

        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(f.call(2000, [&f] {
            f.sink->log(QtInfoMsg, "test", QStringLiteral("stamped"));
        }));
        const qint64 after = QDateTime::currentMSecsSinceEpoch();
        QThread::msleep(150);
        f.openGate();
        QVERIFY(f.sink->flush(5000));

        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 2);
        QCOMPARE(messageOf(lines.at(1)), QByteArray("stamped"));
        const QByteArray stamp = lines.at(1).left(lines.at(1).indexOf(' '));
        const QDateTime parsed =
            QDateTime::fromString(QString::fromLatin1(stamp), Qt::ISODateWithMs);
        QVERIFY2(parsed.isValid(), stamp.constData());
        const qint64 at = parsed.toMSecsSinceEpoch();
        QVERIFY2(at >= before && at <= after,
                 qPrintable(QStringLiteral("stamped %1, logged between %2 and "
                                           "%3: the time was taken at the write")
                                .arg(at).arg(before).arg(after)));
    }

    // Byte for byte the line the synchronous handler wrote.
    void theLineFormatIsUnchanged()
    {
        const qint64 instants[] = {
            0,
            951782400000,      // 2000-02-29T00:00:00.000
            1709251199999,     // 2024-02-29T23:59:59.999
            1790689717035,     // 2026-09-29
            4102444800000,     // 2100-01-01
            253402300799999,   // 9999-12-31T23:59:59.999
        };
        for (const qint64 ms : instants) {
            const QByteArray expected =
                QDateTime::fromMSecsSinceEpoch(ms, QTimeZone(QTimeZone::UTC))
                    .toString(Qt::ISODateWithMs).toUtf8();
            QCOMPARE(AsyncLogSink::isoTimestamp(ms), expected);
            QCOMPARE(AsyncLogSink::formatLine(ms, QtWarningMsg, "matrix.rust",
                                              QStringLiteral("héllo")),
                     expected + " warning matrix.rust: h\xc3\xa9llo\n");
        }
        const struct { QtMsgType type; const char *name; } levels[] = {
            {QtDebugMsg, "debug"}, {QtInfoMsg, "info"},
            {QtWarningMsg, "warning"}, {QtCriticalMsg, "critical"},
            {QtFatalMsg, "fatal"},
        };
        for (const auto &level : levels) {
            QCOMPARE(AsyncLogSink::formatLine(0, level.type, nullptr,
                                              QStringLiteral("m")),
                     QByteArray("1970-01-01T00:00:00.000Z ") + level.name
                         + " default: m\n");
        }
    }

    // The fatal path does not return until what was queued before it, and
    // then its own line, is in the file: Qt aborts right after.
    void aFatalLineDrainsTheQueueThenWritesItself()
    {
        std::atomic<bool> wrote{false};   // outlives the fixture's threads
        Fixture f(/*gated=*/true);
        QVERIFY(f.holdTheWriterIn(QStringLiteral("held")));
        QVERIFY(f.call(2000, [&f] {
            for (int i = 0; i < 5; ++i)
                f.sink->log(QtInfoMsg, "test", QString::fromLatin1(numbered("queued", i)));
        }));

        auto done = std::make_shared<QSemaphore>();
        f.threads.emplace_back([&f, &wrote, done] {
            wrote.store(f.sink->logFatal("test", QStringLiteral("fatal line"), 5000));
            done->release();
        });
        QThread::msleep(100);
        QVERIFY2(f.tap->entered.available() == 0,
                 "the fatal line went to the device past the stuck writer");
        QVERIFY2(!done->tryAcquire(1, 0),
                 "logFatal() returned before its line was written");

        f.openGate();
        QVERIFY2(done->tryAcquire(1, 5000), "logFatal() did not return");
        QVERIFY(wrote.load());
        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 7);
        QCOMPARE(messageOf(lines.at(0)), QByteArray("held"));
        for (int i = 0; i < 5; ++i)
            QCOMPARE(messageOf(lines.at(i + 1)), numbered("queued", i));
        QVERIFY2(lines.at(6).endsWith(" fatal test: fatal line"),
                 lines.at(6).constData());

        // The sink carries on after a fatal write that did not abort.
        QVERIFY(f.sink->log(QtInfoMsg, "test", QStringLiteral("after")));
        QVERIFY(f.sink->flush(5000));
        QCOMPARE(messageOf(f.lines().last()), QByteArray("after"));
    }

    // With the writer stuck for good, the fatal path gives up at its bound
    // (the abort must still happen); its line stays queued.
    void aFatalLineGivesUpAtItsBound()
    {
        std::atomic<bool> wrote{true};   // outlives the fixture's threads
        Fixture f(/*gated=*/true);
        QVERIFY(f.holdTheWriterIn(QStringLiteral("held")));

        qint64 took = -1;
        QVERIFY2(f.call(3000, [&f, &wrote] {
            wrote.store(f.sink->logFatal("test", QStringLiteral("fatal line"), 200));
        }, &took), "logFatal() is not bounded");
        QVERIFY(!wrote.load());
        QVERIFY2(took >= 150 && took < 2000,
                 qPrintable(QStringLiteral("gave up after %1 ms, bound 200").arg(took)));

        f.openGate();
        QVERIFY(f.sink->flush(5000));
        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines.at(1).endsWith(" fatal test: fatal line"));
    }

    // A full queue never drops the fatal line, and the notice for the gap
    // before it still lands in place.
    void aFatalLineIsNeverDroppedAndFollowsTheGapNotice()
    {
        std::atomic<bool> wrote{false};   // outlives the fixture's threads
        AsyncLogSink::Limits limits;
        limits.maxLines = 4;
        Fixture f(/*gated=*/true, limits);
        QVERIFY(f.holdTheWriterIn(QStringLiteral("held")));
        QVERIFY(f.call(2000, [&f] {
            for (int i = 0; i < 4; ++i)
                f.sink->log(QtInfoMsg, "test", QString::fromLatin1(numbered("kept", i)));
            for (int i = 0; i < 3; ++i)
                f.sink->log(QtInfoMsg, "test", QString::fromLatin1(numbered("lost", i)));
        }));
        QCOMPARE(f.sink->droppedTotal(), quint64(3));

        auto done = std::make_shared<QSemaphore>();
        f.threads.emplace_back([&f, &wrote, done] {
            wrote.store(f.sink->logFatal("test", QStringLiteral("fatal line"), 5000));
            done->release();
        });
        QThread::msleep(50);
        f.openGate();
        QVERIFY2(done->tryAcquire(1, 5000), "logFatal() did not return");
        QVERIFY(wrote.load());

        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 1 + 4 + 1 + 1);
        QCOMPARE(messageOf(lines.at(0)), QByteArray("held"));
        for (int i = 0; i < 4; ++i)
            QCOMPARE(messageOf(lines.at(i + 1)), numbered("kept", i));
        QVERIFY2(lines.at(5).contains("dropped 3 log lines"), lines.at(5).constData());
        QVERIFY2(lines.at(6).endsWith(" fatal test: fatal line"), lines.at(6).constData());
    }

    // Shutdown writes everything queued before it returns, and a line or a
    // fatal after it is harmless (a handler during static destruction).
    void shutdownWritesWhatIsQueued()
    {
        Fixture f(/*gated=*/true);
        QVERIFY(f.holdTheWriterIn(QStringLiteral("held")));
        QVERIFY(f.call(2000, [&f] {
            for (int i = 0; i < 1000; ++i)
                f.sink->log(QtInfoMsg, "test", QString::fromLatin1(numbered("queued", i)));
        }));

        auto done = std::make_shared<QSemaphore>();
        f.threads.emplace_back([&f, done] {
            f.sink->shutdown(5000);
            done->release();
        });
        QThread::msleep(50);
        f.openGate();
        QVERIFY2(done->tryAcquire(1, 5000), "shutdown() did not return");

        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 1001);
        QCOMPARE(messageOf(lines.last()), numbered("queued", 999));

        QVERIFY(!f.sink->log(QtInfoMsg, "test", QStringLiteral("late")));
        QVERIFY(f.sink->flush(100));
        QVERIFY(f.sink->logFatal("test", QStringLiteral("late fatal"), 500));
        QVERIFY(f.lines().last().endsWith(" fatal test: late fatal"));
        f.sink->shutdown(100);   // idempotent
    }

    // A writer stuck in the device cannot hold shutdown past its bound.
    void shutdownIsBoundedWhenTheDeviceIsStuck()
    {
        Fixture f(/*gated=*/true);
        QVERIFY(f.holdTheWriterIn(QStringLiteral("held")));
        QVERIFY(f.call(2000, [&f] {
            f.sink->log(QtInfoMsg, "test", QStringLiteral("queued"));
        }));

        qint64 took = -1;
        QVERIFY2(f.call(3000, [&f] { f.sink->shutdown(200); }, &took),
                 "shutdown() is not bounded");
        QVERIFY2(took >= 150 && took < 2000,
                 qPrintable(QStringLiteral("gave up after %1 ms, bound 200").arg(took)));
        QVERIFY(!f.sink->log(QtInfoMsg, "test", QStringLiteral("late")));
        QVERIFY(!f.sink->flush(50));
        // Tear-down opens the gate; the writer left behind finishes on its
        // own and keeps its state alive until then.
    }

    // A device that logs from inside its own write is not fed back into the
    // queue it is draining.
    void theWriterDoesNotQueueItsOwnLines()
    {
        std::atomic<int> refused{0};    // outlive the writer
        std::atomic<int> accepted{0};
        Fixture f(/*gated=*/false);
        AsyncLogSink *sink = f.sink.get();
        f.tap->onWrite = [sink, &refused, &accepted] {
            if (sink->log(QtWarningMsg, "device", QStringLiteral("inner")))
                ++accepted;
            else
                ++refused;
        };
        QVERIFY(f.sink->log(QtInfoMsg, "test", QStringLiteral("outer")));
        QVERIFY(f.sink->flush(5000));
        f.tap->onWrite = nullptr;
        QCOMPARE(accepted.load(), 0);
        QVERIFY(refused.load() >= 1);
        const QList<QByteArray> lines = f.lines();
        QCOMPARE(lines.size(), 1);
        QCOMPARE(messageOf(lines.at(0)), QByteArray("outer"));
    }

    void theProcessSinkIsOptional()
    {
        QVERIFY(!lightning::logging::processLogSink());
        QVERIFY(lightning::logging::flushProcessLog(0));
        Fixture f(/*gated=*/false);
        lightning::logging::setProcessLogSink(f.sink.get());
        QVERIFY(lightning::logging::processLogSink()->log(
            QtInfoMsg, "test", QStringLiteral("via the process sink")));
        QVERIFY(lightning::logging::flushProcessLog(5000));
        lightning::logging::setProcessLogSink(nullptr);
        QCOMPARE(messageOf(f.lines().last()), QByteArray("via the process sink"));
    }
};

QTEST_GUILESS_MAIN(AsyncLogSinkTest)
#include "AsyncLogSinkTest.moc"
