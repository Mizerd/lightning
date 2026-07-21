// v0.7: shared inline-playback coordinator. One audible owner at a time,
// release is owner-checked so a superseded card can never clear the new
// owner's claim, and stopAll (room/account switch, sign-out) both clears
// the owner and bumps the stop generation cards use to force-pause.
#include <QSignalSpy>
#include <QtTest/QtTest>

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
};

QTEST_GUILESS_MAIN(MediaPlaybackControllerTest)
#include "MediaPlaybackControllerTest.moc"
