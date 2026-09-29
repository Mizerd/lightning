// CallSoundPlayer's liveness: a sound thread stuck on the sound server must
// stop reporting sounds as playable, or the ringer keeps choosing its own
// (silent) ring over the desktop's call sound. No sound effect is ever
// created here, so no audio device is needed.
#include "calls/CallSoundPlayer.h"

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
};

QTEST_GUILESS_MAIN(CallSoundPlayerTest)
#include "CallSoundPlayerTest.moc"
