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

        c.acquire(QStringLiteral("$bmediaB"));
        QCOMPARE(c.audibleOwner(), QStringLiteral("$bmediaB"));
        QVERIFY(!c.owns(QStringLiteral("$amediaA")));
        QCOMPARE(changed.count(), 2);

        c.acquire(QStringLiteral("$bmediaB"));
        QCOMPARE(changed.count(), 2);
        c.acquire(QString());
        QCOMPARE(changed.count(), 2);
    }

    void releaseIsOwnerChecked()
    {
        MediaPlaybackController c;
        c.acquire(QStringLiteral("owner1"));
        c.acquire(QStringLiteral("owner2"));

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

        c.stopAll();
        QCOMPARE(c.stopGeneration(), genBefore + 2);
        QCOMPARE(changed.count(), 2);
    }

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
        QVERIFY(c.resumeRevision() != revision);

        c.rememberVideoPosition(key, 55000, 180000);
        QCOMPARE(changed.count(), 1);

        c.rememberVideoPosition(key, MediaPlaybackController::kMinResumeMs - 1,
                                180000);
        QCOMPARE(c.videoResumePosition(key), qint64(0));
        QCOMPARE(changed.count(), 2);

        c.rememberVideoPosition(key, 179000, 180000);
        QCOMPARE(c.videoResumePosition(key), qint64(0));
        QVERIFY(c.videoFinished(key));

        c.rememberVideoPosition(key, 90000, 180000);
        QVERIFY(!c.videoFinished(key));
        QCOMPARE(c.videoResumePosition(key), qint64(90000));

        c.rememberVideoPosition(QStringLiteral("$short"), 0, 1500);
        QVERIFY(!c.videoFinished(QStringLiteral("$short")));
        c.rememberVideoPosition(QStringLiteral("$short"), 1400, 1500);
        QVERIFY(c.videoFinished(QStringLiteral("$short")));

        c.rememberVideoPosition(key, 179000, 0);
        QCOMPARE(c.videoResumePosition(key), qint64(179000));

        const int before = changed.count();
        c.rememberVideoPosition(QString(), 55000, 180000);
        c.markVideoFinished(QString());
        QCOMPARE(changed.count(), before);
        QVERIFY(!c.videoFinished(QString()));
    }

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
        c.forgetResumePositions();
        QCOMPARE(changed.count(), 1);
    }

    void theResumeStoreIsBounded()
    {
        MediaPlaybackController c;
        const int cap = MediaPlaybackController::kMaxResumeEntries;
        for (int i = 0; i < cap; ++i)
            c.rememberVideoPosition(QStringLiteral("k%1").arg(i), 10000 + i,
                                    0);
        c.rememberVideoPosition(QStringLiteral("k0"), 20000, 0);
        QCOMPARE(c.videoResumePosition(QStringLiteral("k0")), qint64(20000));
        QCOMPARE(c.videoResumePosition(QStringLiteral("k1")), qint64(10001));
        c.rememberVideoPosition(QStringLiteral("new"), 30000, 0);
        QCOMPARE(c.videoResumePosition(QStringLiteral("k0")), qint64(0));
        QCOMPARE(c.videoResumePosition(QStringLiteral("k1")), qint64(10001));
        QCOMPARE(c.videoResumePosition(QStringLiteral("new")), qint64(30000));
        c.forgetVideoPosition(QStringLiteral("k1"));
        c.rememberVideoPosition(QStringLiteral("again"), 30000, 0);
        QCOMPARE(c.videoResumePosition(QStringLiteral("k2")), qint64(10002));
    }

    void aPickedTrackIsKeptPerVideoUntilTheAccountGoes()
    {
        MediaPlaybackController c;
        const QString key = QStringLiteral("$video:example.org");
        QCOMPARE(c.videoTracks(key), (QVariantList{-1, -2}));
        c.rememberVideoTracks(key, 1, 0);
        QCOMPARE(c.videoTracks(key), (QVariantList{1, 0}));
        QCOMPARE(c.videoTracks(QStringLiteral("$other")), (QVariantList{-1, -2}));
        c.rememberVideoTracks(key, 1, -1);
        QCOMPARE(c.videoTracks(key), (QVariantList{1, -1}));
        c.stopAll();
        QCOMPARE(c.videoTracks(key), (QVariantList{1, -1}));
        c.forgetResumePositions();
        QCOMPARE(c.videoTracks(key), (QVariantList{-1, -2}));
    }

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
        c.setVideoPlaybackRate(2.0);
        c.forgetResumePositions();
        QCOMPARE(c.videoPlaybackRate(), 2.0);
    }
};

QTEST_GUILESS_MAIN(MediaPlaybackControllerTest)
#include "MediaPlaybackControllerTest.moc"
