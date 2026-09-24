// Ownership of the shared voice recorder. There is one VoiceRecorder and both
// composers (room and thread) listen to its ready()/failed(), so only the
// owner may act on them or one ready() would be sent twice.
//
// VoiceRecorder::start() refuses while Recording or Processing and returns
// false without emitting failed(). The rules pinned:
//   * ownership is taken only after a successful start;
//   * a live recording is never stolen, only refused;
//   * a refusal leaves the existing owner (and therefore its UI) intact;
//   * the refusal is distinguishable from "no microphone available".

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

    // Record in the room composer, then press the thread mic: the thread
    // start is refused and the room keeps ownership, its pill, cancel button
    // and ready() arming.
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

    // The same during the finalizing window: the owner that pressed Send
    // stays armed, or its ready() would reach nobody.
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
