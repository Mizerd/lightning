// CallSoundPlayer's liveness: a sound thread stuck on the sound server must
// stop reporting sounds as playable, or the ringer keeps choosing its own
// (silent) ring over the desktop's call sound. No sound effect is ever
// created here, so no audio device is needed.
#include "calls/CallSoundPlayer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QLoggingCategory>
#include <QSemaphore>
#include <QtTest/QtTest>

Q_LOGGING_CATEGORY(lcCallSound, "lightning.calls.sound")

class CallSoundPlayerTest : public QObject
{
    Q_OBJECT

private slots:
    // Measured 2026-09-25 on Qt 6.8.2: after a sound-server stall the sound
    // thread never comes back. canPlay() must turn false within ~2 s of a job
    // going unanswered, and true again if the thread catches up.
    void aStuckSoundThreadReportsNothingPlayable()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
                               /*offTheGuiThread=*/true);
        player.markReadyForTest(QStringLiteral("ring"));
        QVERIFY(player.canPlay(QStringLiteral("ring")));

        // A quick job is answered and changes nothing.
        QSemaphore quick;
        player.postForTest([&quick] { quick.release(); });
        QVERIFY(quick.tryAcquire(1, 2000));
        QTest::qWait(2300);
        QVERIFY(player.canPlay(QStringLiteral("ring")));

        // A job that blocks, as an open on a stalled server does.
        QSemaphore unblock;
        player.postForTest([&unblock] { unblock.acquire(); });
        QVERIFY(player.canPlay(QStringLiteral("ring"))); // not yet 2 s
        QTRY_VERIFY_WITH_TIMEOUT(!player.canPlay(QStringLiteral("ring")), 4000);
        // Later cues queue behind it and do not make it look alive.
        player.postForTest([] {});
        QVERIFY(!player.canPlay(QStringLiteral("ring")));

        // The thread catches up: the sound is playable again.
        unblock.release();
        QTRY_VERIFY_WITH_TIMEOUT(player.canPlay(QStringLiteral("ring")), 4000);
    }

    // Qt 6.10+ keeps the effects on the GUI thread, where a job runs at once:
    // there is nothing to wait on and readiness is the loaded state alone.
    void onTheGuiThreadReadinessIsTheLoadedState()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
                               /*offTheGuiThread=*/false);
        QVERIFY(!player.canPlay(QStringLiteral("ring")));
        bool ran = false;
        player.postForTest([&ran] { ran = true; });
        QVERIFY(ran);
        player.markReadyForTest(QStringLiteral("ring"));
        QVERIFY(player.canPlay(QStringLiteral("ring")));
    }

    // Measured live on Windows 2026-10-07: the audio endpoint went away, every
    // cue went to Error ("call sound unusable"), and when the device came
    // back nothing reloaded them, so the ringer stayed on the desktop's sound
    // for the rest of the session. A changed output list must reload them,
    // once per burst of changes (devices flap), and a burst that never goes
    // quiet must still be answered.
    void aChangedOutputListReloadsTheCuesOncePerBurst()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
                               /*offTheGuiThread=*/false);
        player.setOutputChangeDebounceForTest(150, 600);
        QCOMPARE(player.soundReloadsForTest(), 0);

        // A burst: one reload, after it goes quiet.
        for (int i = 0; i < 5; ++i) {
            player.audioOutputsChanged();
            QTest::qWait(30);
        }
        QCOMPARE(player.soundReloadsForTest(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 1, 2000);
        QTest::qWait(400);
        QCOMPARE(player.soundReloadsForTest(), 1);

        // The next change is a new burst.
        player.audioOutputsChanged();
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 2, 2000);

        // Changes every 100 ms for 1.5 s never go quiet for 150 ms, and are
        // still answered: at the longest wait, then again.
        QElapsedTimer flapping;
        flapping.start();
        while (flapping.elapsed() < 1500) {
            player.audioOutputsChanged();
            QTest::qWait(100);
        }
        QVERIFY2(player.soundReloadsForTest() >= 3,
                 "a burst that never went quiet was never answered");
        QVERIFY2(player.soundReloadsForTest() <= 5,
                 qPrintable(QStringLiteral("%1 reloads for 1.5 s of flapping")
                                .arg(player.soundReloadsForTest() - 2)));
    }

    // The notification chimes are bundled like every other sound, in the same
    // format (48 kHz, 16-bit, stereo), and short enough to repeat all day.
    void theNotificationChimesAreBundledInTheCallSoundFormat()
    {
        const QStringList known = CallSoundPlayer::knownSounds();
        for (const char *name : { "message", "mention" }) {
            const QString sound = QString::fromLatin1(name);
            QVERIFY2(known.contains(sound),
                     qPrintable(sound + " is not a known sound"));
            QFile file(QStringLiteral(SOUNDS_DIR "/") + sound
                       + QStringLiteral(".wav"));
            QVERIFY2(file.open(QIODevice::ReadOnly),
                     qPrintable(file.fileName() + " is missing"));
            const QByteArray wav = file.readAll();
            QVERIFY(wav.size() > 44);
            QCOMPARE(wav.left(4), QByteArrayLiteral("RIFF"));
            auto u16 = [&wav](int at) {
                return quint16(quint8(wav[at]) | (quint8(wav[at + 1]) << 8));
            };
            auto u32 = [&wav](int at) {
                return quint32(quint8(wav[at]) | (quint8(wav[at + 1]) << 8)
                               | (quint8(wav[at + 2]) << 16)
                               | (quint32(quint8(wav[at + 3])) << 24));
            };
            QCOMPARE(u16(22), quint16(2));       // stereo
            QCOMPARE(u32(24), quint32(48000));   // 48 kHz
            QCOMPARE(u16(34), quint16(16));      // 16-bit
            const double seconds = double(wav.size() - 44) / (48000.0 * 4.0);
            QVERIFY2(seconds >= 0.15 && seconds <= 0.5,
                     qPrintable(sound + QStringLiteral(" lasts %1 s")
                                    .arg(seconds)));
        }
    }
};

QTEST_GUILESS_MAIN(CallSoundPlayerTest)
#include "CallSoundPlayerTest.moc"
