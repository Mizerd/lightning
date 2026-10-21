// C43: a microphone that vanishes mid-recording ends the recording WITHOUT
// ever emitting ready() (every consumer of ready() sends the clip), and reports
// failed() once and removes the clip, which is what makes every owner release
// the recorder. Headless: no capture chain is
// opened. The preview pill's width is VoicePreviewWidthQmlTest.

#include "media/VoiceRecorder.h"

#include <QFile>
#include <QMediaDevices>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest/QtTest>

class VoiceFixC43Test : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aVanishedMicrophoneFailsOnceAndDiscardsTheClip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString clip = dir.filePath(QStringLiteral("clip.ogg"));
        QFile f(clip);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("OggS-captured");
        f.close();

        VoiceRecorder rec;
        rec.setRecordingForTest("mic-1", clip, 4200);
        QSignalSpy failed(&rec, &VoiceRecorder::failed);
        QSignalSpy ready(&rec, &VoiceRecorder::ready);
        rec.inputDevicesChanged({}); // the list no longer holds mic-1
        QVERIFY(rec.endedByDisconnect());
        QVERIFY(!rec.recording());
        QVERIFY(rec.processing());
        rec.finishForTest();
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!rec.processing());
        QVERIFY(!QFile::exists(clip));
    }

    void anOrdinaryStopStillEmitsReady()
    {
        VoiceRecorder rec;
        rec.setRecordingForTest("mic-1", QString(), 1000);
        QSignalSpy ready(&rec, &VoiceRecorder::ready);
        QSignalSpy failed(&rec, &VoiceRecorder::failed);
        rec.stop();
        rec.finishForTest();
        QCOMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!rec.endedByDisconnect());
    }

    void aListedDeviceOrAnUnrelatedChangeLeavesTheRecordingAlone()
    {
        VoiceRecorder rec;
        rec.setRecordingForTest("mic-1");
        QSignalSpy failed(&rec, &VoiceRecorder::failed);
        rec.inputDevicesChanged({{"mic-2", false}, {"mic-1", false}});
        QCOMPARE(failed.count(), 0);
        QVERIFY(rec.recording());
        QVERIFY(!rec.endedByDisconnect());
        // No device id recorded: nothing to compare against.
        VoiceRecorder anon;
        anon.setRecordingForTest(QByteArray());
        anon.inputDevicesChanged({});
        QVERIFY(anon.recording());
    }

    void aRecordingOnTheDefaultFollowsItUnderANewId()
    {
        using Info = VoiceRecorder::InputInfo;
        // Followed the default, which is now listed as a different id.
        QVERIFY(VoiceRecorder::inputStillPresent(
            "old-default", true, {Info{"usb", false}, Info{"new-default", true}}));
        // Pinned to a device: another default is not it.
        QVERIFY(!VoiceRecorder::inputStillPresent(
            "usb", false, {Info{"new-default", true}}));
        // Followed the default and no input is left at all.
        QVERIFY(!VoiceRecorder::inputStillPresent("old-default", true, {}));
        QVERIFY(!VoiceRecorder::inputStillPresent(
            "old-default", true, {Info{"usb", false}}));
    }

    // The QMediaDevices::audioInputsChanged connection itself: emitting it
    // reaches inputDevicesChanged() with the live list, which cannot hold a
    // fake id, so the recording ends. A missing connect leaves it recording.
    void theDeviceWatchIsConnected()
    {
        VoiceRecorder rec;
        rec.setRecordingForTest("mic-that-is-not-real");
        QMediaDevices *devices = rec.watchInputDevicesForTest();
        QVERIFY(devices != nullptr);
        QVERIFY(rec.recording());
        Q_EMIT devices->audioInputsChanged();
        QVERIFY(!rec.recording());
        QVERIFY(rec.endedByDisconnect());
    }
};

QTEST_GUILESS_MAIN(VoiceFixC43Test)
#include "VoiceFixC43Test.moc"
