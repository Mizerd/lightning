#include <QtTest/QtTest>

#include "app/NotificationNotices.h"

class NotificationNoticesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void privateWithholdsEveryIdentityFromEveryNotice()
    {
        using namespace notices;
        const int priv = kPreviewPrivate;
        const QString alice = QStringLiteral("Alice");
        const QString room = QStringLiteral("Secret Room");
        for (const QString &body :
             { incomingCallBody(priv, alice, room, false, false),
               incomingCallBody(priv, alice, room, true, true),
               missedCallBody(priv, alice, room, false, false),
               missedCallBody(priv, alice, room, true, true),
               inviteBody(priv, alice, room, false),
               inviteBody(priv, alice, room, true),
               verificationBody(priv, false, alice),
               highlightSummaryBody(priv, 3, room) }) {
            QVERIFY2(!body.contains(alice) && !body.contains(room),
                     qPrintable(QStringLiteral("Private leaked an identity: ")
                                + body));
            QVERIFY(!body.isEmpty());
        }
        QVERIFY(!showsRoomAvatar(priv));
        QVERIFY(showsRoomAvatar(0));
        QVERIFY(showsRoomAvatar(1));
        QVERIFY(showsInviteAvatar(0, true));
        QVERIFY(!showsInviteAvatar(0, false));
        QVERIFY(!showsInviteAvatar(priv, true));
    }

    void callWordingNamesTheCallerOnceAndTheKindOfCall()
    {
        using namespace notices;
        QCOMPARE(incomingCallBody(0, QStringLiteral("Alice"),
                                  QStringLiteral("Alice"), true, false),
                 QStringLiteral("Alice is calling"));
        QCOMPARE(incomingCallBody(0, QStringLiteral("Alice"),
                                  QStringLiteral("Team"), false, false),
                 QStringLiteral("Alice is calling in Team"));
        QCOMPARE(incomingCallTitle(true), QStringLiteral("Incoming video call"));
        QCOMPARE(incomingCallTitle(false), QStringLiteral("Incoming call"));
        QCOMPARE(incomingCallBody(kPreviewPrivate, QString(), QString(), false,
                                  true),
                 QStringLiteral("Incoming video call"));
        QCOMPARE(missedCallBody(0, QStringLiteral("Alice"),
                                QStringLiteral("Alice"), true, true),
                 QStringLiteral("Missed video call from Alice"));
        QCOMPARE(missedCallBody(1, QStringLiteral("Alice"),
                                QStringLiteral("Team"), false, false),
                 QStringLiteral("Missed voice call from Alice in Team"));
        QCOMPARE(missedCallBody(0, QString(), QStringLiteral("Team"), false,
                                false),
                 QStringLiteral("You missed a voice call in Team"));
    }

    void inviteWordingNamesTheInviter()
    {
        using namespace notices;
        QCOMPARE(inviteBody(0, QStringLiteral("Bob"), QStringLiteral("Ops"),
                            false),
                 QStringLiteral("Bob invited you to Ops"));
        QCOMPARE(inviteBody(0, QStringLiteral("Bob"), QStringLiteral("Bob"),
                            true),
                 QStringLiteral("Bob wants to chat with you"));
        QCOMPARE(inviteBody(0, QString(), QStringLiteral("Ops"), false),
                 QStringLiteral("You were invited to Ops"));
        QCOMPARE(inviteBody(0, QString(), QString(), false),
                 QStringLiteral("You were invited to a room"));
    }

    void verificationNamesAPersonNotAnAddress()
    {
        using namespace notices;
        QCOMPARE(verificationBody(0, false, QStringLiteral("alice")),
                 QStringLiteral("alice wants to verify a session. Open "
                                "Lightning to review it."));
        QVERIFY(!verificationBody(0, true, QStringLiteral("me"))
                     .contains(QStringLiteral("me wants")));
        QVERIFY(verificationNoticeKey(QStringLiteral("f1"))
                != verificationNoticeKey(QStringLiteral("f2")));
    }

    void theRingFollowsTheRingSwitchNotTheMessageSound()
    {
        using notices::ringPlan;
        auto plan = ringPlan(true,false,
true,
true);
        QVERIFY(plan.ring && plan.ownRinger && !plan.cardSound);
        plan = ringPlan(true, false, false, true);
        QVERIFY(plan.ring && !plan.ownRinger && plan.cardSound);
        plan = ringPlan(false, false, true, true);
        QVERIFY(!plan.ring && !plan.ownRinger && !plan.cardSound);
        plan = ringPlan(true,true, true, true);
        QVERIFY(!plan.ring && !plan.ownRinger && !plan.cardSound);
    }

    void doNotDisturbSilencesTheRingButNotTheCall()
    {
        for (const bool loops : { true, false }) {
            for (const bool ringerReady : { true, false }) {
                const auto plan = notices::ringPlan(
true,false, ringerReady,
                    loops,true);
                QVERIFY(!plan.ring);
                QVERIFY(!plan.ownRinger);
                QVERIFY(!plan.cardSound);
            }
        }
        const auto off = notices::ringPlan(true, false, true, true, false);
        const auto unset = notices::ringPlan(true, false, true, true);
        QCOMPARE(off.ring, unset.ring);
        QCOMPARE(off.ownRinger, unset.ownRinger);
        QCOMPARE(off.cardSound, unset.cardSound);
        QVERIFY(off.ring && off.ownRinger);
    }

    void withoutALoopingDesktopSoundOurOwnRingerAlwaysRuns()
    {
        auto plan = notices::ringPlan(true, false,false,
false);
        QVERIFY(plan.ring);
        QVERIFY(plan.ownRinger);
        QVERIFY(plan.cardSound);
        plan = notices::ringPlan(true, false, true, false);
        QVERIFY(plan.ownRinger && !plan.cardSound);
    }

    void aRedialIsAnnouncedAndOnlyABurstIsRefused()
    {
        using V = notices::RingAnnounceLimiter::Verdict;
        notices::RingAnnounceLimiter limiter;
        const QString alice = QStringLiteral("@alice:example.org");
        QCOMPARE(limiter.admit(alice, QStringLiteral("c1"), 1000), V::Announce);
        QCOMPARE(limiter.admit(alice, QStringLiteral("c1"), 2000), V::Refresh);
        QCOMPARE(limiter.admit(alice, QStringLiteral("c2"), 16000), V::Announce);
        QCOMPARE(limiter.admit(alice, QStringLiteral("c3"), 20000), V::Announce);
        QCOMPARE(limiter.admit(alice, QStringLiteral("c4"), 25000), V::Refuse);
        QCOMPARE(limiter.admit(QStringLiteral("@bob:example.org"),
                               QStringLiteral("b1"), 25000),
                 V::Announce);
        QCOMPARE(limiter.admit(alice, QStringLiteral("c5"), 31001), V::Announce);
    }

    void theLimiterStaysBounded()
    {
        notices::RingAnnounceLimiter limiter;
        for (int i = 0; i < 500; ++i) {
            limiter.admit(QStringLiteral("@u%1:x").arg(i),
                          QStringLiteral("c%1").arg(i), i);
        }
        QVERIFY(limiter.senderCountForTest()
                <= notices::RingAnnounceLimiter::kMaxSenders);
    }

    void invitesFollowTheAccountsRules()
    {
        using notices::inviteNoticeAllowed;
        QVERIFY(inviteNoticeAllowed(true, false, false, -1));
        QVERIFY(!inviteNoticeAllowed(false, false, false, -1));
        QVERIFY(!inviteNoticeAllowed(true, true,true, 1));
        QVERIFY(!inviteNoticeAllowed(true, true, false,0));
        QVERIFY(inviteNoticeAllowed(true, true, false, 1));
        QVERIFY(inviteNoticeAllowed(true, true, false, -1));
        QVERIFY(!notices::verificationNoticeAllowed(true, true, true));
        QVERIFY(notices::verificationNoticeAllowed(true, false, true));
    }

    void anInvitationSoundsLikeADirectMessage()
    {
        using notices::inviteSound;
        QVERIFY(inviteSound( 1, false, -1));
        QVERIFY(inviteSound( 2, false, -1));
        QVERIFY(!inviteSound( 0, false, -1));
        QVERIFY(!inviteSound(0, true, 2));
        QVERIFY(inviteSound(1, true, 2));
        QVERIFY(!inviteSound(1, true, 1));
    }

    void anOldNotificationIsForgottenOneAtATime()
    {
        notices::RecentIds ids(4);
        QVERIFY(ids.insert(QStringLiteral("$a")));
        QVERIFY(!ids.insert(QStringLiteral("$a")));
        QVERIFY(ids.insert(QStringLiteral("$b")));
        QVERIFY(ids.insert(QStringLiteral("$c")));
        QVERIFY(ids.insert(QStringLiteral("$d")));
        QVERIFY(ids.insert(QStringLiteral("$e")));
        QCOMPARE(ids.size(), 4);
        QVERIFY(!ids.contains(QStringLiteral("$a")));
        QVERIFY(ids.contains(QStringLiteral("$b")));
        QVERIFY(!ids.insert(QStringLiteral("$d")));
    }

    void aSpaceMarkReadLeavesTheUsersOwnUnreadMarks()
    {
        RoomInfo room;
        room.id = QStringLiteral("!r:x");
        room.membership = RoomInfo::Joined;
        room.unreadCount = 3;
        QVERIFY(notices::roomWantsMarkRead(room));
        room.markedUnread = true;
        QVERIFY(!notices::roomWantsMarkRead(room));
        room.markedUnread = false;
        room.unreadCount = 0;
        QVERIFY2(!notices::roomWantsMarkRead(room),
                 "a room with nothing unread still got a receipt");
        room.hasUnreadMessages = true;
        QVERIFY(notices::roomWantsMarkRead(room));
        room.membership = RoomInfo::Invited;
        QVERIFY(!notices::roomWantsMarkRead(room));
        room.membership = RoomInfo::Joined;
        room.isSpace = true;
        QVERIFY(!notices::roomWantsMarkRead(room));
    }

    void onlyAFocusedVisibleMainWindowReadsMessages()
    {
        using notices::mainWindowCountsAsActive;
        QVERIFY(mainWindowCountsAsActive(true, false, false));
        QVERIFY(!mainWindowCountsAsActive(false, false, false));
        QVERIFY(!mainWindowCountsAsActive(true,true, false));
        QVERIFY(!mainWindowCountsAsActive(true, false,true));
    }

    void waylandNeverGetsAnAlertThatBecomesAnActivation()
    {
        using notices::clickNeedsAlertActivation;
        using notices::windowAlertAllowed;
        QVERIFY(!windowAlertAllowed(QStringLiteral("wayland")));
        QVERIFY(!windowAlertAllowed(QStringLiteral("wayland-egl")));
        QVERIFY(windowAlertAllowed(QStringLiteral("xcb")));
        QVERIFY(windowAlertAllowed(QStringLiteral("windows")));
        QVERIFY(windowAlertAllowed(QStringLiteral("cocoa")));
        QVERIFY(clickNeedsAlertActivation(QStringLiteral("wayland"), false));
        QVERIFY(!clickNeedsAlertActivation(QStringLiteral("wayland"), true));
        QVERIFY(!clickNeedsAlertActivation(QStringLiteral("xcb"), false));
    }

    void theTrayNamesTheAccountWithoutItsAddress()
    {
        using notices::trayAccountLabel;
        QCOMPARE(trayAccountLabel(QString(), QStringLiteral("Ghost")), QString());
        QCOMPARE(trayAccountLabel(QStringLiteral("@rokas:smetonis.net"),
                                  QString()),
                 QStringLiteral("rokas"));
        QCOMPARE(trayAccountLabel(QStringLiteral("@rokas:smetonis.net"),
                                  QStringLiteral("  Rokas ")),
                 QStringLiteral("Rokas"));
    }

    void theTroubleshooterGetsTheDefaultForTheRoomsKind()
    {
        using notices::accountDefaultLevel;
        QCOMPARE(accountDefaultLevel(false, true, true, true), -1);
        QCOMPARE(accountDefaultLevel(true,true,true,
false),
                 0);
        QCOMPARE(accountDefaultLevel(true, false, true, false), 1);
        QCOMPARE(accountDefaultLevel(true, false, false, true), 0);
    }

    void aFailedSeedRetriesWithABoundedBackoff()
    {
        using notices::activitySeedRetryDelayMs;
        QCOMPARE(activitySeedRetryDelayMs(0), 5000);
        QCOMPARE(activitySeedRetryDelayMs(1), 10000);
        QVERIFY(activitySeedRetryDelayMs(5) <= 300000);
        QCOMPARE(activitySeedRetryDelayMs(6), -1);
        QCOMPARE(activitySeedRetryDelayMs(-1), -1);
    }

    void aMissedMentionSummaryRespectsTheRoomsState()
    {
        using notices::highlightSummaryAllowed;
        QVERIFY(highlightSummaryAllowed(true, true, false, false, false, 2));
        QVERIFY(!highlightSummaryAllowed(true, false, false, false, false, 2));
        QVERIFY(!highlightSummaryAllowed(true, true,true, false,
                                         false, 2));
        QVERIFY(!highlightSummaryAllowed(true, true, false,true,
                                         false, 2));
        QVERIFY(!highlightSummaryAllowed(true, true, false, false,
true, 2));
        QVERIFY(!highlightSummaryAllowed(true, true, false, false, false, 0));
        QCOMPARE(notices::highlightSummaryBody(0, 2, QStringLiteral("Ops")),
                 QStringLiteral("2 new mention(s) in Ops"));
        QVERIFY(notices::highlightSummaryKey(QStringLiteral("!a:x"))
                != notices::highlightSummaryKey(QStringLiteral("!b:x")));
    }

    void aCallThatDoesNotRingUsIsAQuietNoticeWhereAllowed()
    {
        using notices::callStartedNoticeAllowed;
        QVERIFY(callStartedNoticeAllowed(true, true, false, false, false,
                                         false, false));
        QVERIFY(!callStartedNoticeAllowed(false, true, false, false, false,
                                          false, false));
        QVERIFY(!callStartedNoticeAllowed(true,false, false, false,
                                          false, false, false));
        QVERIFY(!callStartedNoticeAllowed(true, true,true, false,
                                          false, false, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, false,true,
                                          false, false, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, false, false,
true, false, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, false, false, false,
true, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, false, false, false,
                                          false,true));
        QCOMPARE(notices::callStartedBody(0, QStringLiteral("Alice"),
                                          QStringLiteral("Ops")),
                 QStringLiteral("Alice started a call in Ops"));
        QCOMPARE(notices::callStartedBody(1, QStringLiteral("Alice"), QString()),
                 QStringLiteral("Alice started a call"));
        const QString hidden = notices::callStartedBody(
            notices::kPreviewPrivate, QStringLiteral("Alice"),
            QStringLiteral("Ops"));
        QVERIFY(!hidden.contains(QStringLiteral("Alice")));
        QVERIFY(!hidden.contains(QStringLiteral("Ops")));
        QVERIFY(notices::callStartedKey(QStringLiteral("!a:x"))
                != notices::callStartedKey(QStringLiteral("!b:x")));
        QVERIFY(notices::callStartedKey(QStringLiteral("!a:x"))
                != notices::highlightSummaryKey(QStringLiteral("!a:x")));
    }
};

QTEST_GUILESS_MAIN(NotificationNoticesTest)
#include "NotificationNoticesTest.moc"
