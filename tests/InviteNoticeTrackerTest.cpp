
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

    void aDueInviteOffTheListWaitsForIt()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom }, true, 1000);
        tracker.observe({}, true, 1990);
        QVERIFY(tracker.takeDue(2000).isEmpty());
        tracker.observe({ kRoom }, true, 2100);
        QCOMPARE(tracker.takeDue(2100), QStringList{ kRoom });
    }

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

    void anInviteGoneBeforeItsSettleIsNeverAnnounced()
    {
        InviteNoticeTracker tracker;
        tracker.observe({ kRoom }, true, 1000);
        tracker.observe({}, true, 1200);
        QCOMPARE(tracker.takeResolved(1200 + InviteNoticeTracker::kGoneGraceMs),
                 QStringList{ kRoom });
        QVERIFY(tracker.takeDue(99999).isEmpty());
    }

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
