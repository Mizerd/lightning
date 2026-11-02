
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
        rec.inputDevicesChanged({});
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
        VoiceRecorder anon;
        anon.setRecordingForTest(QByteArray());
        anon.inputDevicesChanged({});
        QVERIFY(anon.recording());
    }

    void aRecordingOnTheDefaultFollowsItUnderANewId()
    {
        using Info = VoiceRecorder::InputInfo;
        QVERIFY(VoiceRecorder::inputStillPresent(
            "old-default", true, {Info{"usb", false}, Info{"new-default", true}}));
        QVERIFY(!VoiceRecorder::inputStillPresent(
            "usb", false, {Info{"new-default", true}}));
        QVERIFY(!VoiceRecorder::inputStillPresent("old-default", true, {}));
        QVERIFY(!VoiceRecorder::inputStillPresent(
            "old-default", true, {Info{"usb", false}}));
    }

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
