// Shared inline-playback coordinator. One audible owner at a time, release is
// owner-checked so a superseded card can never clear the new owner's claim,
// and stopAll (room/account switch, sign-out) clears the owner and bumps the
// stop generation cards use to force-pause.
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <limits>

#include "media/MediaPlaybackController.h"

class MediaPlaybackControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void acquireTransfersOwnership()
    {
        MediaPlaybackController c;
        QSignalSpy changed(&c, &MediaPlaybackController::audibleOwnerChanged);
        QCOMPARE(c.audibleOwner(), QString());

        c.acquire(QStringLiteral("$amediaA"));
        QCOMPARE(c.audibleOwner(), QStringLiteral("$amediaA"));
        QVERIFY(c.owns(QStringLiteral("$amediaA")));
        QCOMPARE(changed.count(), 1);

        // Second card starts: ownership transfers, first card observes.
        c.acquire(QStringLiteral("$bmediaB"));
        QCOMPARE(c.audibleOwner(), QStringLiteral("$bmediaB"));
        QVERIFY(!c.owns(QStringLiteral("$amediaA")));
        QCOMPARE(changed.count(), 2);

        // Re-acquiring the current owner is a no-op (no signal storm).
        c.acquire(QStringLiteral("$bmediaB"));
        QCOMPARE(changed.count(), 2);
        // Empty keys are ignored.
        c.acquire(QString());
        QCOMPARE(changed.count(), 2);
    }

    void releaseIsOwnerChecked()
    {
        MediaPlaybackController c;
        c.acquire(QStringLiteral("owner1"));
        c.acquire(QStringLiteral("owner2"));

        // The superseded card releasing late must NOT clear owner2.
        c.release(QStringLiteral("owner1"));
        QCOMPARE(c.audibleOwner(), QStringLiteral("owner2"));

        c.release(QStringLiteral("owner2"));
        QCOMPARE(c.audibleOwner(), QString());
    }

    void stopAllClearsAndBumpsGeneration()
    {
        MediaPlaybackController c;
        c.acquire(QStringLiteral("owner"));
        const int genBefore = c.stopGeneration();
        QSignalSpy changed(&c, &MediaPlaybackController::audibleOwnerChanged);

        c.stopAll();
        QCOMPARE(c.audibleOwner(), QString());
        QCOMPARE(c.stopGeneration(), genBefore + 1);
        QCOMPARE(changed.count(), 1);

        // stopAll with no owner still bumps the generation — a card whose
        // release raced the switch must still observe a forced stop.
        c.stopAll();
        QCOMPARE(c.stopGeneration(), genBefore + 2);
        QCOMPARE(changed.count(), 2);
    }

    // A video stopped by a room switch is remembered where it was, so its
    // cover can resume it (design review U4: it came back at 0:00). Session
    // RAM only; short positions are not worth offering and a stop at the end
    // means "watched", which the cover shows as Replay.
    void aStoppedVideoIsRememberedWhereItWas()
    {
        MediaPlaybackController c;
        QSignalSpy changed(&c,
                           &MediaPlaybackController::resumePositionsChanged);
        const QString key = QStringLiteral("$video:example.org");
        QCOMPARE(c.videoResumePosition(key), qint64(0));
        QVERIFY(!c.videoFinished(key));

        const int revision = c.resumeRevision();
        c.rememberVideoPosition(key, 55000, 180000);
        QCOMPARE(c.videoResumePosition(key), qint64(55000));
        QVERIFY(!c.videoFinished(key));
        QCOMPARE(changed.count(), 1);
        // QML reads the position through a call, so a binding can only
        // follow it through this revision.
        QVERIFY(c.resumeRevision() != revision);

        // The same answer again is not a change.
        c.rememberVideoPosition(key, 55000, 180000);
        QCOMPARE(changed.count(), 1);

        // Under kMinResumeMs: nothing to resume, and the old entry goes.
        c.rememberVideoPosition(key, MediaPlaybackController::kMinResumeMs - 1,
                                180000);
        QCOMPARE(c.videoResumePosition(key), qint64(0));
        QCOMPARE(changed.count(), 2);

        // Within kEndSlackMs of the end: watched, not "resume from 2:59".
        c.rememberVideoPosition(key, 179000, 180000);
        QCOMPARE(c.videoResumePosition(key), qint64(0));
        QVERIFY(c.videoFinished(key));

        // Replayed and stopped half way: resumable again.
        c.rememberVideoPosition(key, 90000, 180000);
        QVERIFY(!c.videoFinished(key));
        QCOMPARE(c.videoResumePosition(key), qint64(90000));

        // A clip shorter than the slack stopped at 0 was not watched.
        c.rememberVideoPosition(QStringLiteral("$short"), 0, 1500);
        QVERIFY(!c.videoFinished(QStringLiteral("$short")));
        c.rememberVideoPosition(QStringLiteral("$short"), 1400, 1500);
        QVERIFY(c.videoFinished(QStringLiteral("$short")));

        // An unknown duration cannot be "near the end".
        c.rememberVideoPosition(key, 179000, 0);
        QCOMPARE(c.videoResumePosition(key), qint64(179000));

        // Empty keys are ignored.
        const int before = changed.count();
        c.rememberVideoPosition(QString(), 55000, 180000);
        c.markVideoFinished(QString());
        QCOMPARE(changed.count(), before);
        QVERIFY(!c.videoFinished(QString()));
    }

    // Leaving a room is a stopAll(); it must not forget where videos were.
    // Account switch and sign-out forget everything.
    void stopAllKeepsPositionsAndTheAccountForgetsThem()
    {
        MediaPlaybackController c;
        c.rememberVideoPosition(QStringLiteral("a"), 55000, 180000);
        c.markVideoFinished(QStringLiteral("b"));
        c.stopAll();
        QCOMPARE(c.videoResumePosition(QStringLiteral("a")), qint64(55000));
        QVERIFY(c.videoFinished(QStringLiteral("b")));

        QSignalSpy changed(&c,
                           &MediaPlaybackController::resumePositionsChanged);
        c.forgetResumePositions();
        QCOMPARE(c.videoResumePosition(QStringLiteral("a")), qint64(0));
        QVERIFY(!c.videoFinished(QStringLiteral("b")));
        QCOMPARE(changed.count(), 1);
        // Nothing left: no signal.
        c.forgetResumePositions();
        QCOMPARE(changed.count(), 1);
    }

    // The session store is bounded, oldest first, and a key that is
    // re-remembered keeps its first slot rather than duplicating it.
    void theResumeStoreIsBounded()
    {
        MediaPlaybackController c;
        const int cap = MediaPlaybackController::kMaxResumeEntries;
        for (int i = 0; i < cap; ++i)
            c.rememberVideoPosition(QStringLiteral("k%1").arg(i), 10000 + i,
                                    0);
        // Updating an existing key does not evict anything.
        c.rememberVideoPosition(QStringLiteral("k0"), 20000, 0);
        QCOMPARE(c.videoResumePosition(QStringLiteral("k0")), qint64(20000));
        QCOMPARE(c.videoResumePosition(QStringLiteral("k1")), qint64(10001));
        // One more evicts the oldest.
        c.rememberVideoPosition(QStringLiteral("new"), 30000, 0);
        QCOMPARE(c.videoResumePosition(QStringLiteral("k0")), qint64(0));
        QCOMPARE(c.videoResumePosition(QStringLiteral("k1")), qint64(10001));
        QCOMPARE(c.videoResumePosition(QStringLiteral("new")), qint64(30000));
        // A forgotten key leaves the order too, so it cannot evict twice.
        c.forgetVideoPosition(QStringLiteral("k1"));
        c.rememberVideoPosition(QStringLiteral("again"), 30000, 0);
        QCOMPARE(c.videoResumePosition(QStringLiteral("k2")), qint64(10002));
    }

    // One session speed for video (design review B4: the expanded bar read
    // 1x while the player ran at 1.5x). Clamped, and NaN or <= 0 is 1x.
    void theVideoSpeedIsOneSessionValue()
    {
        MediaPlaybackController c;
        QSignalSpy changed(&c,
                           &MediaPlaybackController::videoPlaybackRateChanged);
        QCOMPARE(c.videoPlaybackRate(), 1.0);
        c.setVideoPlaybackRate(1.5);
        QCOMPARE(c.videoPlaybackRate(), 1.5);
        QCOMPARE(changed.count(), 1);
        c.setVideoPlaybackRate(1.5);
        QCOMPARE(changed.count(), 1);
        c.setVideoPlaybackRate(100.0);
        QCOMPARE(c.videoPlaybackRate(), 4.0);
        c.setVideoPlaybackRate(-1.0);
        QCOMPARE(c.videoPlaybackRate(), 1.0);
        c.setVideoPlaybackRate(std::numeric_limits<qreal>::quiet_NaN());
        QCOMPARE(c.videoPlaybackRate(), 1.0);
        // The account's data goes; the user's speed stays.
        c.setVideoPlaybackRate(2.0);
        c.forgetResumePositions();
        QCOMPARE(c.videoPlaybackRate(), 2.0);
    }
};

QTEST_GUILESS_MAIN(MediaPlaybackControllerTest)
#include "MediaPlaybackControllerTest.moc"
