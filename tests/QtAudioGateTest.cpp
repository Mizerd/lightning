#include <QDataStream>
#include <QFile>
#include <QMediaPlayer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "media/MediaPlaybackController.h"
#include "media/QtAudioGate.h"
#include "media/VoicePlaybackController.h"
#include "media/VoiceRecorder.h"

using lightning::media::QtAudioGate;

// Qt's PulseAudio backend aborts the process when a new stream starts while
// it has no server connection (QtAudioGate.h). The gate's rules on fake
// probes, and that the voice player and recorder never start a stream while
// it is closed and do start once it opens.
namespace {

struct FakeServer {
    bool thread = true;
    bool socket = true;
    QtAudioGate::Probes probes()
    {
        return {[this] { return thread; }, [this] { return socket; }};
    }
};

bool writeSilentWav(const QString &path, int seconds)
{
    const quint32 rate = 8000;
    const quint32 dataBytes = rate * 2 * quint32(seconds);
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s.writeRawData("RIFF", 4);
    s << quint32(36 + dataBytes);
    s.writeRawData("WAVE", 4);
    s.writeRawData("fmt ", 4);
    s << quint32(16) << quint16(1) << quint16(1) << rate << (rate * 2)
      << quint16(2) << quint16(16);
    s.writeRawData("data", 4);
    s << dataBytes;
    out.append(QByteArray(int(dataBytes), '\0'));
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

} // namespace

// Stands in for a QML MediaPlayer: anything with a play() slot.
class FakePlayer : public QObject
{
    Q_OBJECT
public:
    int plays = 0;
public Q_SLOTS:
    void play() { ++plays; }
};

class QtAudioGateTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void openWhileTheConnectionHoldsAndClosedWhileItIsDown()
    {
        FakeServer server;
        QtAudioGate gate(QtAudioGate::Mode::Pulse, server.probes());
        gate.setTimingForTest(20, 100);
        QVERIFY(gate.ready());
        QVERIFY(gate.checkNow());
        // Qt dropped its connection (its thread stops with it).
        server.thread = false;
        QVERIFY(!gate.checkNow());
        // Thread back but the server's socket does not answer: still closed.
        server.thread = true;
        server.socket = false;
        QVERIFY(!gate.checkNow());
        // Both back: open only after it has held for the live time.
        server.socket = true;
        QVERIFY(!gate.checkNow());
        QTRY_VERIFY_WITH_TIMEOUT(gate.checkNow(), 2000);
        // Nothing waits: nothing is polled (asked on demand only).
        QVERIFY(!gate.pollingForTest());
        server.thread = false;
        QVERIFY(!gate.checkNow());
        QVERIFY(!gate.pollingForTest());
    }

    void aStartWaitsRunsOnceAndOnlyWhenOpen()
    {
        FakeServer server;
        server.thread = false;
        QtAudioGate gate(QtAudioGate::Mode::Pulse, server.probes());
        gate.setTimingForTest(20, 100);
        QVERIFY(!gate.ready());
        QObject context;
        int started = 0;
        int gaveUp = 0;
        gate.whenReady(&context, [&] { ++started; }, [&] { ++gaveUp; }, 5000);
        QCOMPARE(gate.pendingForTest(), 1);
        QTest::qWait(300);
        QCOMPARE(started, 0);
        QCOMPARE(gaveUp, 0);
        server.thread = true;
        QTRY_COMPARE_WITH_TIMEOUT(started, 1, 3000);
        QTest::qWait(200);
        QCOMPARE(started, 1);
        QCOMPARE(gaveUp, 0);
        QCOMPARE(gate.pendingForTest(), 0);
        QVERIFY2(!gate.pollingForTest(), "still polling with nothing waiting");
        // Open now: a start runs at once, synchronously.
        gate.whenReady(&context, [&] { ++started; }, [&] { ++gaveUp; });
        QCOMPARE(started, 2);
    }

    void aStartThatWaitsTooLongGivesUpAndACancelledOneRunsNothing()
    {
        FakeServer server;
        server.thread = false;
        QtAudioGate gate(QtAudioGate::Mode::Pulse, server.probes());
        gate.setTimingForTest(20, 100);
        QObject context;
        int started = 0;
        int gaveUp = 0;
        gate.whenReady(&context, [&] { ++started; }, [&] { ++gaveUp; }, 200);
        QTRY_COMPARE_WITH_TIMEOUT(gaveUp, 1, 3000);
        QCOMPARE(started, 0);

        gate.whenReady(&context, [&] { ++started; }, [&] { ++gaveUp; }, 5000);
        gate.cancel(&context);
        QCOMPARE(gate.pendingForTest(), 0);
        {
            auto gone = std::make_unique<QObject>();
            gate.whenReady(gone.get(), [&] { ++started; }, [&] { ++gaveUp; },
                           5000);
        }
        server.thread = true;
        QTest::qWait(400);
        QCOMPARE(started, 0);
        QCOMPARE(gaveUp, 1);
        QCOMPARE(gate.pendingForTest(), 0);
    }

    // Where Qt does not say which backend it chose, the gate holds only on
    // evidence: the PulseAudio thread seen, then gone. A process on another
    // backend never sees it and is never held; an ungated gate never asks.
    void anUnknownBackendIsHeldOnlyOnEvidence()
    {
        FakeServer server;
        server.thread = false;
        QtAudioGate maybe(QtAudioGate::Mode::MaybePulse, server.probes());
        maybe.setTimingForTest(20, 100);
        QVERIFY(maybe.ready());
        QVERIFY(maybe.checkNow());
        server.thread = true;
        QVERIFY(maybe.checkNow());
        server.thread = false;
        QVERIFY(!maybe.checkNow());

        int asked = 0;
        QtAudioGate never(QtAudioGate::Mode::Ungated,
                          {[&] { ++asked; return false; },
                           [&] { ++asked; return false; }});
        QVERIFY(never.ready());
        QVERIFY(never.checkNow());
        int started = 0;
        never.whenReady(this, [&] { ++started; }, {});
        QCOMPARE(started, 1);
        QCOMPARE(asked, 0);
    }

    // The QML route: qtAudio.play(player) instead of player.play().
    void aQmlPlayWaitsAndACancelDropsIt()
    {
        FakeServer server;
        server.thread = false;
        QtAudioGate gate(QtAudioGate::Mode::Pulse, server.probes());
        gate.setTimingForTest(20, 100);
        FakePlayer player;
        gate.play(&player);
        gate.play(&player); // a second press replaces the first
        QCOMPARE(gate.pendingForTest(), 1);
        gate.cancelPlay(&player); // a pause
        QCOMPARE(gate.pendingForTest(), 0);
        gate.play(&player);
        QTest::qWait(300);
        QCOMPARE(player.plays, 0);
        server.thread = true;
        QTRY_COMPARE_WITH_TIMEOUT(player.plays, 1, 3000);
        QTest::qWait(200);
        QCOMPARE(player.plays, 1);
        // Open: at once.
        gate.play(&player);
        QCOMPARE(player.plays, 2);
    }

    // The player: play() never reaches QMediaPlayer while the gate is closed,
    // and does once it opens. A pause in between drops it.
    void theVoicePlayerWaitsForTheGate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString clip = dir.filePath(QStringLiteral("one.wav"));
        QVERIFY(writeSilentWav(clip, 3));
        FakeServer server;
        server.thread = false;
        QtAudioGate gate(QtAudioGate::Mode::Pulse, server.probes());
        gate.setTimingForTest(20, 100);

        VoicePlaybackController vp;
        vp.setAudioGateForTest(&gate);
        const QString url = QUrl::fromLocalFile(clip).toString();
        vp.setSourceResolverForTest([url](const QString &) { return url; });
        vp.play(QStringLiteral("$one"), QStringLiteral("!room:x"),
                QStringLiteral("mk-one"), {});
        QVERIFY(vp.active());
        QVERIFY(vp.loaded());
        QTest::qWait(300);
        QCOMPARE(vp.playerStartsForTest(), 0);
        QVERIFY(vp.player());
        QCOMPARE(vp.player()->playbackState(), QMediaPlayer::StoppedState);

        // A pause while waiting drops the start.
        vp.pause();
        QCOMPARE(gate.pendingForTest(), 0);
        server.thread = true;
        QTest::qWait(400);
        QCOMPARE(vp.playerStartsForTest(), 0);

        // Open: the next play starts (once the reopened connection has held).
        vp.resume();
        QTRY_COMPARE_WITH_TIMEOUT(vp.playerStartsForTest(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(vp.playing(), 5000);
        vp.stop();

        // Closed again, then reopened: the waiting start runs by itself.
        server.thread = false;
        QVERIFY(!gate.checkNow());
        vp.play(QStringLiteral("$one"), QStringLiteral("!room:x"),
                QStringLiteral("mk-one"), {});
        QTest::qWait(200);
        QCOMPARE(vp.playerStartsForTest(), 1);
        server.thread = true;
        QTRY_COMPARE_WITH_TIMEOUT(vp.playerStartsForTest(), 2, 3000);
        vp.stop();
    }

    // A call that starts while a clip's start waits at the gate pauses it
    // for the call: nothing starts while the call runs, even once the gate
    // opens, and the end of the call plays it (through the gate again).
    void aClipHeldAtTheGateWhenACallStartsPlaysOnlyAfterTheCall()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString clip = dir.filePath(QStringLiteral("one.wav"));
        QVERIFY(writeSilentWav(clip, 3));
        FakeServer server;
        server.thread = false;
        QtAudioGate gate(QtAudioGate::Mode::Pulse, server.probes());
        gate.setTimingForTest(20, 100);

        VoicePlaybackController vp;
        vp.setAudioGateForTest(&gate);
        const QString url = QUrl::fromLocalFile(clip).toString();
        vp.setSourceResolverForTest([url](const QString &) { return url; });
        vp.play(QStringLiteral("$one"), QStringLiteral("!room:x"),
                QStringLiteral("mk-one"), {});
        QVERIFY(vp.loaded());
        QCOMPARE(gate.pendingForTest(), 1);
        QCOMPARE(vp.playerStartsForTest(), 0);

        vp.pauseForCall();
        QVERIFY2(vp.pausedForCall(),
                 "a start waiting at the gate was not counted as running");
        QCOMPARE(gate.pendingForTest(), 0);

        // The gate opens during the call: still silent.
        server.thread = true;
        QTest::qWait(400);
        QTRY_VERIFY_WITH_TIMEOUT(gate.checkNow(), 2000);
        QTest::qWait(300);
        QCOMPARE(vp.playerStartsForTest(), 0);
        QVERIFY(!vp.playing());

        // The call ends: the clip plays.
        vp.resumeAfterCall();
        QVERIFY(!vp.pausedForCall());
        QTRY_COMPARE_WITH_TIMEOUT(vp.playerStartsForTest(), 1, 3000);
        vp.stop();
    }

    // Another card taking the speaker while a voice clip waits at the gate
    // drops the wait: it must not start later over that card.
    void anotherCardTakingTheSpeakerDropsAWaitingStart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString clip = dir.filePath(QStringLiteral("one.wav"));
        QVERIFY(writeSilentWav(clip, 3));
        FakeServer server;
        server.thread = false;
        QtAudioGate gate(QtAudioGate::Mode::Pulse, server.probes());
        gate.setTimingForTest(20, 100);
        MediaPlaybackController audibility;
        VoicePlaybackController vp;
        vp.setAudibility(&audibility);
        vp.setAudioGateForTest(&gate);
        const QString url = QUrl::fromLocalFile(clip).toString();
        vp.setSourceResolverForTest([url](const QString &) { return url; });
        vp.play(QStringLiteral("$one"), QStringLiteral("!room:x"),
                QStringLiteral("mk-one"), {});
        QCOMPARE(gate.pendingForTest(), 1);
        audibility.acquire(QStringLiteral("video:someone-else"));
        QCOMPARE(gate.pendingForTest(), 0);
        server.thread = true;
        QTest::qWait(500);
        QTRY_VERIFY_WITH_TIMEOUT(gate.checkNow(), 2000); // open now
        QTest::qWait(300);
        QCOMPARE(vp.playerStartsForTest(), 0);
        QVERIFY(!vp.playing());
        vp.stop();
    }

    // The recorder: start() is accepted while the gate is closed, but
    // record() is not called until it opens; cancel and stop drop it.
    void theVoiceRecorderWaitsForTheGate()
    {
        FakeServer server;
        server.thread = false;
        QtAudioGate gate(QtAudioGate::Mode::Pulse, server.probes());
        gate.setTimingForTest(20, 100);
        VoiceRecorder recorder;
        recorder.setAudioGateForTest(&gate);
        // Never a real microphone in a test: the start past the gate is a
        // fake that counts.
        int opened = 0;
        recorder.setStartNowForTest([&opened] {
            ++opened;
            return true;
        });
        QSignalSpy failed(&recorder, &VoiceRecorder::failed);

        QVERIFY(recorder.start());
        QVERIFY(recorder.startPending());
        QVERIFY(!recorder.recording());
        QVERIFY2(!recorder.start(), "a second start while one waits");
        QTest::qWait(300);
        QCOMPARE(recorder.startNowCallsForTest(), 0);
        QCOMPARE(failed.count(), 0);

        recorder.cancel();
        QVERIFY(!recorder.startPending());
        QCOMPARE(gate.pendingForTest(), 0);
        QCOMPARE(failed.count(), 0);

        // Released (stop) before it could open: failed() tells the owner.
        QVERIFY(recorder.start());
        recorder.stop();
        QVERIFY(!recorder.startPending());
        QCOMPARE(failed.count(), 1);

        // Opens: the start runs, once, and only now.
        QVERIFY(recorder.start());
        QTest::qWait(200);
        QCOMPARE(opened, 0);
        server.thread = true;
        QTRY_VERIFY_WITH_TIMEOUT(!recorder.startPending(), 3000);
        QCOMPARE(opened, 1);
        QCOMPARE(recorder.startNowCallsForTest(), 1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(gate.pendingForTest(), 0);
        // Open now: a start goes straight through.
        QVERIFY(recorder.start());
        QCOMPARE(opened, 2);
        QCOMPARE(recorder.recordStartsForTest(), 0); // no microphone opened
    }
};

QTEST_MAIN(QtAudioGateTest)
#include "QtAudioGateTest.moc"
