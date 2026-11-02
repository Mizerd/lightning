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
            return false;
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

}

class VoiceOwnershipTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

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

    void asecondComposerCannotStealALiveRecording()
    {
        AppController app(AppController::MockBackend);
        auto *rec = new FakeRecorder;
        app.setVoiceRecorderForTest(rec);

        QVERIFY(app.startVoiceRecording(QStringLiteral("room")));
        QCOMPARE(app.voiceOwner(), QStringLiteral("room"));

        QVERIFY(!app.startVoiceRecording(QStringLiteral("thread")));
        QCOMPARE(app.voiceOwner(), QStringLiteral("room"));
        QVERIFY(rec->isRecording);
        QCOMPARE(rec->cancelCalls, 0);
        QVERIFY(app.voiceRecordingBusy());
    }

    void aliveRecordingIsNotStolenWhileFinalizing()
    {
        AppController app(AppController::MockBackend);
        auto *rec = new FakeRecorder;
        app.setVoiceRecorderForTest(rec);

        QVERIFY(app.startVoiceRecording(QStringLiteral("room")));
        rec->isRecording = false;
        rec->isProcessing = true;

        QVERIFY(!app.startVoiceRecording(QStringLiteral("thread")));
        QCOMPARE(app.voiceOwner(), QStringLiteral("room"));
        QVERIFY(app.voiceRecordingBusy());
    }

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
