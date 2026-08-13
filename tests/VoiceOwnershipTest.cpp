// v0.7 thread parity: ownership of the SHARED voice recorder.
//
// There is exactly one VoiceRecorder for the application and both composers
// (room and thread) listen to its ready()/failed(). Before ownership existed,
// each composer armed those Connections from its own local flag, so a
// recording started in the room composer and then superseded from the thread
// panel left BOTH armed and one ready() sent the same file twice — into the
// room AND into the thread. Opening a thread does not change currentRoomId,
// so the room composer's cancel-on-room-change never fired for that sequence.
//
// The first fix was worse than the bug: it moved ownership BEFORE calling
// start() and cleared it when start() failed. VoiceRecorder::start() REFUSES
// while Recording or Processing and returns false WITHOUT emitting failed(),
// so the recorder stayed live while owned by NOBODY — microphone open, no
// pill, no cancel button, cancelVoiceRecording() early-returning on an empty
// owner — until the 15-minute cap, and across sign-out.
//
// These tests pin the corrected rules:
//   * ownership is taken only AFTER a successful start;
//   * a live recording is never stolen, only refused;
//   * a refusal leaves the existing owner (and therefore its UI) intact;
//   * the refusal is distinguishable from "no microphone available".
// No real capture chain, no microphone, no audio backend: the recorder is
// replaced with a state-only double through the test seam.

#include "app/AppController.h"
#include "media/VoiceRecorder.h"

#include <QtTest>

namespace {

// State-only stand-in for the capture chain. Mirrors the real recorder's
// contract exactly where it matters: start() refuses unless Idle and does
// NOT emit failed() for that refusal.
class FakeRecorder : public VoiceRecorder
{
public:
    bool isRecording = false;
    bool isProcessing = false;
    bool deviceAvailable = true;
    int startCalls = 0;
    int cancelCalls = 0;

    bool recording() const override { return isRecording; }
    bool processing() const override { return isProcessing; }

    bool start() override
    {
        ++startCalls;
        if (isRecording || isProcessing)
            return false;          // busy: refused, and silent (no failed())
        if (!deviceAvailable) {
            Q_EMIT failed(QStringLiteral("no device"));
            return false;
        }
        isRecording = true;
        return true;
    }

    void cancel() override
    {
        ++cancelCalls;
        isRecording = false;
        isProcessing = false;
    }
};

} // namespace

class VoiceOwnershipTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    // A successful start takes ownership; nothing owns the recorder before.
    void ownershipIsTakenOnlyAfterASuccessfulStart()
    {
        AppController app(AppController::MockBackend);
        auto *rec = new FakeRecorder;
        app.setVoiceRecorderForTest(rec);
        QCOMPARE(app.voiceOwner(), QString{});

        QVERIFY(app.startVoiceRecording(QStringLiteral("room")));
        QCOMPARE(app.voiceOwner(), QStringLiteral("room"));
        QVERIFY(rec->isRecording);
    }

    // A start that fails for a real hardware/encoder reason must leave NO
    // owner behind — otherwise the composer would show a recording pill for
    // a recorder that never started.
    void afailedStartLeavesNoOwner()
    {
        AppController app(AppController::MockBackend);
        auto *rec = new FakeRecorder;
        rec->deviceAvailable = false;
        app.setVoiceRecorderForTest(rec);

        QVERIFY(!app.startVoiceRecording(QStringLiteral("room")));
        QCOMPARE(app.voiceOwner(), QString{});
        QVERIFY(!app.voiceRecordingBusy());
    }

    // THE C1 REGRESSION. Record in the room composer, then press the thread
    // mic. The thread start must be refused and the ROOM must keep
    // ownership — so the live recording keeps its pill, its cancel button
    // and its ready() arming, and the microphone is never orphaned.
    void asecondComposerCannotStealALiveRecording()
    {
        AppController app(AppController::MockBackend);
        auto *rec = new FakeRecorder;
        app.setVoiceRecorderForTest(rec);

        QVERIFY(app.startVoiceRecording(QStringLiteral("room")));
        QCOMPARE(app.voiceOwner(), QStringLiteral("room"));

        QVERIFY(!app.startVoiceRecording(QStringLiteral("thread")));
        // Ownership unchanged — this is the whole point.
        QCOMPARE(app.voiceOwner(), QStringLiteral("room"));
        // And the recorder is still running, not cancelled or restarted.
        QVERIFY(rec->isRecording);
        QCOMPARE(rec->cancelCalls, 0);
        // Distinguishable from "no microphone", so the UI can say so.
        QVERIFY(app.voiceRecordingBusy());
    }

    // Same rule during the finalizing window: Send pressed in the room
    // composer, then the thread mic. The owner that pressed Send must stay
    // armed, or its ready() would be delivered to nobody and the recording
    // would be neither sent nor deleted.
    void aliveRecordingIsNotStolenWhileFinalizing()
    {
        AppController app(AppController::MockBackend);
        auto *rec = new FakeRecorder;
        app.setVoiceRecorderForTest(rec);

        QVERIFY(app.startVoiceRecording(QStringLiteral("room")));
        rec->isRecording = false;
        rec->isProcessing = true;      // stop() pressed; deriving waveform

        QVERIFY(!app.startVoiceRecording(QStringLiteral("thread")));
        QCOMPARE(app.voiceOwner(), QStringLiteral("room"));
        QVERIFY(app.voiceRecordingBusy());
    }

    // Releasing lets the other composer record; ownership then moves
    // cleanly. Also proves the refusal is not permanent.
    void ownershipMovesOnceTheRecorderIsFree()
    {
        AppController app(AppController::MockBackend);
        auto *rec = new FakeRecorder;
        app.setVoiceRecorderForTest(rec);

        QVERIFY(app.startVoiceRecording(QStringLiteral("room")));
        app.cancelVoiceRecording();
        QCOMPARE(app.voiceOwner(), QString{});
        QVERIFY(!rec->isRecording);

        QVERIFY(app.startVoiceRecording(QStringLiteral("thread")));
        QCOMPARE(app.voiceOwner(), QStringLiteral("thread"));
    }

    // Ownership is a send authorisation: an unrecognised owner must never
    // hold it, and must not start a recording either.
    void anUnknownOwnerIsRefused()
    {
        AppController app(AppController::MockBackend);
        auto *rec = new FakeRecorder;
        app.setVoiceRecorderForTest(rec);

        QVERIFY(!app.startVoiceRecording(QStringLiteral("elsewhere")));
        QCOMPARE(app.voiceOwner(), QString{});
        QCOMPARE(rec->startCalls, 0);
        QVERIFY(!rec->isRecording);
    }
};

QTEST_MAIN(VoiceOwnershipTest)
#include "VoiceOwnershipTest.moc"
