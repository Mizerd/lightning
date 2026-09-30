// Which room invites are announced, and when one counts as resolved.
//
// Reported live 2026-09-30: one invite raised two notifications 0 s apart,
// "You were invited to Empty Room" and then the room's real name. The room
// list re-sorted the invite when its name arrived: a remove and an insert,
// each its own roomsChanged, with the room absent in between. The old sweep
// resolved it on the remove and announced it again on the insert. Pure
// logic with the clock passed in; no homeserver.

#include "notifications/InviteNoticeTracker.h"

#include <QtTest/QtTest>

namespace {
const QString kRoom = QStringLiteral("!invite:example.org");
const QString kOther = QStringLiteral("!other:example.org");
}

class InviteNoticeTrackerTest : public QObject
{
    Q_OBJECT

private slots:
    // The reported shape: listed, gone, listed again within one burst, each
    // step processed the way AppController processes a roomsChanged. One
    // announcement and nothing resolved, both when the re-sort comes before
    // the announcement and when it comes long after.
    void aResortIsOneInviteNotTwo()
    {
        InviteNoticeTracker tracker;
        QStringList announced;
        QStringList resolved;
        const auto step = [&](const QSet<QString> &invites, qint64 now) {
            tracker.observe(invites, true, now);
            resolved += tracker.takeResolved(now);
            announced += tracker.takeDue(now);
        };
        const auto idle = [&](qint64 now) {
            resolved += tracker.takeResolved(now);
            announced += tracker.takeDue(now);
        };
        step({ kRoom }, 1000);
        step({}, 1001);
        step({ kRoom }, 1002);
        idle(1000 + InviteNoticeTracker::kSettleMs);
        QCOMPARE(announced, QStringList{ kRoom });
        step({}, 5000);
        step({ kRoom }, 5010);
        idle(60000);
        QCOMPARE(announced, QStringList{ kRoom });
        QVERIFY2(resolved.isEmpty(),
                 "a re-sort resolved the invite, so its Activity entry churned "
                 "and the next insert was a new invite");
    }

    // Announced only after the settle, so the name that arrives a moment
    // later is the one read at delivery.
    void aNewInviteWaitsForTheSettle()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom }, true, 1000);
        QVERIFY(tracker.takeDue(1000).isEmpty());
        QCOMPARE(tracker.nextDueInMs(1000), InviteNoticeTracker::kSettleMs);
        QVERIFY(tracker.takeDue(1000 + InviteNoticeTracker::kSettleMs - 1).isEmpty());
        QCOMPARE(tracker.takeDue(1000 + InviteNoticeTracker::kSettleMs),
                 QStringList{ kRoom });
        QVERIFY(tracker.takeDue(99999).isEmpty());
        QCOMPARE(tracker.nextDueInMs(99999), qint64(-1));
    }

    // Due while it is momentarily off the list: it waits for the room to be
    // back rather than being announced for a room that is not listed.
    void aDueInviteOffTheListWaitsForIt()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom }, true, 1000);
        tracker.observe({}, true, 1990);
        QVERIFY(tracker.takeDue(2000).isEmpty());
        tracker.observe({ kRoom }, true, 2100);
        QCOMPARE(tracker.takeDue(2100), QStringList{ kRoom });
    }

    // Accepted or declined: off the list past the grace. Resolved once, and
    // an invite to the same room after that is new again.
    void aGoneInviteResolvesAfterTheGrace()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom, kOther }, true, 1000);
        QCOMPARE(tracker.takeDue(2000), (QStringList{ kRoom, kOther }));
        tracker.observe({ kOther }, true, 3000);
        QCOMPARE(tracker.nextDueInMs(3000), InviteNoticeTracker::kGoneGraceMs);
        QVERIFY(tracker.takeResolved(3000 + InviteNoticeTracker::kGoneGraceMs - 1).isEmpty());
        QCOMPARE(tracker.takeResolved(3000 + InviteNoticeTracker::kGoneGraceMs),
                 QStringList{ kRoom });
        QVERIFY(tracker.takeResolved(99999).isEmpty());

        tracker.observe({ kOther, kRoom }, true, 100000);
        QCOMPARE(tracker.takeDue(100000 + InviteNoticeTracker::kSettleMs),
                 QStringList{ kRoom });
    }

    // Declined during its settle: never announced.
    void anInviteGoneBeforeItsSettleIsNeverAnnounced()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom }, true, 1000);
        tracker.observe({}, true, 1200);
        QCOMPARE(tracker.takeResolved(1200 + InviteNoticeTracker::kGoneGraceMs),
                 QStringList{ kRoom });
        QVERIFY(tracker.takeDue(99999).isEmpty());
    }

    // Accepted or left, and still listed as that: settled at once, with no
    // grace and no announcement still to come; inviting again is new.
    void aSettledInviteIsForgottenAtOnce()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom }, true, 1000);
        QVERIFY(tracker.forget(kRoom));
        QVERIFY(!tracker.forget(kRoom));
        QVERIFY(!tracker.forget(kOther));
        QVERIFY(tracker.takeDue(99999).isEmpty());
        QVERIFY(tracker.takeResolved(99999).isEmpty());
        QCOMPARE(tracker.nextDueInMs(1000), qint64(-1));
        tracker.observe({ kRoom }, true, 100000);
        QCOMPARE(tracker.takeDue(100000 + InviteNoticeTracker::kSettleMs),
                 QStringList{ kRoom });
    }

    // Invites already there at startup are recorded, never announced, even
    // across a re-sort after initial sync.
    void invitesFromBeforeInitialSyncAreSilent()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom }, false, 1000);
        QVERIFY(tracker.takeDue(99999).isEmpty());
        tracker.observe({}, true, 2000);
        tracker.observe({ kRoom }, true, 2001);
        QVERIFY(tracker.takeDue(99999).isEmpty());
        QVERIFY(tracker.takeResolved(99999).isEmpty());
    }

    // An account change starts over.
    void clearForgetsEverything()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom }, true, 1000);
        tracker.clear();
        QVERIFY(tracker.takeDue(99999).isEmpty());
        QCOMPARE(tracker.nextDueInMs(1000), qint64(-1));
        tracker.observe({ kRoom }, true, 2000);
        QCOMPARE(tracker.takeDue(2000 + InviteNoticeTracker::kSettleMs),
                 QStringList{ kRoom });
    }
};

QTEST_GUILESS_MAIN(InviteNoticeTrackerTest)
#include "InviteNoticeTrackerTest.moc"
