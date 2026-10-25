// The wording and gating of Lightning's own non-message notices, and the
// small decisions AppController takes around them (src/app/
// NotificationNotices.h). Pure: no bus, no window, no session.
//
// What each case pins, and what the code before it did instead:
//  * Private preview reached the message cards only. The call, missed-call
//    and invite notices read the GENERAL level alone (so "In encrypted rooms:
//    Private" never applied to them), and the verification notice read no
//    level at all and printed the other user's full MXID.
//  * The ring was `ringForCalls && notificationSound != Off`, so turning chat
//    chimes off silenced every incoming call.
//  * A per-sender 30 s cooldown dropped a redial outright: no ring, no card,
//    and no missed-call notice.
//  * The thread dedup set was wiped wholesale at 512 entries.
//  * "Mark as read" on a Space cleared the user's own marked-unread rooms.
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
        // An invitation's picture also needs invite_avatars known and on.
        QVERIFY(showsInviteAvatar(0, true));
        QVERIFY(!showsInviteAvatar(0, false));
        QVERIFY(!showsInviteAvatar(priv, true));
    }

    void callWordingNamesTheCallerOnceAndTheKindOfCall()
    {
        using namespace notices;
        // A DM's name is the peer's own name: never "Alice is calling in
        // Alice".
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
        // The missed-call notice names the caller now, and the kind of call.
        QCOMPARE(missedCallBody(0, QStringLiteral("Alice"),
                                QStringLiteral("Alice"), true, true),
                 QStringLiteral("Missed video call from Alice"));
        QCOMPARE(missedCallBody(1, QStringLiteral("Alice"),
                                QStringLiteral("Team"), false, false),
                 QStringLiteral("Missed voice call from Alice in Team"));
        // No caller remembered: the room-only wording that existed before.
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
        // Never a raw room id for a room with no name yet.
        QCOMPARE(inviteBody(0, QString(), QString(), false),
                 QStringLiteral("You were invited to a room"));
    }

    void verificationNamesAPersonNotAnAddress()
    {
        using namespace notices;
        QCOMPARE(verificationBody(0, false, QStringLiteral("alice")),
                 QStringLiteral("alice wants to verify a session. Open "
                                "Lightning to review it."));
        // One of the user's own sessions needs no name at all.
        QVERIFY(!verificationBody(0, true, QStringLiteral("me"))
                     .contains(QStringLiteral("me wants")));
        QVERIFY(verificationNoticeKey(QStringLiteral("f1"))
                != verificationNoticeKey(QStringLiteral("f2")));
    }

    void theRingFollowsTheRingSwitchNotTheMessageSound()
    {
        using notices::ringPlan;
        // The call path never reads the message sound mode any more: these
        // are the only inputs, and "ring for calls" on means it rings.
        auto plan = ringPlan(/*ringForCalls=*/true, /*silenced=*/false,
                             /*ringerAvailable=*/true,
                             /*platformLoopsCallSound=*/true);
        QVERIFY(plan.ring && plan.ownRinger && !plan.cardSound);
        plan = ringPlan(true, false, false, true);
        QVERIFY(plan.ring && !plan.ownRinger && plan.cardSound);
        plan = ringPlan(false, false, true, true);
        QVERIFY(!plan.ring && !plan.ownRinger && !plan.cardSound);
        plan = ringPlan(true, /*silenced=*/true, true, true);
        QVERIFY(!plan.ring && !plan.ownRinger && !plan.cardSound);
    }

    // Decided 2026-10-18 (C2, finding 104): the desktop's Do Not Disturb
    // silences an incoming call, on every delivery path. Old code: the plan
    // had no such input and rang (ownRinger / cardSound true).
    void doNotDisturbSilencesTheRingButNotTheCall()
    {
        for (const bool loops : { true, false }) {
            for (const bool ringerReady : { true, false }) {
                const auto plan = notices::ringPlan(
                    /*ringForCalls=*/true, /*silenced=*/false, ringerReady,
                    loops, /*desktopDoNotDisturb=*/true);
                QVERIFY(!plan.ring);      // nothing to Silence either
                QVERIFY(!plan.ownRinger);
                QVERIFY(!plan.cardSound);
            }
        }
        // Do Not Disturb off: exactly the plan without the argument.
        const auto off = notices::ringPlan(true, false, true, true, false);
        const auto unset = notices::ringPlan(true, false, true, true);
        QCOMPARE(off.ring, unset.ring);
        QCOMPARE(off.ownRinger, unset.ownRinger);
        QCOMPARE(off.cardSound, unset.cardSound);
        QVERIFY(off.ring && off.ownRinger);
    }

    void withoutALoopingDesktopSoundOurOwnRingerAlwaysRuns()
    {
        // Windows / macOS: the balloon cues once at most. Handing the ring to
        // it would ring once (or never) for a whole call.
        auto plan = notices::ringPlan(true, false, /*ringerAvailable=*/false,
                                      /*platformLoopsCallSound=*/false);
        QVERIFY(plan.ring);
        QVERIFY(plan.ownRinger);
        QVERIFY(plan.cardSound); // one cue while the ringer is not ready
        plan = notices::ringPlan(true, false, true, false);
        QVERIFY(plan.ownRinger && !plan.cardSound);
    }

    void aRedialIsAnnouncedAndOnlyABurstIsRefused()
    {
        using V = notices::RingAnnounceLimiter::Verdict;
        notices::RingAnnounceLimiter limiter;
        const QString alice = QStringLiteral("@alice:example.org");
        QCOMPARE(limiter.admit(alice, QStringLiteral("c1"), 1000), V::Announce);
        // The same call again is a refresh, never counted.
        QCOMPARE(limiter.admit(alice, QStringLiteral("c1"), 2000), V::Refresh);
        // Alice gives up and calls again 15 s later: a real call. The code
        // before refused this one (30 s per sender).
        QCOMPARE(limiter.admit(alice, QStringLiteral("c2"), 16000), V::Announce);
        QCOMPARE(limiter.admit(alice, QStringLiteral("c3"), 20000), V::Announce);
        // A fourth fresh id inside the window is a pump, not a redial.
        QCOMPARE(limiter.admit(alice, QStringLiteral("c4"), 25000), V::Refuse);
        // Someone else is unaffected.
        QCOMPARE(limiter.admit(QStringLiteral("@bob:example.org"),
                               QStringLiteral("b1"), 25000),
                 V::Announce);
        // The window slides.
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
        // Unknown rules: the local switch alone, as before.
        QVERIFY(inviteNoticeAllowed(true, false, false, -1));
        QVERIFY(!inviteNoticeAllowed(false, false, false, -1));
        // Known rules: master mute and "Invitations to you: Off" both silence
        // the card. The code before read neither.
        QVERIFY(!inviteNoticeAllowed(true, true, /*masterMute=*/true, 1));
        QVERIFY(!inviteNoticeAllowed(true, true, false, /*inviteRule=*/0));
        QVERIFY(inviteNoticeAllowed(true, true, false, 1));
        QVERIFY(inviteNoticeAllowed(true, true, false, -1));
        QVERIFY(!notices::verificationNoticeAllowed(true, true, true));
        QVERIFY(notices::verificationNoticeAllowed(true, false, true));
    }

    void anInvitationSoundsLikeADirectMessage()
    {
        using notices::inviteSound;
        // Always silent before.
        QVERIFY(inviteSound(/*MentionsAndDirect*/ 1, false, -1));
        QVERIFY(inviteSound(/*All*/ 2, false, -1));
        QVERIFY(!inviteSound(/*Off*/ 0, false, -1));
        QVERIFY(!inviteSound(0, true, 2));
        // Known rules: the rule's own sound tweak decides.
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
        // Over the cap: only the OLDEST goes. A set wiped wholesale (the code
        // before) forgot $b..$d too, so a re-delivered $d notified again.
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
        QVERIFY(!mainWindowCountsAsActive(true, /*minimized=*/true, false));
        QVERIFY(!mainWindowCountsAsActive(true, false, /*hidden=*/true));
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
        // A click with no daemon token falls back to alert() on Wayland only.
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
        QCOMPARE(accountDefaultLevel(true, /*direct=*/true, /*dmAll=*/true,
                                     /*roomsAll=*/false),
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
        QVERIFY(!highlightSummaryAllowed(true, true, /*muted=*/true, false,
                                         false, 2));
        QVERIFY(!highlightSummaryAllowed(true, true, false, /*visible=*/true,
                                         false, 2));
        QVERIFY(!highlightSummaryAllowed(true, true, false, false,
                                         /*masterMute=*/true, 2));
        QVERIFY(!highlightSummaryAllowed(true, true, false, false, false, 0));
        QCOMPARE(notices::highlightSummaryBody(0, 2, QStringLiteral("Ops")),
                 QStringLiteral("2 new mention(s) in Ops"));
        QVERIFY(notices::highlightSummaryKey(QStringLiteral("!a:x"))
                != notices::highlightSummaryKey(QStringLiteral("!b:x")));
    }

    // A call announced without ringing us (P8's callStartedNotice) is a quiet
    // card, and only where a message from that room would be one.
    void aCallThatDoesNotRingUsIsAQuietNoticeWhereAllowed()
    {
        using notices::callStartedNoticeAllowed;
        QVERIFY(callStartedNoticeAllowed(true, true, false, false, false,
                                         false, false));
        QVERIFY(!callStartedNoticeAllowed(false, true, false, false, false,
                                          false, false));
        QVERIFY(!callStartedNoticeAllowed(true, /*sync=*/false, false, false,
                                          false, false, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, /*muted=*/true, false,
                                          false, false, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, false, /*visible=*/true,
                                          false, false, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, false, false,
                                          /*masterMute=*/true, false, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, false, false, false,
                                          /*ignored=*/true, false));
        QVERIFY(!callStartedNoticeAllowed(true, true, false, false, false,
                                          false, /*own=*/true));
        QCOMPARE(notices::callStartedBody(0, QStringLiteral("Alice"),
                                          QStringLiteral("Ops")),
                 QStringLiteral("Alice started a call in Ops"));
        QCOMPARE(notices::callStartedBody(1, QStringLiteral("Alice"), QString()),
                 QStringLiteral("Alice started a call"));
        // Private names nobody and no room.
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
