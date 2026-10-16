// The pure notification decision policy: privacy modes, mention handling,
// per-room modes, active-room suppression, and never exposing ciphertext. No
// DBus, no homeserver.

#include "notifications/NotificationManager.h"

#include "app/TrayIcon.h"
// threadTimelineId() is static inline in this header, so cases build a real
// composite timeline id instead of hard-coding the separator.
#include "matrix/MatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QColor>
#include <QImage>
#include <QMetaMethod>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QtTest/QtTest>

#include <cmath>

namespace {
TimelineEvent incomingText(const QString &body = QStringLiteral("hello"))
{
    TimelineEvent event;
    event.eventId = QStringLiteral("$ev:example.org");
    event.roomId = QStringLiteral("!room:example.org");
    event.sender = QStringLiteral("@bob:example.org");
    event.senderDisplayName = QStringLiteral("Bob");
    event.body = body;
    event.type = TimelineEvent::TextMessage;
    event.status = TimelineEvent::Sent;
    return event;
}

TimelineEvent withVerdict(TimelineEvent event,
                          TimelineEvent::PushVerdict verdict,
                          bool highlight = false)
{
    event.pushVerdict = verdict;
    event.pushHighlight = highlight;
    return event;
}

NotificationManager::Context baseContext()
{
    NotificationManager::Context context;
    context.selfUserId = QStringLiteral("@alice:example.org");
    context.roomName = QStringLiteral("Lightning Dev");
    context.previewMode = NotificationManager::SenderOnly;
    context.notificationsEnabled = true;
    return context;
}

// A name as a card shows it: first-strong isolated (FSI ... PDI), so its own
// direction cannot reorder the sentence around it.
QString iso(const QString &name)
{
    return QChar(0x2068) + name + QChar(0x2069);
}

QVariantMap messagePayload(const QString &room, const QString &eventId,
                           const QString &threadRootId = QString(),
                           const QString &account = QString())
{
    return QVariantMap{ { QStringLiteral("roomId"), room },
                        { QStringLiteral("eventId"), eventId },
                        { QStringLiteral("threadRootId"), threadRootId },
                        { QStringLiteral("accountUserId"), account } };
}
} // namespace

class NotificationManagerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readingARoomWithdrawsItsGhostNotifications();
    // Notification actions.
    void anActionCarriesTheAccountTheCardWasRaisedFor();
    void anInlineReplyKeepsThePayloadUntilTheTextArrives();
    void anEmptyInlineReplySendsNothing();
    void withdrawingSparesTheIncomingCallRing();
    void anExpiredNotificationStaysWithdrawableUntilTheRoomIsRead();
    void aTrayBalloonClickOpensTheRoomItWasRaisedFor();
    void signingOutForgetsTheTrayBalloonsClick();
    void readingARoomDropsAPopupStillWaitingForItsAvatar();
    void aThreadReplyNotifiesForItsRoomNotItsTimelineId();
    void aTrayBalloonClickNeverRoutesToATimelineId();
    void aNotificationWithNoRoomStillBringsTheWindowForward();
    void everySlotTheseCasesDriveByNameStillExists();
    // Whose sound a notification makes, and the hint the daemon is sent.
    void theSoundPlanSuppressesThePlatformUnlessItWasChosen();
    void soundHintsAreSuppressSoundOrTheThemedNameNeverBoth();
    void aDeliveredNotificationPlaysLightningsChimeOnce();
    void theChimeFollowsThePolicyThatDecidesTheNotification();
    void aMentionPlaysTheMentionChime();
    void theSystemDefaultSourcePlaysNoChimeOfOurs();
    void aTrayBalloonAllowsThePlatformSoundOnlyWhenSystemDefaultWasChosen();
    void aCallBalloonIsSilentWhileLightningRings();
    // 2026-10-15 notification sweep (package P6). Each names its finding.
    void anActionOnACardFromBeforeSignOutReachesTheRefusal();
    void anActionOnACardThisSessionNeverRaisedIsLeftAlone();
    void theReadWalkSparesThreadRepliesAndNotices();
    void anUncheckedRoomMentionIsNotAMention();
    void everyMessageCardIsNormalUrgency();
    void theDesktopsDoNotDisturbSilencesTheChime();
    void aChimeThatCannotPlayFallsBackToTheDesktopsSound();
    void theCallPolicyHoldsForTheDesktopsSoundToo();
    void aMentionBreaksThroughAnOrdinaryBurst();
    void theRulesSoundTweakDecidesTheDefaultSoundMode();
    void aPathThatCannotSoundPlaysOursForSystemDefault();
    void aGenericNoticeCanSound();
    void anUndecryptableMessageWaitsBrieflyForItsKey();
    void aMentionThatArrivesUndecryptableStillNotifies();
    void aLateDecryptionNeverRaisesASecondBalloon();
    void aDecryptionOfHistoryRaisesNothing();
    void aRoomOpensHistoryNeverNotifiesWhenItDecrypts();
    void aRedactedMessageNeitherNotifiesNorStays();
    void withoutRulesEditsAndNoticesStayQuiet();
    void anEmoteNamesItsActor();
    void aSpoilerStaysHidden();
    void namesAreIsolatedInTheSentence();
    void aRoomsSecondMessageUpdatesItsCard();
    void theSnapIdentityIsOneTheHostCanResolve();
    void aClickHandsTheActivationTokenToTheWindow();
    void theSilentCallCardSuppressesTheDesktopsSound();
    void theSameCallIsNeverBalloonedTwice();
    void dismissingTheCallCardStopsTheRinger();
    void anExpiredCallCardIsKeptForWithdrawal();
    void aMissedCallSurvivesTheReadWalk();
    void twoRoomsOfBalloonsOpenNeitherByGuess();
    void aDeliveredMessageAsksForAttention();
    // Live accounts, Phase 0 (WP0-N): per-account tables, with the foreground
    // ("") account exactly as before.
    void anEmptyAccountKeepsTodaysPayloadsEvictionAndTombstones();
    void clearPendingIsExactlyTheForegroundAccountsClear();
    void everyOpenNamesItsAccountFirst();
    void clearingOneAccountSparesAnothersCards();
    void aSixtyFifthCardEvictsOnlyThatAccountsOldest();
    void aClearOfOneAccountDoesNotRejectAnothersReply();
    void aBalloonCarriesTheAccountItWasRaisedFor();
    // Live Windows test 2026-10-16 (FAIL 2c, and the title's sender).
    void aSecondRoomWaitsForTheBalloonOnScreen();
    void waitingBalloonsGoOutAsOneSummary();
    void aMentionBreaksThroughTheBalloonOnScreen();
    void aMentionWaitsBehindTheCallBalloon();
    void withoutWithdrawalEveryBalloonShowsAndTwoRoomsOpenHome();
    void anAccountClearEmptiesOnlyItsBalloons();
    void aDisplacedBalloonBringsBackNothingReadOrCleared();
    void aReadRoomLeavesTheBalloonSummary();
    void theBalloonHoldFollowsTheNotificationDuration();
    void theSenderIsNamedTheSameWhicheverPathDeliveredIt();
    void directMessageNotifiesWithSenderOnlyDefault()
    {
        const auto decision =
            NotificationManager::decide(incomingText(), baseContext());
        QVERIFY(decision.notify);
        QCOMPARE(decision.title, iso(QStringLiteral("Lightning Dev")));
        // Sender only: sender and room, but never the message body.
        QVERIFY(decision.body.contains(QStringLiteral("Bob")));
        QVERIFY(!decision.body.contains(QStringLiteral("hello")));
    }

    void senderAndMessageShowsBoundedPreview()
    {
        auto context = baseContext();
        context.previewMode = NotificationManager::SenderAndMessage;
        const auto decision =
            NotificationManager::decide(incomingText(), context);
        QVERIFY(decision.notify);
        QVERIFY(decision.title.contains(QStringLiteral("Bob")));
        QVERIFY(decision.title.contains(QStringLiteral("Lightning Dev")));
        QCOMPARE(decision.body, QStringLiteral("hello"));

        // Direct rooms title with the sender alone.
        context.roomIsDirect = true;
        const auto dm = NotificationManager::decide(incomingText(), context);
        QCOMPARE(dm.title, iso(QStringLiteral("Bob")));
    }

    void pollAndMultilineBodiesStayOneLine()
    {
        auto context = baseContext();
        context.previewMode = NotificationManager::SenderAndMessage;

        // Poll: question only, never the MSC3381 answer-list fallback.
        TimelineEvent poll = incomingText(
            QStringLiteral("Best answer?\n1. Yes\n2. No\n3. Big Money"));
        poll.type = TimelineEvent::Poll;
        poll.pollQuestion = QStringLiteral("Best answer?");
        const auto pollDecision = NotificationManager::decide(poll, context);
        QCOMPARE(pollDecision.body, QStringLiteral("Poll: Best answer?"));

        // Multi-line text bodies collapse; mention markdown reduces to its
        // label.
        const auto text = NotificationManager::decide(
            incomingText(QStringLiteral(
                "[@test](https://matrix.to/#/%40test%3Ax) hi\nthere")),
            context);
        QCOMPARE(text.body, QStringLiteral("@test hi there"));
    }

    void privateModeIsGeneric()
    {
        auto context = baseContext();
        context.previewMode = NotificationManager::Private;
        const auto decision =
            NotificationManager::decide(incomingText(), context);
        QVERIFY(decision.notify);
        QCOMPARE(decision.title, QStringLiteral("Lightning"));
        QVERIFY(!decision.body.contains(QStringLiteral("Bob")));
        QVERIFY(!decision.body.contains(QStringLiteral("hello")));
        QVERIFY(!decision.body.contains(QStringLiteral("Lightning Dev")));
    }

    // Undecryptable events never leak anything, even in the fullest mode.
    void undecryptableEventStaysGeneric()
    {
        auto context = baseContext();
        context.previewMode = NotificationManager::SenderAndMessage;
        TimelineEvent event = incomingText(
            QStringLiteral("[unable to decrypt yet]"));
        event.isEncrypted = true;
        event.undecryptable = true;
        const auto decision = NotificationManager::decide(event, context);
        QVERIFY(decision.notify);
        QVERIFY(!decision.body.contains(QStringLiteral("unable to decrypt")));
        QVERIFY(decision.body.contains(QStringLiteral("Encrypted")));
    }

    void mentionMetadataDrivesMentionsOnlyMode()
    {
        auto context = baseContext();
        context.roomMode = NotificationManager::MentionsOnly;

        // Plain message: suppressed.
        QVERIFY(!NotificationManager::decide(incomingText(), context).notify);

        // Direct mention (m.mentions metadata, not substring matching).
        TimelineEvent mention = incomingText(
            QStringLiteral("without your name in the body"));
        mention.mentionsMe = true;
        const auto decision = NotificationManager::decide(mention, context);
        QVERIFY(decision.notify);
        QVERIFY(decision.body.contains(QStringLiteral("mentioned")));

        // m.mentions.room alone does not: without the account's rules nothing
        // here checks the sender's @room power level (finding 47).
        TimelineEvent roomMention = incomingText();
        roomMention.mentionsRoom = true;
        QVERIFY2(!NotificationManager::decide(roomMention, context).notify,
                 "an unchecked m.mentions.room passed a mentions-only room");
        // The rules' highlight is what admits an @room the sender may use.
        roomMention.pushVerdict = TimelineEvent::PushVerdict::Notify;
        roomMention.pushHighlight = true;
        QVERIFY(NotificationManager::decide(roomMention, context).notify);
    }

    void mutedRoomNeverNotifies()
    {
        auto context = baseContext();
        context.roomMode = NotificationManager::Muted;
        TimelineEvent mention = incomingText();
        mention.mentionsMe = true;
        QVERIFY(!NotificationManager::decide(mention, context).notify);
    }

    // Issue #15: "Mentions & keywords" chosen on another client, or as the
    // account default, never reaches this device's local mode. The SDK's
    // push-rule verdict carries it, and a quiet verdict is final.
    void aQuietVerdictSilencesWhatTheLocalModeWouldAllow()
    {
        using Verdict = TimelineEvent::PushVerdict;
        for (const auto mode : {NotificationManager::AllMessages,
                                NotificationManager::FollowDefault}) {
            auto context = baseContext();
            context.roomMode = mode;
            QVERIFY2(!NotificationManager::decide(
                          withVerdict(incomingText(), Verdict::Quiet), context)
                          .notify,
                     "a room the account's rules keep quiet notified");
            QVERIFY(NotificationManager::decide(
                        withVerdict(incomingText(), Verdict::Notify), context)
                        .notify);
        }
        // The same for a thread reply and a DM.
        auto dm = baseContext();
        dm.roomIsDirect = true;
        TimelineEvent reply = incomingText();
        reply.threadRootId = QStringLiteral("$root:example.org");
        QVERIFY(!NotificationManager::decide(
                     withVerdict(reply, Verdict::Quiet), dm).notify);
    }

    // The rules already decided a mention or keyword notifies; the local
    // mentions-only filter must not second-guess them. matrix-sdk writes a
    // keyword rule without a highlight, so there is nothing else to go on.
    void aKeywordTheRulesNotifyForPassesALocalMentionsOnlyRoom()
    {
        using Verdict = TimelineEvent::PushVerdict;
        auto context = baseContext();
        context.roomMode = NotificationManager::MentionsOnly;
        const TimelineEvent keyword =
            withVerdict(incomingText(QStringLiteral("banana bread")),
                        Verdict::Notify);
        QVERIFY2(NotificationManager::decide(keyword, context).notify,
                 "a keyword the account's rules notify for was dropped");
        // A quiet verdict stays quiet there too.
        QVERIFY(!NotificationManager::decide(
                     withVerdict(incomingText(), Verdict::Quiet), context)
                     .notify);
    }

    // A highlight is a mention: keywords and mentions without m.mentions get
    // the mention chime. Whether a sound plays at all follows the rule's
    // sound tweak (findings 48/105), as the default rules give every
    // highlight one.
    void aHighlightSoundsLikeAMention()
    {
        using Verdict = TimelineEvent::PushVerdict;
        auto context = baseContext();
        context.soundMode = NotificationManager::SoundMentionsAndDirect;
        const auto plain = NotificationManager::decide(
            withVerdict(incomingText(), Verdict::Notify), context);
        QVERIFY(plain.notify);
        QVERIFY(!plain.playSound);
        QVERIFY(!plain.mention);
        TimelineEvent loud = withVerdict(incomingText(), Verdict::Notify, true);
        loud.pushSound = true;
        const auto highlighted = NotificationManager::decide(loud, context);
        QVERIFY(highlighted.notify);
        QVERIFY(highlighted.playSound);
        QVERIFY(highlighted.mention);
    }

    // A local mute applies at once, before the server echoes the rule, and a
    // mute is what people notice failing.
    void aLocalMuteSilencesWhateverTheRulesSay()
    {
        using Verdict = TimelineEvent::PushVerdict;
        auto context = baseContext();
        context.roomMode = NotificationManager::Muted;
        TimelineEvent mention =
            withVerdict(incomingText(), Verdict::Notify, true);
        mention.mentionsMe = true;
        QVERIFY(!NotificationManager::decide(mention, context).notify);
    }

    // A failed write left the server on the old rules, and the UI promises
    // the choice is kept on this device: the local mode decides alone.
    void anUnsyncedLocalModeDecidesAlone()
    {
        using Verdict = TimelineEvent::PushVerdict;
        auto context = baseContext();
        context.localModeUnsynced = true;
        context.roomMode = NotificationManager::AllMessages;
        QVERIFY(NotificationManager::decide(
                    withVerdict(incomingText(), Verdict::Quiet), context)
                    .notify);
        context.roomMode = NotificationManager::MentionsOnly;
        QVERIFY(!NotificationManager::decide(
                     withVerdict(incomingText(), Verdict::Notify), context)
                     .notify);
        TimelineEvent mention = withVerdict(incomingText(), Verdict::Quiet);
        mention.mentionsMe = true;
        QVERIFY(NotificationManager::decide(mention, context).notify);
    }

    // A notifying verdict opens no other gate.
    void aNotifyVerdictBypassesNoOtherGate()
    {
        using Verdict = TimelineEvent::PushVerdict;
        const TimelineEvent loud =
            withVerdict(incomingText(), Verdict::Notify, true);

        auto visible = baseContext();
        visible.roomVisibleAtLatest = true;
        QVERIFY(!NotificationManager::decide(loud, visible).notify);

        auto hydrating = baseContext();
        hydrating.roomHydrating = true;
        QVERIFY(!NotificationManager::decide(loud, hydrating).notify);

        auto backlog = baseContext();
        backlog.initialSyncComplete = false;
        QVERIFY(!NotificationManager::decide(loud, backlog).notify);

        auto ignored = baseContext();
        ignored.senderIsIgnored = true;
        QVERIFY(!NotificationManager::decide(loud, ignored).notify);

        auto disabled = baseContext();
        disabled.notificationsEnabled = false;
        QVERIFY(!NotificationManager::decide(loud, disabled).notify);

        TimelineEvent own = loud;
        own.sender = baseContext().selfUserId;
        QVERIFY(!NotificationManager::decide(own, baseContext()).notify);
    }

    void activeRoomAtLatestSuppresses()
    {
        auto context = baseContext();
        context.roomVisibleAtLatest = true;
        QVERIFY(!NotificationManager::decide(incomingText(), context).notify);
        // Scrolled away / unfocused: notify again.
        context.roomVisibleAtLatest = false;
        QVERIFY(NotificationManager::decide(incomingText(), context).notify);
    }

    // An open room that is still hydrating suppresses notifications: opening
    // a room subscribes it in sliding sync, so its recent history arrives as
    // live appends before the view can be "visible at latest".
    void hydratingOpenRoomSuppresses()
    {
        auto context = baseContext();
        context.roomVisibleAtLatest = false; // the view has not settled yet
        context.roomHydrating = true;
        QVERIFY(!NotificationManager::decide(incomingText(), context).notify);
        // A mention in the room being read is suppressed too; it is on screen
        // once the view settles.
        TimelineEvent mention = incomingText();
        mention.mentionsMe = true;
        QVERIFY(!NotificationManager::decide(mention, context).notify);
        // Once hydration ends, an ordinary background room notifies again.
        context.roomHydrating = false;
        QVERIFY(NotificationManager::decide(incomingText(), context).notify);
    }

    void ownLocalAndNonMessageEventsNeverNotify()
    {
        const auto context = baseContext();

        TimelineEvent own = incomingText();
        own.sender = context.selfUserId;
        QVERIFY(!NotificationManager::decide(own, context).notify);

        TimelineEvent echo = incomingText();
        echo.isLocalEcho = true;
        echo.status = TimelineEvent::Sending;
        QVERIFY(!NotificationManager::decide(echo, context).notify);

        TimelineEvent state = incomingText();
        state.type = TimelineEvent::StateChange;
        QVERIFY(!NotificationManager::decide(state, context).notify);

        TimelineEvent divider;
        divider.type = TimelineEvent::DateDivider;
        QVERIFY(!NotificationManager::decide(divider, context).notify);

        auto disabled = baseContext();
        disabled.notificationsEnabled = false;
        QVERIFY(!NotificationManager::decide(incomingText(), disabled).notify);
    }

    // Thread replies (hidden from the main timeline) reach notifications via
    // the sync-level handler, follow the same policy as any message, and carry
    // the thread root in their click payload.
    void threadRepliesCarryThreadIdentity()
    {
        auto context = baseContext();
        TimelineEvent reply = incomingText(QStringLiteral("thread reply"));
        reply.threadRootId = QStringLiteral("$root:example.org");

        // Inactive (background) room: notifies like any message.
        QVERIFY(NotificationManager::decide(reply, context).notify);

        // Muted room: never.
        context.roomMode = NotificationManager::Muted;
        QVERIFY(!NotificationManager::decide(reply, context).notify);

        // Mentions-only: a plain thread reply is silent, a mentioning one is
        // not.
        context.roomMode = NotificationManager::MentionsOnly;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
        TimelineEvent mentioningReply = reply;
        mentioningReply.mentionsMe = true;
        QVERIFY(NotificationManager::decide(mentioningReply, context).notify);

        // Initial-sync backlog: never.
        context.roomMode = NotificationManager::AllMessages;
        context.initialSyncComplete = false;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
        context.initialSyncComplete = true;

        // Active, visible-at-latest room: suppressed.
        context.roomVisibleAtLatest = true;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
    }

    // A thread reply's click payload carries the thread root, so activation
    // opens the thread, not just the room.
    void threadReplyClickRoutesToThread()
    {
        NotificationManager manager;
        QSignalSpy spy(&manager, &NotificationManager::openRequested);
        QVariantMap p;
        p.insert(QStringLiteral("roomId"), QStringLiteral("!room:example.org"));
        p.insert(QStringLiteral("eventId"), QStringLiteral("$reply:example.org"));
        p.insert(QStringLiteral("threadRootId"),
                 QStringLiteral("$root:example.org"));
        manager.recordPayloadForTest(7, p);
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 7u),
                                  Q_ARG(QString, QStringLiteral("default")));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("!room:example.org"));
        QCOMPARE(spy.first().at(2).toString(), QStringLiteral("$root:example.org"));
    }

    // Events applied before the initial sync completes are backlog, never
    // fresh activity, whatever the mode or mention state.
    void initialSyncBacklogNeverNotifies()
    {
        auto context = baseContext();
        context.initialSyncComplete = false;
        QVERIFY(!NotificationManager::decide(incomingText(), context).notify);

        // Even a direct mention during backlog stays silent.
        TimelineEvent mention = incomingText();
        mention.mentionsMe = true;
        QVERIFY(!NotificationManager::decide(mention, context).notify);

        // Once the sync is live the same event notifies.
        context.initialSyncComplete = true;
        QVERIFY(NotificationManager::decide(incomingText(), context).notify);
    }

    // A room's recent history re-delivered after the initial sync (its first
    // subscription on this device, or a gap) is backlog too, whatever the push
    // verdict, mention or room mode. Measured 2026-09-29: opening a room on a
    // new device raised one popup per event, 20, on 0.9.9 and on #15.
    void aResyncedBacklogNeverNotifiesWhateverTheVerdict()
    {
        auto context = baseContext();
        TimelineEvent history = withVerdict(
            incomingText(), TimelineEvent::PushVerdict::Notify, true);
        history.mentionsMe = true;
        history.backlog = true;
        QVERIFY(!NotificationManager::decide(history, context).notify);

        // Without a verdict (the local mode would decide) and with a mode
        // saved only on this device.
        history.pushVerdict = TimelineEvent::PushVerdict::Unknown;
        QVERIFY(!NotificationManager::decide(history, context).notify);
        context.localModeUnsynced = true;
        QVERIFY(!NotificationManager::decide(history, context).notify);

        // Nothing else was holding it back: the same event as a new arrival
        // notifies.
        history.backlog = false;
        QVERIFY(NotificationManager::decide(history, context).notify);
    }

    // The card for a new message can arrive before the room list shows the
    // room unread, and the list's walk read that stale "read" as the user
    // reading the room: the card was withdrawn 0.3-0.6 s after it appeared.
    void aReadThatPredatesTheCardDoesNotWithdrawIt()
    {
        NotificationManager manager;
        const QString room = QStringLiteral("!room:example.org");
        manager.processEvent(incomingText(), baseContext());
        QVERIFY(manager.roomAwaitsUnreadForTest(room));
        // Stands in for the Notify reply (no daemon here).
        manager.recordPayloadForTest(
            7, QVariantMap{ { QStringLiteral("roomId"), room },
                            { QStringLiteral("eventId"),
                              QStringLiteral("$ev:example.org") },
                            { QStringLiteral("threadRootId"), QString() } });

        // The list has not caught up: the room still reads as read.
        manager.observeRoomUnread(room, false);
        QCOMPARE(manager.pendingPayloadCountForTest(), 1);
        // It catches up, and then the user reads the room.
        manager.observeRoomUnread(room, true);
        QVERIFY(!manager.roomAwaitsUnreadForTest(room));
        QCOMPARE(manager.pendingPayloadCountForTest(), 1);
        manager.observeRoomUnread(room, false);
        QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    }

    // Review B3: a room read here or elsewhere before the list ever showed it
    // unread would have kept its card until the next unread-then-read. The
    // wait for the list is bounded.
    void theWaitForTheListIsBounded()
    {
        NotificationManager manager;
        manager.setAwaitUnreadMsForTest(50);
        const QString room = QStringLiteral("!room:example.org");
        manager.processEvent(incomingText(), baseContext());
        manager.recordPayloadForTest(
            7, QVariantMap{ { QStringLiteral("roomId"), room },
                            { QStringLiteral("eventId"),
                              QStringLiteral("$ev:example.org") },
                            { QStringLiteral("threadRootId"), QString() } });
        manager.observeRoomUnread(room, false);
        QCOMPARE(manager.pendingPayloadCountForTest(), 1);
        QTest::qWait(100);
        manager.observeRoomUnread(room, false);
        QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    }

    // What the walk must still do: a card whose room was read on another
    // client goes when the list says so, and an explicit read (mark-read,
    // reply) closes at once even before the list caught up.
    void aReadRoomStillWithdrawsItsCard()
    {
        NotificationManager manager;
        const QString room = QStringLiteral("!elsewhere:example.org");
        const QVariantMap payload{ { QStringLiteral("roomId"), room },
                                   { QStringLiteral("eventId"),
                                     QStringLiteral("$e:example.org") },
                                   { QStringLiteral("threadRootId"),
                                     QString() } };
        // Raised earlier; the list has shown it unread since.
        manager.recordPayloadForTest(1, payload);
        manager.observeRoomUnread(room, false);
        QCOMPARE(manager.pendingPayloadCountForTest(), 0);

        TimelineEvent event = incomingText();
        event.roomId = room;
        manager.processEvent(event, baseContext());
        manager.recordPayloadForTest(2, payload);
        QVERIFY(manager.roomAwaitsUnreadForTest(room));
        manager.closeRoomNotifications(room);
        QCOMPARE(manager.pendingPayloadCountForTest(), 0);
        QVERIFY(!manager.roomAwaitsUnreadForTest(room));

        // Sign-out forgets which rooms were waiting.
        manager.processEvent(event, baseContext());
        manager.clearPending();
        QVERIFY(!manager.roomAwaitsUnreadForTest(room));
    }

    // Pending invites present at launch are seeded silently; only invites seen
    // after the initial sync (and not yet announced) notify.
    void invitePolicySuppressesBacklog()
    {
        // During initial sync: never notify, even for a brand-new invite.
        QVERIFY(!NotificationManager::shouldNotifyInvite(
            /*initialSyncComplete=*/false, /*alreadyKnown=*/false,
            /*notificationsEnabled=*/true));
        // After sync, a genuinely new invite notifies once.
        QVERIFY(NotificationManager::shouldNotifyInvite(true, false, true));
        // Already-announced invites never re-notify.
        QVERIFY(!NotificationManager::shouldNotifyInvite(true, true, true));
        // Notifications disabled: never.
        QVERIFY(!NotificationManager::shouldNotifyInvite(true, false, false));
    }

    // Sound rides on the notify decision, and the sound mode narrows which
    // eligible notifications also sound.
    void soundModeGatesEligibleNotifications()
    {
        auto ctx = baseContext();

        // Off: never sounds, even for a mention.
        ctx.soundMode = NotificationManager::SoundOff;
        TimelineEvent mention = incomingText();
        mention.mentionsMe = true;
        QVERIFY(!NotificationManager::decide(mention, ctx).playSound);

        // Mentions & DMs: a plain non-DM message notifies but is silent…
        ctx.soundMode = NotificationManager::SoundMentionsAndDirect;
        const auto plain = NotificationManager::decide(incomingText(), ctx);
        QVERIFY(plain.notify);
        QVERIFY(!plain.playSound);
        // …a mention sounds…
        QVERIFY(NotificationManager::decide(mention, ctx).playSound);
        // …and a direct message sounds.
        auto dm = baseContext();
        dm.soundMode = NotificationManager::SoundMentionsAndDirect;
        dm.roomIsDirect = true;
        QVERIFY(NotificationManager::decide(incomingText(), dm).playSound);

        // All: every eligible notification sounds.
        ctx.soundMode = NotificationManager::SoundAll;
        QVERIFY(NotificationManager::decide(incomingText(), ctx).playSound);
    }

    // Suppression that stops the notification also stops the sound.
    void suppressedNotificationsNeverSound()
    {
        auto ctx = baseContext();
        ctx.soundMode = NotificationManager::SoundAll;

        // Muted room: no notify, no sound.
        auto muted = ctx;
        muted.roomMode = NotificationManager::Muted;
        QVERIFY(!NotificationManager::decide(incomingText(), muted).playSound);

        // Active room at latest: no notify, no sound.
        auto active = ctx;
        active.roomVisibleAtLatest = true;
        QVERIFY(!NotificationManager::decide(incomingText(), active).playSound);

        // Backlog during initial sync: no notify, no sound.
        auto backlog = ctx;
        backlog.initialSyncComplete = false;
        QVERIFY(!NotificationManager::decide(incomingText(), backlog).playSound);

        // Mentions-only room, plain message: no notify, no sound.
        auto mentionsOnly = ctx;
        mentionsOnly.roomMode = NotificationManager::MentionsOnly;
        QVERIFY(!NotificationManager::decide(incomingText(), mentionsOnly)
                     .playSound);
    }

    // The bounded payload map evicts the oldest entries (FIFO) rather than
    // clearing everything, so recent notifications stay clickable.
    void clickPayloadEvictionIsFifo()
    {
        NotificationManager manager;
        QSignalSpy spy(&manager, &NotificationManager::openRequested);

        auto payload = [](const QString &room) {
            QVariantMap p;
            p.insert(QStringLiteral("roomId"), room);
            p.insert(QStringLiteral("eventId"), QStringLiteral("$e:example.org"));
            p.insert(QStringLiteral("threadRootId"), QString{});
            return p;
        };

        // Record 66 payloads (ids 1..66), two past the cap of 64.
        for (quint32 id = 1; id <= 66; ++id)
            manager.recordPayloadForTest(id, payload(QStringLiteral("!r%1")
                                                         .arg(id)));
        QCOMPARE(manager.pendingPayloadCountForTest(), 64);

        // The two oldest (ids 1, 2) were evicted, and withdrawn with it. A
        // click on one a daemon still shows raises the window but names no
        // room (finding 24): its room is no longer known.
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 1u),
                                  Q_ARG(QString, QStringLiteral("default")));
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.takeFirst().at(0).toString().isEmpty());

        // A recent notification (id 66) still routes to its room.
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 66u),
                                  Q_ARG(QString, QStringLiteral("default")));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("!r66"));
        // Consumed on activation.
        QCOMPARE(manager.pendingPayloadCountForTest(), 63);

        // clearPending() drops everything (logout/account switch).
        manager.clearPending();
        QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    }

    // The incoming-call ring. With no DBus daemon here, Notify() never runs;
    // these pin the state machine around it: timer lifecycle, id-matched
    // decline and close, replacement, deadline, and teardown.

    void callRingTimerFollowsSoundAndStops()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"),
                                 /*sound=*/true, /*ringSeconds=*/60);
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-1"));
        QVERIFY(manager.callRingActiveForTest());
        // Mismatched id is a no-op; empty id retires whatever is active.
        manager.stopIncomingCall(QStringLiteral("other-call"));
        QVERIFY(manager.callRingActiveForTest());
        manager.stopIncomingCall(QString());
        QVERIFY(!manager.callRingActiveForTest());
        QCOMPARE(manager.activeCallIdForTest(), QString());

        // Silent card (ringForCalls off): no repeat timer.
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-2"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"),
                                 /*sound=*/false, /*ringSeconds=*/60);
        QVERIFY(!manager.callRingActiveForTest());
        manager.stopIncomingCall(QStringLiteral("call-2"));
    }

    void newerCallReplacesTheOlderRing()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!a:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60);
        manager.showIncomingCall(QStringLiteral("!b:x"),
                                 QStringLiteral("call-2"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60);
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-2"));
        // Stopping the OLD id no longer touches the new ring.
        manager.stopIncomingCall(QStringLiteral("call-1"));
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-2"));
        QVERIFY(manager.callRingActiveForTest());
        manager.stopIncomingCall(QStringLiteral("call-2"));
    }

    void ringDeadlineRetiresTheCard()
    {
        NotificationManager manager;
        // ringSeconds is clamped to >= 5 and the timer interval is 5 s, so the
        // first tick lands at or after the deadline.
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true,
                                 /*ringSeconds=*/-100 /* clamps to 5 */);
        QVERIFY(manager.callRingActiveForTest());
        // First tick at ~5 s: at or after the deadline, so it retires.
        QTest::qWait(5100);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.callRingActiveForTest(), 7000);
        QCOMPARE(manager.activeCallIdForTest(), QString());
    }

    // The exact action list of the incoming-call card (Join/Answer, Silence,
    // Decline). `callActions` is extracted so it can be tested without a
    // daemon; the whole list is asserted, not just `contains`.
    void theCallCardOffersAnswerAndDecline()
    {
        // MatrixRTC ring: the verb is Join, since an RTC call belongs to the
        // room and may already be in progress.
        QCOMPARE(NotificationManager::callActions(/*acceptOffered=*/true,
                                                  /*rtcLane=*/true),
                 (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                               QStringLiteral("accept"), QObject::tr("Join"),
                               QStringLiteral("decline"),
                               QObject::tr("Decline") }));
        // Legacy 1:1 invite: the verb is Answer.
        QCOMPARE(NotificationManager::callActions(true, /*rtcLane=*/false),
                 (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                               QStringLiteral("accept"),
                               QObject::tr("Answer"),
                               QStringLiteral("decline"),
                               QObject::tr("Decline") }));
        // Not answerable (no media backend, or the join is blocked): no accept
        // key at all.
        QCOMPARE(NotificationManager::callActions(/*acceptOffered=*/false,
                                                  false),
                 (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                               QStringLiteral("decline"),
                               QObject::tr("Decline") }));
        // Silence goes before Decline, and only when offered.
        QCOMPARE(NotificationManager::callActions(true, /*rtcLane=*/false,
                                                  /*silenceOffered=*/true),
                 (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                               QStringLiteral("accept"),
                               QObject::tr("Answer"),
                               QStringLiteral("silence"),
                               QObject::tr("Silence"),
                               QStringLiteral("decline"),
                               QObject::tr("Decline") }));
        // Decline is last in every shape, so a mis-aimed click is least likely
        // to reach it.
        int shapes = 0;
        for (bool offered : { true, false }) {
            for (bool rtc : { true, false }) {
                for (bool silence : { true, false }) {
                    const QStringList a = NotificationManager::callActions(
                        offered, rtc, silence);
                    QCOMPARE(a.at(a.size() - 2), QStringLiteral("decline"));
                    QCOMPARE(a.contains(QStringLiteral("silence")), silence);
                    ++shapes;
                }
            }
        }
        QCOMPARE(shapes, 8);
    }

    // Silence is id-matched like accept/decline, retires nothing, and is
    // honoured only on a card that offered it.
    void silenceActionMatchesTheDeliveredIdAndEndsNothing()
    {
        NotificationManager manager;
        QSignalSpy silenced(&manager,
                            &NotificationManager::callSilenceRequested);
        QSignalSpy declined(&manager,
                            &NotificationManager::callDeclineRequested);
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60,
                                 false, false, /*silenceOffered=*/true);
        manager.setActiveCallNotificationIdForTest(42);
        for (const QString &crafted :
             { QStringLiteral("Silence"), QStringLiteral("silence "),
               QStringLiteral("silencecall-1"), QStringLiteral("") }) {
            QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 42u),
                                      Q_ARG(QString, crafted));
        }
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 7u),
                                  Q_ARG(QString, QStringLiteral("silence")));
        QCOMPARE(silenced.count(), 0);
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 42u),
                                  Q_ARG(QString, QStringLiteral("silence")));
        QCOMPARE(silenced.count(), 1);
        QCOMPARE(silenced.first().at(0).toString(), QStringLiteral("call-1"));
        // Nothing ended and nothing was declined; the card is still up.
        QCOMPARE(declined.count(), 0);
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-1"));

        // A card that did not offer Silence does not honour one.
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-2"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), false, 60);
        manager.setActiveCallNotificationIdForTest(43);
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 43u),
                                  Q_ARG(QString, QStringLiteral("silence")));
        QCOMPARE(silenced.count(), 1);
        manager.stopIncomingCall(QString());
    }

    // Silencing stops the themed sound and its 5 s re-post, keeps the card,
    // and nothing later brings the sound back.
    void silenceIncomingCallQuietensTheCardAndKeepsIt()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60,
                                 true, false, /*silenceOffered=*/true);
        QVERIFY(manager.callRingActiveForTest());
        QVERIFY(manager.callSoundActiveForTest());
        // Another call's id is not this card's.
        manager.silenceIncomingCall(QStringLiteral("call-0"));
        manager.silenceIncomingCall(QString());
        QVERIFY(manager.callRingActiveForTest());
        QVERIFY(manager.callSoundActiveForTest());

        manager.silenceIncomingCall(QStringLiteral("call-1"));
        QVERIFY(!manager.callRingActiveForTest());
        QVERIFY(!manager.callSoundActiveForTest());
        QVERIFY(!manager.callSilenceOfferedForTest());
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-1"));
        // A redraw for the join gate must not resurrect the sound.
        manager.setCallAcceptOffered(QStringLiteral("call-1"), false, false);
        QVERIFY(!manager.callSoundActiveForTest());
        QVERIFY(!manager.callRingActiveForTest());
        manager.stopIncomingCall(QString());
        QVERIFY(!manager.callSilenceOfferedForTest());
    }

    // Accept is id-matched like decline: an action for a stale notification
    // id must not answer a call no longer offered.
    void acceptActionMatchesTheDeliveredId()
    {
        NotificationManager manager;
        QSignalSpy accepted(&manager,
                            &NotificationManager::callAcceptRequested);
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60,
                                 /*acceptOffered=*/true, /*rtcLane=*/true);
        manager.setActiveCallNotificationIdForTest(42);
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 7u),
                                  Q_ARG(QString, QStringLiteral("accept")));
        QCOMPARE(accepted.count(), 0);
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 42u),
                                  Q_ARG(QString, QStringLiteral("accept")));
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(accepted.first().at(0).toString(),
                 QStringLiteral("call-1"));
        // Retired before the signal: a re-delivery racing the answer would put
        // a dead card back up.
        QVERIFY(!manager.callRingActiveForTest());
        QCOMPARE(manager.activeCallIdForTest(), QString());
    }

    void declineActionMatchesTheDeliveredId()
    {
        NotificationManager manager;
        QSignalSpy declined(&manager,
                            &NotificationManager::callDeclineRequested);
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60);
        manager.setActiveCallNotificationIdForTest(42);
        // A decline on some other notification id is not ours.
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 7u),
                                  Q_ARG(QString, QStringLiteral("decline")));
        QCOMPARE(declined.count(), 0);
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 42u),
                                  Q_ARG(QString, QStringLiteral("decline")));
        QCOMPARE(declined.count(), 1);
        QCOMPARE(declined.first().at(0).toString(),
                 QStringLiteral("call-1"));
        // Retired before the signal, so reentrant stop calls are no-ops.
        QVERIFY(!manager.callRingActiveForTest());
        QCOMPARE(manager.activeCallIdForTest(), QString());
    }

    void daemonClosingTheCardStopsReRingWithoutDeclining()
    {
        NotificationManager manager;
        QSignalSpy declined(&manager,
                            &NotificationManager::callDeclineRequested);
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60);
        manager.setActiveCallNotificationIdForTest(42);
        QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                                  Q_ARG(quint32, 42u), Q_ARG(quint32, 2u));
        QVERIFY(!manager.callRingActiveForTest());
        QCOMPARE(declined.count(), 0); // dismissal is not an answer
        manager.stopIncomingCall(QString());
    }

    // An incoming call is announced where there is no freedesktop daemon, via
    // the tray balloon like every other notification. Run on a DBus build with
    // no reachable bus (pinned by the CMake ENVIRONMENT), so the fallback path
    // actually executes.
    void anIncomingCallIsAnnouncedWhereThereIsNoFreedesktopDaemon()
    {
        NotificationManager manager;
        QCOMPARE(manager.callTrayAttemptsForTest(), 0);
        manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("bob is calling in Room"),
                                 /*sound=*/true, /*ringSeconds=*/60,
                                 /*acceptOffered=*/true, /*rtcLane=*/true);
        QVERIFY2(manager.callTrayAttemptsForTest() == 1,
                 "an incoming call raised with no reachable notification "
                 "service never reached the tray balloon: on Windows and "
                 "macOS that is the ONLY delivery there is, so the call was "
                 "not announced on the desktop at all");
        // The click identity routes to the ringing room, where
        // IncomingCallPrompt offers Answer.
        QCOMPARE(manager.lastCallTrayPayloadForTest()
                     .value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!ring:example.org"));
        // Never a composite timeline id, nor an event id to jump to.
        QCOMPARE(manager.lastCallTrayPayloadForTest()
                     .value(QStringLiteral("eventId")).toString(),
                 QString());
        manager.stopIncomingCall(QStringLiteral("call-1"));
    }

    // A balloon cannot be replaced in place like a freedesktop card, so the
    // call balloon is raised once per call, not on every 5 s ring tick.
    void theCallBalloonIsRaisedOncePerCallNotOncePerRingTick()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"),
                                 /*sound=*/true, /*ringSeconds=*/60);
        QCOMPARE(manager.callTrayAttemptsForTest(), 1);
        for (int i = 0; i < 3; ++i)
            QVERIFY(QMetaObject::invokeMethod(&manager, "onCallRingTick"));
        QCOMPARE(manager.callTrayAttemptsForTest(), 1);
        // ...and the next call gets its own.
        manager.stopIncomingCall(QStringLiteral("call-1"));
        manager.showIncomingCall(QStringLiteral("!other:example.org"),
                                 QStringLiteral("call-2"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"),
                                 /*sound=*/true, /*ringSeconds=*/60);
        QCOMPARE(manager.callTrayAttemptsForTest(), 2);
        QCOMPARE(manager.lastCallTrayPayloadForTest()
                     .value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!other:example.org"));
        manager.stopIncomingCall(QStringLiteral("call-2"));
    }

    void clearPendingRetiresTheRing()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60);
        QVERIFY(manager.callRingActiveForTest());
        manager.clearPending();
        QVERIFY(!manager.callRingActiveForTest());
        QCOMPARE(manager.activeCallIdForTest(), QString());
    }

    void recordPayloadPromotesRefreshedIds()
    {
        NotificationManager manager;
        QVariantMap p;
        p.insert(QStringLiteral("roomId"), QStringLiteral("!call:x"));
        p.insert(QStringLiteral("eventId"), QString());
        p.insert(QStringLiteral("threadRootId"), QString());
        // The call card's id is recorded first, 63 message payloads follow,
        // then the card re-delivers under the same id. The re-record must
        // promote it, so the next eviction takes a stale message payload.
        manager.recordPayloadForTest(100, p);
        for (quint32 id = 1; id <= 63; ++id)
            manager.recordPayloadForTest(id, p);
        manager.recordPayloadForTest(100, p); // the 5s re-delivery
        manager.recordPayloadForTest(200, p); // one more: evicts id 1
        QCOMPARE(manager.pendingPayloadCountForTest(), 64);
        QSignalSpy spy(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 100u),
                                  Q_ARG(QString, QStringLiteral("default")));
        QCOMPARE(spy.count(), 1); // the call card survived the eviction
    }

    // The macOS menu-bar badge. A status item takes a template image (AppKit
    // uses only alpha), so the badge disc is written into the alpha channel
    // and the digit knocked out of it. The rule is compiled everywhere so it
    // can be tested off a Mac.
    void theMacMenuBarBadgeIsCutIntoTheAlphaChannel()
    {
        // A fully opaque base: every transparent pixel in the result was
        // produced by this function.
        QPixmap base(44, 44);
        base.fill(QColor(0, 0, 0, 255));

        const QImage plain =
            TrayIcon::macTemplateBadged(base, QString{}).toImage();
        QCOMPARE(plain.pixelColor(43, 43).alpha(), 255);

        const QImage badged =
            TrayIcon::macTemplateBadged(base, QStringLiteral("3"))
                .toImage()
                .convertToFormat(QImage::Format_ARGB32);

        // Geometry from the production constants.
        const qreal side = 44.0;
        const qreal diameter = side * 0.62;
        const QRectF disc(side - diameter, side - diameter,
                          diameter, diameter);
        const QPointF hub = disc.center();
        const qreal radius = diameter / 2.0;
        const qreal moat = qMax<qreal>(1.0, side * 0.08);
        auto alphaAt = [&](qreal x, qreal y) {
            return badged.pixelColor(qRound(x), qRound(y)).alpha();
        };

        // 1. The disc is opaque (sampled off-centre, away from the digit).
        QVERIFY2(alphaAt(hub.x() + radius * 0.7, hub.y()) > 200,
                 "the badge disc is not opaque, so the menu bar would paint "
                 "nothing where the count should be");

        // 2. The digit is cleared, not painted: a template image keeps no
        //    colour, so white ink on the disc would vanish.
        int cleared = 0;
        for (int y = 0; y < badged.height(); ++y) {
            for (int x = 0; x < badged.width(); ++x) {
                const QPointF from(x - hub.x(), y - hub.y());
                if (QPointF::dotProduct(from, from)
                    > radius * radius * 0.81)
                    continue;
                if (badged.pixelColor(x, y).alpha() < 40)
                    ++cleared;
            }
        }
        QVERIFY2(cleared > 0,
                 "nothing was knocked out of the badge disc, so the count "
                 "is a featureless blob in the menu bar");

        // 3. A moat separates the badge from the mark beneath it. Walked along
        //    the diagonal, since antialiasing owns the band's edges.
        bool gap = false;
        for (qreal d = radius + 0.5; d < radius + moat && !gap; d += 0.5) {
            const qreal offset = d / std::sqrt(2.0);
            gap = alphaAt(hub.x() - offset, hub.y() - offset) < 40;
        }
        QVERIFY2(gap,
                 "the badge touches the mark it sits on, which in a "
                 "template image is one merged blob");

        // 4. The dot form has no knockout.
        const QImage dot =
            TrayIcon::macTemplateBadged(base, QStringLiteral("\u2022"))
                .toImage()
                .convertToFormat(QImage::Format_ARGB32);
        const qreal dotDiameter = side * 0.42;
        const QPointF dotHub =
            QRectF(side - dotDiameter, side - dotDiameter,
                   dotDiameter, dotDiameter).center();
        QCOMPARE(dot.pixelColor(qRound(dotHub.x()), qRound(dotHub.y()))
                     .alpha(),
                 255);

        // 5. An empty label changes nothing.
        QCOMPARE(TrayIcon::macTemplateBadged(base, QString{}).size(),
                 base.size());
    }
};


// Reading a room (here or on another client) withdraws its delivered
// notifications, so the notification area does not claim unread messages
// that were read.
void NotificationManagerTest::readingARoomWithdrawsItsGhostNotifications()
{
    NotificationManager manager;
    const auto payload = [](const QString &room) {
        QVariantMap p;
        p.insert(QStringLiteral("roomId"), room);
        p.insert(QStringLiteral("eventId"), QStringLiteral("$e:example.org"));
        p.insert(QStringLiteral("threadRootId"), QString{});
        return p;
    };
    manager.recordPayloadForTest(1, payload(QStringLiteral("!read:x")));
    manager.recordPayloadForTest(2, payload(QStringLiteral("!read:x")));
    manager.recordPayloadForTest(3, payload(QStringLiteral("!other:x")));
    QCOMPARE(manager.pendingPayloadCountForTest(), 3);

    manager.closeRoomNotifications(QStringLiteral("!read:x"));

    // The read room's notifications are gone; another room's are untouched.
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    // An unknown room is a no-op, not a clear-everything.
    manager.closeRoomNotifications(QStringLiteral("!nothing:x"));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    manager.closeRoomNotifications(QString{});
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
}

// The call ring is not closed because its room was read.
void NotificationManagerTest::withdrawingSparesTheIncomingCallRing()
{
    NotificationManager manager;
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!ringing:x"));
    p.insert(QStringLiteral("eventId"), QString{});
    p.insert(QStringLiteral("threadRootId"), QString{});
    manager.recordPayloadForTest(9, p);
    manager.setActiveCallNotificationIdForTest(9);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    manager.closeRoomNotifications(QStringLiteral("!ringing:x"));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
}


// Notification actions carry the account the card was raised for, and the
// app compares it against the live account: a card can outlive an account
// switch or sign-out, and a reply under the wrong identity would succeed.

void NotificationManagerTest::anActionCarriesTheAccountTheCardWasRaisedFor()
{
    NotificationManager manager;
    manager.setAccountUserId(QStringLiteral("@ann:example.org"));

    TimelineEvent event;
    event.roomId = QStringLiteral("!room:example.org");
    event.eventId = QStringLiteral("$msg:example.org");
    event.sender = QStringLiteral("@bob:example.org");
    event.body = QStringLiteral("hello");
    auto context = baseContext();
    manager.processEvent(event, context);

    // No DBus daemon, so stand in for the Notify() reply as the other action
    // cases do.
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!room:example.org"));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$msg:example.org"));
    p.insert(QStringLiteral("threadRootId"), QString());
    p.insert(QStringLiteral("accountUserId"), QStringLiteral("@ann:example.org"));
    manager.recordPayloadForTest(11, p);

    // The account changes while the card is on screen.
    manager.setAccountUserId(QStringLiteral("@bea:example.org"));

    QSignalSpy spy(&manager, &NotificationManager::markReadRequested);
    QMetaObject::invokeMethod(&manager, "onActionInvoked",
                              Q_ARG(quint32, 11u),
                              Q_ARG(QString, QStringLiteral("mark-read")));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toString(), QStringLiteral("@ann:example.org"));
    QVERIFY2(spy.first().at(0).toString() != manager.accountUserId(),
             "the action must report the account it was RAISED for, not the "
             "one that happens to be live when it arrives");
    QCOMPARE(spy.first().at(1).toString(), QStringLiteral("!room:example.org"));
}

// Some daemons deliver an inline reply in two parts (ActionInvoked, then
// NotificationReplied with the text), so the payload is kept until the text
// arrives.
void NotificationManagerTest::anInlineReplyKeepsThePayloadUntilTheTextArrives()
{
    NotificationManager manager;
    manager.setAccountUserId(QStringLiteral("@ann:example.org"));
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!room:example.org"));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$msg:example.org"));
    p.insert(QStringLiteral("threadRootId"), QStringLiteral("$root:example.org"));
    p.insert(QStringLiteral("accountUserId"), QStringLiteral("@ann:example.org"));
    manager.recordPayloadForTest(12, p);

    QMetaObject::invokeMethod(&manager, "onActionInvoked",
                              Q_ARG(quint32, 12u),
                              Q_ARG(QString, QStringLiteral("inline-reply")));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    QSignalSpy spy(&manager, &NotificationManager::replyRequested);
    QMetaObject::invokeMethod(&manager, "onNotificationReplied",
                              Q_ARG(quint32, 12u),
                              Q_ARG(QString, QStringLiteral("  on my way  ")));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toString(), QStringLiteral("@ann:example.org"));
    QCOMPARE(spy.first().at(1).toString(), QStringLiteral("!room:example.org"));
    // A reply to a threaded message goes to that thread.
    QCOMPARE(spy.first().at(2).toString(), QStringLiteral("$root:example.org"));
    QCOMPARE(spy.first().at(3).toString(), QStringLiteral("on my way"));
    // Consumed: a second submission sends nothing.
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

void NotificationManagerTest::anEmptyInlineReplySendsNothing()
{
    NotificationManager manager;
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!room:example.org"));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$msg:example.org"));
    p.insert(QStringLiteral("accountUserId"), QStringLiteral("@ann:example.org"));
    manager.recordPayloadForTest(13, p);

    QSignalSpy spy(&manager, &NotificationManager::replyRequested);
    QMetaObject::invokeMethod(&manager, "onNotificationReplied",
                              Q_ARG(quint32, 13u),
                              Q_ARG(QString, QStringLiteral("   ")));
    QCOMPARE(spy.count(), 0);
}

// An expired notification (freedesktop reason 1, kept in KDE's history) stays
// withdrawable until its room is read; dismissed and closed ones are
// forgotten.
void NotificationManagerTest::anExpiredNotificationStaysWithdrawableUntilTheRoomIsRead()
{
    NotificationManager manager;
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!late:x"));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$e:example.org"));
    p.insert(QStringLiteral("threadRootId"), QString{});
    manager.recordPayloadForTest(7, p);
    manager.recordPayloadForTest(8, p);
    manager.recordPayloadForTest(9, p);

    QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                              Q_ARG(quint32, 7u), Q_ARG(quint32, 1u)); // expired
    QCOMPARE(manager.pendingPayloadCountForTest(), 3);
    QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                              Q_ARG(quint32, 8u), Q_ARG(quint32, 2u)); // dismissed
    QCOMPARE(manager.pendingPayloadCountForTest(), 2);
    QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                              Q_ARG(quint32, 9u), Q_ARG(quint32, 3u)); // closed by us
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    // Reading the room withdraws the expired one from the history.
    manager.closeRoomNotifications(QStringLiteral("!late:x"));
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

// The tray balloon is the delivery on Windows and macOS. One balloon at a
// time, so a click routes to the payload delivered last.
void NotificationManagerTest::aTrayBalloonClickOpensTheRoomItWasRaisedFor()
{
    NotificationManager manager;
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!tray:x"));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$t:example.org"));
    p.insert(QStringLiteral("threadRootId"), QStringLiteral("$root:example.org"));
    manager.deliverThroughTrayForTest(p);

    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("!tray:x"));
    QCOMPARE(opened.at(0).at(1).toString(), QStringLiteral("$t:example.org"));
    QCOMPARE(opened.at(0).at(2).toString(), QStringLiteral("$root:example.org"));
    // The balloon is consumed: a second click names no room, but still brings
    // the window forward (finding 35: a click that did nothing at all read as
    // a broken notification).
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 2);
    QVERIFY(opened.at(1).at(0).toString().isEmpty());
}

// clearPending() (sign-out / account switch) also forgets the tray balloon's
// payload, which lives outside the id-keyed map; otherwise a click routes into
// the previous account's room.
void NotificationManagerTest::signingOutForgetsTheTrayBalloonsClick()
{
    NotificationManager manager;
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!previous:x"));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$old:example.org"));
    p.insert(QStringLiteral("threadRootId"), QString{});
    manager.deliverThroughTrayForTest(p);

    // Sign out or switch accounts while the balloon is on screen.
    manager.clearPending();

    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    // The click raises the window, and names no room.
    QCOMPARE(opened.count(), 1);
    QVERIFY2(opened.first().at(0).toString().isEmpty(),
             "a tray balloon raised for the previous account still opened its "
             "room after clearPending(): the sign-out sweep does not forget "
             "the balloon's click payload");
}

// A popup still parked waiting for its room avatar is dropped when the room
// is read, not shown a moment later.
void NotificationManagerTest::readingARoomDropsAPopupStillWaitingForItsAvatar()
{
    NotificationManager manager;
    // An avatar that never arrives or fails parks the delivery in the wait
    // queue, as a slow media fetch does.
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [](const QString &) { return false; });

    auto context = baseContext();
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");
    manager.processEvent(incomingText(), context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);

    // A second room's notification is waiting too and must survive.
    TimelineEvent other = incomingText();
    other.roomId = QStringLiteral("!other:example.org");
    other.eventId = QStringLiteral("$ev2:example.org");
    manager.processEvent(other, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 2);

    manager.closeRoomNotifications(QStringLiteral("!room:example.org"));
    QVERIFY2(manager.avatarWaitCountForTest() == 1,
             "reading a room left its not-yet-shown notification queued, so it "
             "pops up after the room has already been read");

    // The other room is untouched.
    manager.closeRoomNotifications(QStringLiteral("!other:example.org"));
    QCOMPARE(manager.avatarWaitCountForTest(), 0);
}


// A thread reply's notification names its real room, not the composite
// thread timeline id the diff pipeline stamps into TimelineEvent::roomId.
// The payload feeds app.currentRoomId, sendThreadReply(), markRoomRead() and
// closeRoomNotifications(), all of which need a real room id. Driven through
// processEvent() and observed on the delivered payload.
void NotificationManagerTest::aThreadReplyNotifiesForItsRoomNotItsTimelineId()
{
    const QString room = QStringLiteral("!room:example.org");
    const QString root = QStringLiteral("$root:example.org");
    const QString timelineId = MatrixClient::threadTimelineId(room, root);
    QVERIFY2(MatrixClient::isThreadTimelineId(timelineId),
             "the fixture did not build a composite timeline id, so this "
             "case cannot see the defect it exists for");

    NotificationManager manager;
    // An avatar that never arrives parks the delivery, the only way to read a
    // built payload without a live daemon.
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [](const QString &) { return false; });

    auto context = baseContext();
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");

    TimelineEvent reply = incomingText();
    reply.roomId = timelineId;          // exactly what the thread diff emits
    reply.threadRootId = root;
    manager.processEvent(reply, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);

    const QVariantMap payload = manager.avatarWaitPayloadForTest(0);
    QVERIFY2(payload.value(QStringLiteral("roomId")).toString() == room,
             qPrintable(QStringLiteral(
                 "the notification's click payload carries the composite "
                 "timeline id (%1) instead of the room id: clicking it sets "
                 "app.currentRoomId to something that is not a room, so the "
                 "room opens and never loads")
                 .arg(payload.value(QStringLiteral("roomId")).toString())));
    QCOMPARE(payload.value(QStringLiteral("threadRootId")).toString(), root);

    // Because the payload names the real room, reading it withdraws the card.
    manager.closeRoomNotifications(room);
    QVERIFY2(manager.avatarWaitCountForTest() == 0,
             "reading the room did not withdraw its thread notification, "
             "because the payload's room id is not the room's id");

    // A thread copy without its root still opens the thread (the composite
    // carries the root).
    TimelineEvent rootless = incomingText(QStringLiteral("second"));
    rootless.eventId = QStringLiteral("$ev2:example.org");
    rootless.roomId = timelineId;
    rootless.threadRootId.clear();
    manager.processEvent(rootless, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QCOMPARE(manager.avatarWaitPayloadForTest(0)
                 .value(QStringLiteral("threadRootId")).toString(), root);

    // The ordinary case is unchanged: a real room id contains no unit
    // separator.
    manager.closeRoomNotifications(room);
    TimelineEvent plain = incomingText();
    plain.eventId = QStringLiteral("$ev3:example.org");
    manager.processEvent(plain, context);
    QCOMPARE(manager.avatarWaitPayloadForTest(0)
                 .value(QStringLiteral("roomId")).toString(), room);
    QVERIFY(manager.avatarWaitPayloadForTest(0)
                .value(QStringLiteral("threadRootId")).toString().isEmpty());
}

// The same property for the tray balloon: openRequested's first argument is
// assigned to app.currentRoomId by Main.qml, so it is never a timeline id.
void NotificationManagerTest::aTrayBalloonClickNeverRoutesToATimelineId()
{
    const QString room = QStringLiteral("!tray:example.org");
    const QString root = QStringLiteral("$root:example.org");

    NotificationManager manager;
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    QVariantMap p;
    p.insert(QStringLiteral("roomId"),
             MatrixClient::threadTimelineId(room, root));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$t:example.org"));
    p.insert(QStringLiteral("threadRootId"), QString{});
    manager.deliverThroughTrayForTest(p);

    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
    QVERIFY2(opened.at(0).at(0).toString() == room,
             qPrintable(QStringLiteral(
                 "a tray balloon click asked QML to open \"%1\", which is a "
                 "timeline id and not a room: the room view switches and "
                 "loads nothing")
                 .arg(opened.at(0).at(0).toString())));
    QCOMPARE(opened.at(0).at(2).toString(), root);
}

// A notification with no room (e.g. a verification request) still reaches QML
// on click: Main.qml raises the window before looking at the room id. The gate
// is the payload, which sign-out clears.
void NotificationManagerTest::aNotificationWithNoRoomStillBringsTheWindowForward()
{
    NotificationManager manager;
    QSignalSpy opened(&manager, &NotificationManager::openRequested);

    // Shaped like showGeneric()'s payload, with an empty roomId present.
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QString{});
    p.insert(QStringLiteral("eventId"), QString{});
    p.insert(QStringLiteral("threadRootId"), QString{});
    manager.deliverThroughTrayForTest(p);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");

    QCOMPARE(opened.count(), 1);
    QVERIFY2(opened.at(0).at(0).toString().isEmpty(),
             "a roomless notification invented a room to open");

    // An empty payload (what sign-out leaves) raises the window and routes
    // nowhere.
    QSignalSpy afterClear(&manager, &NotificationManager::openRequested);
    manager.clearPending();
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(afterClear.count(), 1);
    QVERIFY(afterClear.first().at(0).toString().isEmpty());
    QVERIFY(afterClear.first().at(1).toString().isEmpty());
}

// Every slot these cases invoke by name still exists: invokeMethod's return is
// ignored at the call sites, so a rename would turn their assertions vacuous.
void NotificationManagerTest::everySlotTheseCasesDriveByNameStillExists()
{
    NotificationManager manager;
    const QMetaObject *mo = manager.metaObject();
    const QList<QByteArray> names{
        QByteArrayLiteral("onActionInvoked"),
        QByteArrayLiteral("onNotificationClosed"),
        QByteArrayLiteral("onNotificationReplied"),
        QByteArrayLiteral("onFallbackMessageClicked"),
        QByteArrayLiteral("onCallRingTick"),
        QByteArrayLiteral("onActivationToken"),
    };
    for (const QByteArray &name : names) {
        bool found = false;
        for (int i = 0; i < mo->methodCount() && !found; ++i)
            found = mo->method(i).name() == name;
        QVERIFY2(found,
                 qPrintable(QStringLiteral(
                                "NotificationManager has no invokable \"%1\" "
                                "any more, so every case that drives it by "
                                "name is a silent no-op that passes without "
                                "testing anything")
                                .arg(QString::fromLatin1(name))));
    }
}

void NotificationManagerTest::theSoundPlanSuppressesThePlatformUnlessItWasChosen()
{
    using M = NotificationManager;
    // Lightning's chime: ours plays, the platform's is suppressed.
    auto own = M::planSound(true, false, M::SourceLightning);
    QVERIFY(own.playOwn);
    QVERIFY(!own.platformSound);
    QVERIFY(own.suppressPlatform);
    // System default: the platform plays, nothing of ours, nothing suppressed.
    auto system = M::planSound(true, false, M::SourceSystem);
    QVERIFY(!system.playOwn);
    QVERIFY(system.platformSound);
    QVERIFY(!system.suppressPlatform);
    // No sound allowed, or one just played: silence, and the platform's own
    // default sound is still suppressed.
    for (auto source : { M::SourceLightning, M::SourceSystem }) {
        for (auto plan : { M::planSound(false, false, source),
                           M::planSound(true, true, source) }) {
            QVERIFY(!plan.playOwn);
            QVERIFY(!plan.platformSound);
            QVERIFY(plan.suppressPlatform);
        }
    }
}

void NotificationManagerTest::soundHintsAreSuppressSoundOrTheThemedNameNeverBoth()
{
    using M = NotificationManager;
    const QVariantMap own = M::soundHints(M::planSound(true, false, M::SourceLightning));
    QCOMPARE(own.value(QStringLiteral("suppress-sound")).toBool(), true);
    QVERIFY(!own.contains(QStringLiteral("sound-name")));
    const QVariantMap system = M::soundHints(M::planSound(true, false, M::SourceSystem));
    QVERIFY(!system.contains(QStringLiteral("suppress-sound")));
    QCOMPARE(system.value(QStringLiteral("sound-name")).toString(),
             QStringLiteral("message-new-instant"));
    const QVariantMap quiet = M::soundHints(M::planSound(false, false, M::SourceSystem));
    QCOMPARE(quiet.value(QStringLiteral("suppress-sound")).toBool(), true);
    QVERIFY(!quiet.contains(QStringLiteral("sound-name")));
}

void NotificationManagerTest::aDeliveredNotificationPlaysLightningsChimeOnce()
{
    NotificationManager manager;
    QList<bool> played;
    manager.setOwnSoundPlayer([&played](bool mention) {
        played.append(mention);
        return true;
    });
    NotificationManager::Context context = baseContext();
    context.soundMode = NotificationManager::SoundAll;
    // A burst: three messages inside the coalescing window sound once.
    manager.processEvent(incomingText(), context);
    manager.processEvent(incomingText(QStringLiteral("two")), context);
    manager.processEvent(incomingText(QStringLiteral("three")), context);
    QCOMPARE(played.size(), 1);
    QCOMPARE(played.first(), false);
}

void NotificationManagerTest::theChimeFollowsThePolicyThatDecidesTheNotification()
{
    NotificationManager manager;
    int played = 0;
    manager.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    NotificationManager::Context context = baseContext();
    context.soundMode = NotificationManager::SoundAll;

    // Muted room, the room on screen, an ignored sender and a sound mode of
    // Off all deliver nothing, or nothing audible.
    NotificationManager::Context muted = context;
    muted.roomMode = NotificationManager::Muted;
    manager.processEvent(incomingText(), muted);
    NotificationManager::Context visible = context;
    visible.roomVisibleAtLatest = true;
    manager.processEvent(incomingText(), visible);
    NotificationManager::Context ignored = context;
    ignored.senderIsIgnored = true;
    manager.processEvent(incomingText(), ignored);
    NotificationManager::Context off = context;
    off.soundMode = NotificationManager::SoundOff;
    manager.processEvent(incomingText(), off);
    NotificationManager::Context disabled = context;
    disabled.notificationsEnabled = false;
    manager.processEvent(incomingText(), disabled);
    QCOMPARE(played, 0);

    // The same event under a policy that allows it does sound.
    manager.processEvent(incomingText(), context);
    QCOMPARE(played, 1);
}

void NotificationManagerTest::aMentionPlaysTheMentionChime()
{
    NotificationManager manager;
    QList<bool> played;
    manager.setOwnSoundPlayer([&played](bool mention) {
        played.append(mention);
        return true;
    });
    NotificationManager::Context context = baseContext();
    context.soundMode = NotificationManager::SoundAll;
    TimelineEvent mention = incomingText();
    mention.mentionsMe = true;
    manager.processEvent(mention, context);
    QCOMPARE(played, QList<bool>{ true });
}

void NotificationManagerTest::theSystemDefaultSourcePlaysNoChimeOfOurs()
{
    NotificationManager manager;
    // A delivery that can make the desktop's sound (the daemon, a Windows
    // balloon); see aPathThatCannotSoundPlaysOursForSystemDefault for the
    // others.
    manager.setTrayCarriesSoundForTest(true);
    int played = 0;
    manager.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    manager.setSoundSource(NotificationManager::SourceSystem);
    NotificationManager::Context context = baseContext();
    context.soundMode = NotificationManager::SoundAll;
    manager.processEvent(incomingText(), context);
    QCOMPARE(played, 0);
    // And back: the setting is live (a fresh manager, since the first
    // delivery opened the burst window).
    NotificationManager other;
    other.setTrayCarriesSoundForTest(true);
    other.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    other.setSoundSource(NotificationManager::SourceSystem);
    other.setSoundSource(NotificationManager::SourceLightning);
    other.processEvent(incomingText(QStringLiteral("again")), context);
    QCOMPARE(played, 1);
}

// Windows, measured 2026-10-07 on the pipeline-289 package: every tray balloon
// ALSO played Windows' "Notify System Generic" — on top of Lightning's chime,
// for a plain message that should have been silent, and with notification
// sound Off — because the balloon was told nothing about the sound plan and
// Qt's never sets NIIF_NOSOUND. The balloon now carries the plan's
// platformSound, and only System default may set it. Driven through the
// no-bus branch (DBUS_SESSION_BUS_ADDRESS is dead for this suite), which is
// the same deliverThroughTray() the no-QtDBus build uses.
void NotificationManagerTest::
    aTrayBalloonAllowsThePlatformSoundOnlyWhenSystemDefaultWasChosen()
{
    struct Row {
        const char *name;
        NotificationManager::SoundSource source;
        NotificationManager::SoundMode mode;
        bool mention;
        bool expectOwnChime;
        bool expectPlatformSound;
    };
    using M = NotificationManager;
    const Row rows[] = {
        // The measured FAIL: our mention chime AND Windows' sound.
        { "lightning, mention", M::SourceLightning, M::SoundMentionsAndDirect,
          true, true, false },
        // A plain message under the default mode makes no sound at all.
        { "lightning, plain message", M::SourceLightning,
          M::SoundMentionsAndDirect, false, false, false },
        // Sound Off silences everything, whatever the style.
        { "lightning, sound off", M::SourceLightning, M::SoundOff, true,
          false, false },
        { "system default, sound off", M::SourceSystem, M::SoundOff, true,
          false, false },
        // The one case the platform plays: the user chose its sound.
        { "system default, mention", M::SourceSystem, M::SoundAll, true,
          false, true },
    };
    for (const Row &row : rows) {
        NotificationManager manager;
        // Windows' balloon, which can carry the platform's sound.
        manager.setTrayCarriesSoundForTest(true);
        int chimes = 0;
        manager.setOwnSoundPlayer([&chimes](bool) { ++chimes; return true; });
        manager.setSoundSource(row.source);
        NotificationManager::Context context = baseContext();
        context.soundMode = row.mode;
        TimelineEvent event = incomingText();
        event.mentionsMe = row.mention;
        manager.processEvent(event, context);
        QVERIFY2(manager.trayAttemptsForTest() == 1,
                 qPrintable(QStringLiteral("%1: the notification did not reach "
                                           "the tray balloon at all")
                                .arg(QLatin1String(row.name))));
        QVERIFY2(chimes == (row.expectOwnChime ? 1 : 0),
                 qPrintable(QStringLiteral("%1: own chime played %2 times")
                                .arg(QLatin1String(row.name))
                                .arg(chimes)));
        QVERIFY2(manager.lastTrayPlatformSoundForTest()
                     == row.expectPlatformSound,
                 qPrintable(QStringLiteral(
                                "%1: the balloon %2 the platform's own "
                                "notification sound")
                                .arg(QLatin1String(row.name),
                                     row.expectPlatformSound
                                         ? QStringLiteral("forbade")
                                         : QStringLiteral("allowed"))));
    }

    // A burst coalesces into one alert for the platform's sound too: the
    // second balloon inside the window is silent.
    NotificationManager burst;
    burst.setTrayCarriesSoundForTest(true);
    burst.setSoundSource(M::SourceSystem);
    NotificationManager::Context context = baseContext();
    context.soundMode = M::SoundAll;
    burst.processEvent(incomingText(), context);
    QVERIFY(burst.lastTrayPlatformSoundForTest());
    burst.processEvent(incomingText(QStringLiteral("two")), context);
    QCOMPARE(burst.trayAttemptsForTest(), 2);
    QVERIFY2(!burst.lastTrayPlatformSoundForTest(),
             "a coalesced balloon still allowed the platform's sound");
}

// The call balloon follows the caller's "the desktop rings": AppController
// passes it true only when Lightning's own ringer cannot play. With our ringer
// running the balloon must not add Windows' notification sound to the ring.
void NotificationManagerTest::aCallBalloonIsSilentWhileLightningRings()
{
    NotificationManager ours;
    ours.showIncomingCall(QStringLiteral("!ring:example.org"),
                          QStringLiteral("call-1"),
                          QStringLiteral("Incoming call"),
                          QStringLiteral("body"),
                          /*sound=*/false, /*ringSeconds=*/60);
    QCOMPARE(ours.callTrayAttemptsForTest(), 1);
    QCOMPARE(ours.trayAttemptsForTest(), 1);
    QVERIFY2(!ours.lastTrayPlatformSoundForTest(),
             "the call balloon allowed Windows' sound while Lightning rings");
    ours.stopIncomingCall(QStringLiteral("call-1"));

    NotificationManager desktop;
    desktop.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-2"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"),
                             /*sound=*/true, /*ringSeconds=*/60);
    QCOMPARE(desktop.trayAttemptsForTest(), 1);
    QVERIFY(desktop.lastTrayPlatformSoundForTest());
    desktop.stopIncomingCall(QStringLiteral("call-2"));
}

// ── 2026-10-15 notification sweep (P6) ─────────────────────────────────────
// Each case names the finding it pins, and fails on the code before it.

// Findings 20/118: after a sign-out or switch the card's payload was simply
// gone, so a reply typed into it vanished and the app's "that notification
// was for a different account" refusal could never run.
void NotificationManagerTest::anActionOnACardFromBeforeSignOutReachesTheRefusal()
{
    const QString room = QStringLiteral("!room:example.org");
    const QString ann = QStringLiteral("@ann:example.org");
    NotificationManager manager;
    manager.setAccountUserId(ann);
    manager.recordPayloadForTest(
        11, messagePayload(room, QStringLiteral("$a:x"), QString(), ann));
    manager.recordPayloadForTest(
        12, messagePayload(room, QStringLiteral("$b:x"), QString(), ann));
    manager.clearPending();
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    manager.setAccountUserId(QStringLiteral("@bea:example.org"));

    QSignalSpy replies(&manager, &NotificationManager::replyRequested);
    QSignalSpy reads(&manager, &NotificationManager::markReadRequested);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationReplied",
                                      Q_ARG(quint32, 11u),
                                      Q_ARG(QString, QStringLiteral("on my way"))));
    QVERIFY2(replies.count() == 1,
             "a reply typed into the previous account's card vanished "
             "without the different-account refusal");
    // The account it was raised for, and no room: the app refuses on the
    // account before it ever looks for one.
    QCOMPARE(replies.first().at(0).toString(), ann);
    QVERIFY(replies.first().at(1).toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 12u),
                                      Q_ARG(QString, QStringLiteral("mark-read"))));
    QCOMPARE(reads.count(), 1);
    QCOMPARE(reads.first().at(0).toString(), ann);

    // The same account signed back in: no room left to route to, and not a
    // different account either, so this class says nothing was sent.
    manager.setAccountUserId(ann);
    manager.recordPayloadForTest(
        13, messagePayload(room, QStringLiteral("$c:x"), QString(), ann));
    manager.clearPending();
    const int notices = manager.genericNoticeCountForTest();
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationReplied",
                                      Q_ARG(quint32, 13u),
                                      Q_ARG(QString, QStringLiteral("hello"))));
    QCOMPARE(replies.count(), 1);
    QCOMPARE(manager.genericNoticeCountForTest(), notices + 1);
}

// The daemon's signals can reach every client on the bus, so an id this
// session never raised may be another application's card: left alone.
void NotificationManagerTest::anActionOnACardThisSessionNeverRaisedIsLeftAlone()
{
    NotificationManager manager;
    QSignalSpy replies(&manager, &NotificationManager::replyRequested);
    QSignalSpy reads(&manager, &NotificationManager::markReadRequested);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationReplied",
                                      Q_ARG(quint32, 99u),
                                      Q_ARG(QString, QStringLiteral("hi"))));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 99u),
                                      Q_ARG(QString, QStringLiteral("mark-read"))));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 99u),
                                      Q_ARG(QString, QStringLiteral("default"))));
    QCOMPARE(replies.count(), 0);
    QCOMPARE(reads.count(), 0);
    QCOMPARE(opened.count(), 0);
    QCOMPARE(manager.genericNoticeCountForTest(), 0);
}

// Findings 71/128: matrix-sdk with threading support never counts a thread
// reply in the room's unread fields, so the walk read "not unread" as read
// and withdrew a thread card about 5 s after it appeared.
void NotificationManagerTest::theReadWalkSparesThreadRepliesAndNotices()
{
    const QString room = QStringLiteral("!room:example.org");
    const QString root = QStringLiteral("$root:example.org");
    NotificationManager manager;
    manager.recordPayloadForTest(
        1, messagePayload(room, QStringLiteral("$reply:x"), root));
    manager.recordPayloadForTest(
        2, messagePayload(room, QStringLiteral("$plain:x")));
    // Nothing awaited: the walk's "read" is believed at once.
    manager.observeRoomUnread(room, false);
    QVERIFY2(manager.pendingPayloadCountForTest() == 1,
             "the room list's read state withdrew a thread reply's card");
    // Reading the thread withdraws it; so does reading the room explicitly.
    manager.closeThreadNotifications(room, QStringLiteral("$other:x"));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    manager.closeThreadNotifications(room, root);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    manager.recordPayloadForTest(
        3, messagePayload(room, QStringLiteral("$reply2:x"), root));
    manager.closeRoomNotifications(room);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

// Finding 47: m.mentions.room is the sender's claim. With the rules known
// only their highlight (which checks the @room power level and the user's
// own Off) makes a mention, its chime and, before, critical urgency.
void NotificationManagerTest::anUncheckedRoomMentionIsNotAMention()
{
    auto context = baseContext();
    context.soundMode = NotificationManager::SoundMentionsAndDirect;
    TimelineEvent event = withVerdict(incomingText(),
                                      TimelineEvent::PushVerdict::Notify);
    event.mentionsRoom = true;
    const auto decision = NotificationManager::decide(event, context);
    QVERIFY(decision.notify);
    QVERIFY2(!decision.mention, "m.mentions.room overrode the rules' verdict");
    QVERIFY2(!decision.playSound,
             "an @room from a member without the power to use it chimed");

    // And the card it raises plays the ordinary chime, not the mention one.
    NotificationManager manager;
    QList<bool> played;
    manager.setOwnSoundPlayer([&played](bool mention) {
        played.append(mention);
        return true;
    });
    context.soundMode = NotificationManager::SoundAll;
    manager.processEvent(event, context);
    QCOMPARE(played, QList<bool>{ false });
}

// Findings 22/149: a mention was sent critical, which GNOME and KDE let
// through Do Not Disturb and never expire. The composition is pure now.
void NotificationManagerTest::everyMessageCardIsNormalUrgency()
{
    using M = NotificationManager;
    const M::CardShape full = M::messageCardShape(true, true);
    QCOMPARE(full.actions,
             (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                           QStringLiteral("mark-read"),
                           QObject::tr("Mark as read"),
                           QStringLiteral("inline-reply"),
                           QObject::tr("Reply") }));
    QVERIFY(full.inlineReply);
    const M::CardShape noReply = M::messageCardShape(true, false);
    QCOMPARE(noReply.actions,
             (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                           QStringLiteral("mark-read"),
                           QObject::tr("Mark as read") }));
    QVERIFY(!noReply.inlineReply);
    // A notice has no event to read or reply to, whatever the daemon offers.
    const M::CardShape notice = M::messageCardShape(false, true);
    QCOMPARE(notice.actions,
             (QStringList{ QStringLiteral("default"), QObject::tr("Open") }));
    QVERIFY(!notice.inlineReply);
    int shapes = 0;
    for (bool actionable : { true, false }) {
        for (bool reply : { true, false }) {
            QCOMPARE(int(M::messageCardShape(actionable, reply).urgency), 1);
            ++shapes;
        }
    }
    QCOMPARE(shapes, 4);
}

// Findings 23/104: Lightning's chime (the default source) played straight
// through the desktop's Do Not Disturb.
void NotificationManagerTest::theDesktopsDoNotDisturbSilencesTheChime()
{
    NotificationManager manager;
    int played = 0;
    manager.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    auto context = baseContext();
    context.soundMode = NotificationManager::SoundAll;
    manager.setDesktopInhibitedForTest(true);
    QVERIFY(manager.desktopInhibited());
    manager.processEvent(incomingText(), context);
    QCOMPARE(manager.trayAttemptsForTest(), 1); // the card itself still goes
    QVERIFY2(played == 0, "the chime played under Do Not Disturb");
    QVERIFY(!manager.lastTrayPlatformSoundForTest());
    manager.setDesktopInhibitedForTest(false);
    TimelineEvent next = incomingText(QStringLiteral("later"));
    next.roomId = QStringLiteral("!later:example.org");
    manager.processEvent(next, context);
    QCOMPARE(played, 1);
}

// Finding 106: a chime that could not play (sounds not loaded, a wedged
// sound server) left every notification silent while the desktop's own
// sound stayed suppressed.
void NotificationManagerTest::aChimeThatCannotPlayFallsBackToTheDesktopsSound()
{
    auto context = baseContext();
    context.soundMode = NotificationManager::SoundAll;

    NotificationManager manager;
    manager.setTrayCarriesSoundForTest(true);
    manager.setSoundAllowedCheck([] { return true; });
    int asked = 0;
    manager.setOwnSoundPlayer([&asked](bool) { ++asked; return false; });
    manager.processEvent(incomingText(), context);
    QCOMPARE(asked, 1); // asked first
    QVERIFY2(manager.lastTrayPlatformSoundForTest(),
             "the chime failed and the desktop's sound stayed suppressed");

    // Without the call policy installed the player's refusal may BE the
    // policy (in a call): no fallback then.
    NotificationManager bare;
    bare.setTrayCarriesSoundForTest(true);
    bare.setOwnSoundPlayer([](bool) { return false; });
    bare.processEvent(incomingText(), context);
    QVERIFY(!bare.lastTrayPlatformSoundForTest());
}

// Finding 102: "System default" ignored the call state, so the desktop's
// sound played into a call (and into a share capturing the output mix).
void NotificationManagerTest::theCallPolicyHoldsForTheDesktopsSoundToo()
{
    auto context = baseContext();
    context.soundMode = NotificationManager::SoundAll;
    NotificationManager manager;
    manager.setTrayCarriesSoundForTest(true);
    manager.setSoundSource(NotificationManager::SourceSystem);
    bool callQuiet = true;
    manager.setSoundAllowedCheck([&callQuiet] { return !callQuiet; });
    int played = 0;
    manager.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    manager.processEvent(incomingText(), context);
    QVERIFY2(!manager.lastTrayPlatformSoundForTest(),
             "the desktop's sound played during a call");
    QCOMPARE(played, 0);
    callQuiet = false;
    TimelineEvent next = incomingText(QStringLiteral("after"));
    next.roomId = QStringLiteral("!after:example.org");
    manager.processEvent(next, context);
    QVERIFY(manager.lastTrayPlatformSoundForTest());
}

// Finding 111: a mention within 1.5 s of an ordinary chime was silent.
void NotificationManagerTest::aMentionBreaksThroughAnOrdinaryBurst()
{
    NotificationManager manager;
    QList<bool> played;
    manager.setOwnSoundPlayer([&played](bool mention) {
        played.append(mention);
        return true;
    });
    auto context = baseContext();
    context.soundMode = NotificationManager::SoundAll;
    manager.processEvent(incomingText(), context);
    TimelineEvent mention = incomingText(QStringLiteral("@alice look"));
    mention.roomId = QStringLiteral("!other:example.org");
    mention.mentionsMe = true;
    manager.processEvent(mention, context);
    QCOMPARE(played, (QList<bool>{ false, true }));
    // A second mention inside the window is one alert with the first.
    TimelineEvent again = mention;
    again.roomId = QStringLiteral("!third:example.org");
    manager.processEvent(again, context);
    QCOMPARE(played.size(), 2);
}

// Findings 48/105: "Loud" and "Notify" on the rules page (the sound tweak)
// changed nothing about Lightning's own sound.
void NotificationManagerTest::theRulesSoundTweakDecidesTheDefaultSoundMode()
{
    using Verdict = TimelineEvent::PushVerdict;
    auto context = baseContext();
    context.soundMode = NotificationManager::SoundMentionsAndDirect;
    TimelineEvent loudKeyword = withVerdict(incomingText(), Verdict::Notify);
    loudKeyword.pushSound = true;
    QVERIFY2(NotificationManager::decide(loudKeyword, context).playSound,
             "a keyword set to Loud stayed silent");
    TimelineEvent quietName = withVerdict(incomingText(), Verdict::Notify, true);
    quietName.pushSound = false;
    QVERIFY2(!NotificationManager::decide(quietName, context).playSound,
             "the display-name rule set to Notify still chimed");
    auto dm = context;
    dm.roomIsDirect = true;
    QVERIFY(!NotificationManager::decide(
                 withVerdict(incomingText(), Verdict::Notify), dm).playSound);
    // This device's Off and All still override.
    auto all = context;
    all.soundMode = NotificationManager::SoundAll;
    QVERIFY(NotificationManager::decide(quietName, all).playSound);
    auto off = context;
    off.soundMode = NotificationManager::SoundOff;
    QVERIFY(!NotificationManager::decide(loudKeyword, off).playSound);
    // Without the rules the old heuristic stands: mentions and DMs.
    QVERIFY(NotificationManager::decide(incomingText(), dm).playSound);
}

// Findings 39/107: "System default" where the delivery cannot make the
// desktop's sound (a macOS banner, a Linux tray balloon) was silence.
void NotificationManagerTest::aPathThatCannotSoundPlaysOursForSystemDefault()
{
    NotificationManager manager;
    manager.setTrayCarriesSoundForTest(false);
    manager.setSoundSource(NotificationManager::SourceSystem);
    int played = 0;
    manager.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    auto context = baseContext();
    context.soundMode = NotificationManager::SoundAll;
    manager.processEvent(incomingText(), context);
    QVERIFY2(played == 1,
             "System default on a path without the desktop's sound was silent");
    QVERIFY(!manager.lastTrayPlatformSoundForTest());
}

// Finding 108: showGeneric hard-coded silence, so an invitation (often a new
// DM) never made a sound.
void NotificationManagerTest::aGenericNoticeCanSound()
{
    NotificationManager manager;
    int played = 0;
    manager.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"),
                        QStringLiteral("!invite:example.org"), QString(),
                        /*sound=*/false);
    QCOMPARE(played, 0);
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"),
                        QStringLiteral("!invite2:example.org"), QString(),
                        /*sound=*/true);
    QCOMPARE(played, 1);
}

namespace {
TimelineEvent undecryptable(const QString &eventId = QStringLiteral("$utd:x"))
{
    TimelineEvent event = incomingText(QStringLiteral("[unable to decrypt]"));
    event.eventId = eventId;
    event.isEncrypted = true;
    event.undecryptable = true;
    return event;
}

TimelineEvent decrypted(const QString &eventId = QStringLiteral("$utd:x"),
                        const QString &body = QStringLiteral("the real text"))
{
    TimelineEvent event = incomingText(body);
    event.eventId = eventId;
    event.isEncrypted = true;
    return event;
}
} // namespace

// Findings 46/124/32: a message whose key arrived a moment late notified as
// "Encrypted message" and was never looked at again.
void NotificationManagerTest::anUndecryptableMessageWaitsBrieflyForItsKey()
{
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    NotificationManager manager;
    manager.processEvent(undecryptable(), context);
    QCOMPARE(manager.heldUndecryptableCountForTest(), 1);
    QCOMPARE(manager.trayAttemptsForTest(), 0);
    manager.processDecrypted(decrypted(), context);
    QCOMPARE(manager.heldUndecryptableCountForTest(), 0);
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    QCOMPARE(manager.lastTrayBodyForTest(), QStringLiteral("the real text"));
    // Never twice: a second report, or the same event appended again.
    manager.processDecrypted(decrypted(), context);
    manager.processEvent(undecryptable(), context);
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    QCOMPARE(manager.heldUndecryptableCountForTest(), 0);

    // A key that never comes: the generic card after the bounded hold.
    NotificationManager late;
    late.setUndecryptableHoldMsForTest(30);
    late.processEvent(undecryptable(QStringLiteral("$late:x")), context);
    QCOMPARE(late.trayAttemptsForTest(), 0);
    QTRY_COMPARE(late.trayAttemptsForTest(), 1);
    QCOMPARE(late.lastTrayBodyForTest(),
             QCoreApplication::translate("Notifications", "Encrypted message"));
}

// Finding 46: the verdict on ciphertext cannot see a mention, so in a
// mentions-only room the mention never notified at all.
void NotificationManagerTest::aMentionThatArrivesUndecryptableStillNotifies()
{
    using Verdict = TimelineEvent::PushVerdict;
    auto context = baseContext();
    context.roomMode = NotificationManager::MentionsOnly;
    NotificationManager manager;
    manager.processEvent(withVerdict(undecryptable(), Verdict::Quiet), context);
    QCOMPARE(manager.heldUndecryptableCountForTest(), 0);
    QCOMPARE(manager.trayAttemptsForTest(), 0);
    TimelineEvent plain = withVerdict(decrypted(), Verdict::Notify, true);
    plain.mentionsMe = true;
    manager.processDecrypted(plain, context);
    QVERIFY2(manager.trayAttemptsForTest() == 1,
             "a mention that decrypted late never notified");
}

// A balloon cannot be replaced: once "Encrypted message" is out, the late
// plaintext raises no second balloon (never double-notify).
void NotificationManagerTest::aLateDecryptionNeverRaisesASecondBalloon()
{
    auto context = baseContext();
    NotificationManager manager;
    manager.setUndecryptableHoldMsForTest(0);
    manager.processEvent(undecryptable(), context);
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    manager.processDecrypted(decrypted(), context);
    QCOMPARE(manager.trayAttemptsForTest(), 1);
}

// A history backfill (key backup) decrypts old messages by the hundred; none
// of them is news.
void NotificationManagerTest::aDecryptionOfHistoryRaisesNothing()
{
    auto context = baseContext();
    NotificationManager manager;
    manager.processDecrypted(decrypted(QStringLiteral("$never-seen:x")),
                             context);
    TimelineEvent history = undecryptable(QStringLiteral("$old:x"));
    history.backlog = true;
    manager.processEvent(history, context);
    manager.processDecrypted(decrypted(QStringLiteral("$old:x")), context);
    QCOMPARE(manager.trayAttemptsForTest(), 0);
    QCOMPARE(manager.heldUndecryptableCountForTest(), 0);
}

// Review of P6: hydration delivers an opened room's recent history as LIVE
// appends (backlog=false), and the room's backup download decrypts it a few
// seconds later. Remembered as a live arrival, every old encrypted message on
// the first screen raised a card and a sound once the user had scrolled up,
// unfocused the window or left the room. Same for a message that arrived
// while the room was on screen at the latest: it was read.
void NotificationManagerTest::aRoomOpensHistoryNeverNotifiesWhenItDecrypts()
{
    auto later = baseContext();
    later.roomHydrating = false;
    later.roomVisibleAtLatest = false;

    {
        auto hydrating = baseContext();
        hydrating.roomHydrating = true;
        NotificationManager manager;
        QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);
        manager.processEvent(undecryptable(QStringLiteral("$hist:x")),
                             hydrating);
        QCOMPARE(manager.heldUndecryptableCountForTest(), 0);
        manager.processDecrypted(decrypted(QStringLiteral("$hist:x")), later);
        QVERIFY2(manager.trayAttemptsForTest() == 0,
                 "history the room delivered while hydrating notified when "
                 "its key arrived");
        QCOMPARE(delivered.count(), 0);
    }
    {
        auto onScreen = baseContext();
        onScreen.roomVisibleAtLatest = true;
        NotificationManager manager;
        QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);
        manager.processEvent(undecryptable(QStringLiteral("$read:x")),
                             onScreen);
        QCOMPARE(manager.heldUndecryptableCountForTest(), 0);
        manager.processDecrypted(decrypted(QStringLiteral("$read:x")), later);
        QVERIFY2(manager.trayAttemptsForTest() == 0,
                 "a message that arrived on screen notified when its key "
                 "arrived after the user left");
        QCOMPARE(delivered.count(), 0);
    }
    {
        // Control: a genuine live arrival in a room not on screen still
        // decides on its plaintext, so the gate is not simply closed.
        NotificationManager manager;
        QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);
        manager.processEvent(undecryptable(QStringLiteral("$live:x")), later);
        QCOMPARE(manager.heldUndecryptableCountForTest(), 1);
        manager.processDecrypted(decrypted(QStringLiteral("$live:x")), later);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(delivered.count(), 1);
    }
}

// Findings 50/32: a redacted message notified with an empty body, and its
// card (with the text) stayed after the message was gone.
void NotificationManagerTest::aRedactedMessageNeitherNotifiesNorStays()
{
    TimelineEvent redacted = incomingText();
    redacted.redacted = true;
    QVERIFY(!NotificationManager::decide(redacted, baseContext()).notify);

    const QString room = QStringLiteral("!room:example.org");
    NotificationManager manager;
    manager.recordPayloadForTest(5, messagePayload(room, QStringLiteral("$gone:x")));
    manager.recordPayloadForTest(6, messagePayload(room, QStringLiteral("$kept:x")));
    manager.withdrawEvent(QStringLiteral("$gone:x"));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    // One still held for its key goes too, and never surfaces later.
    manager.processEvent(undecryptable(QStringLiteral("$secret:x")),
                         baseContext());
    QCOMPARE(manager.heldUndecryptableCountForTest(), 1);
    manager.withdrawEvent(QStringLiteral("$secret:x"));
    QCOMPARE(manager.heldUndecryptableCountForTest(), 0);
    manager.processDecrypted(decrypted(QStringLiteral("$secret:x")),
                             baseContext());
    QCOMPARE(manager.trayAttemptsForTest(), 0);
}

// Finding 52: without a verdict every edit notified again ("* fixed typo")
// and every bot notice notified, against the spec's suppress rules.
void NotificationManagerTest::withoutRulesEditsAndNoticesStayQuiet()
{
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    TimelineEvent edit = incomingText(QStringLiteral("* fixed typo"));
    edit.isEdit = true;
    edit.editNewBody = QStringLiteral("fixed typo");
    QVERIFY2(!NotificationManager::decide(edit, context).notify,
             "an edit notified a second time");
    TimelineEvent notice = incomingText();
    notice.type = TimelineEvent::Notice;
    QVERIFY2(!NotificationManager::decide(notice, context).notify,
             "a bot notice notified");
    // An edit that newly mentions this user notifies, with its new text.
    TimelineEvent mentioning = edit;
    mentioning.mentionsMe = true;
    const auto decision = NotificationManager::decide(mentioning, context);
    QVERIFY(decision.notify);
    QCOMPARE(decision.body, QStringLiteral("fixed typo"));
    // One the rules notify for shows the new text too, never the fallback.
    TimelineEvent ruled =
        withVerdict(edit, TimelineEvent::PushVerdict::Notify);
    QCOMPARE(NotificationManager::decide(ruled, context).body,
             QStringLiteral("fixed typo"));
    ruled.editNewBody.clear();
    QCOMPARE(NotificationManager::decide(ruled, context).body,
             QStringLiteral("fixed typo"));
}

// Finding 153: "/me is running late" read "is running late".
void NotificationManagerTest::anEmoteNamesItsActor()
{
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    TimelineEvent emote = incomingText(QStringLiteral("is running late"));
    emote.type = TimelineEvent::Emote;
    QCOMPARE(NotificationManager::decide(emote, context).body,
             QStringLiteral("* ") + iso(QStringLiteral("Bob"))
                 + QStringLiteral(" is running late"));
}

// Finding 115: the timeline hides a spoiler behind a click; the card, the
// lock screen and the daemon's history showed it verbatim.
void NotificationManagerTest::aSpoilerStaysHidden()
{
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    TimelineEvent spoiler =
        incomingText(QStringLiteral("the ending: Snape kills Dumbledore"));
    spoiler.formattedBody = QStringLiteral(
        "the ending: <span data-mx-spoiler=\"film\">Snape <span>kills</span> "
        "<b>Dumbledore</b></span> &amp; more");
    const QString body = NotificationManager::decide(spoiler, context).body;
    QVERIFY2(!body.contains(QStringLiteral("Snape"))
                 && !body.contains(QStringLiteral("Dumbledore"))
                 && !body.contains(QStringLiteral("kills")),
             qPrintable(body));
    QVERIFY2(body.contains(QStringLiteral("(spoiler)")), qPrintable(body));
    QVERIFY2(body.startsWith(QStringLiteral("the ending:"))
                 && body.contains(QStringLiteral("& more")),
             qPrintable(body));
    // An unterminated spoiler hides the rest rather than leaking it.
    spoiler.formattedBody =
        QStringLiteral("a <span data-mx-spoiler>secret never closed");
    QVERIFY(!NotificationManager::decide(spoiler, context)
                 .body.contains(QStringLiteral("secret")));
}

// Finding 141: a display name with U+202E reversed " in <room>" around it.
void NotificationManagerTest::namesAreIsolatedInTheSentence()
{
    // U+202E RIGHT-TO-LEFT OVERRIDE, built at run time so the source carries
    // no raw bidi control.
    const QString name = QChar(0x202E) + QStringLiteral("evil");
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    TimelineEvent event = incomingText();
    event.senderDisplayName = name;
    QCOMPARE(NotificationManager::decide(event, context).title,
             iso(name) + QStringLiteral(" in ")
                 + iso(QStringLiteral("Lightning Dev")));
    context.previewMode = NotificationManager::SenderOnly;
    QCOMPARE(NotificationManager::decide(event, context).body,
             QStringLiteral("New message from ") + iso(name));
}

// Findings 24/123/25: one card per room. Without the text, its body counts
// what it stands for.
void NotificationManagerTest::aRoomsSecondMessageUpdatesItsCard()
{
    NotificationManager manager;
    const auto context = baseContext(); // SenderOnly
    manager.processEvent(incomingText(), context);
    QCOMPARE(manager.lastTrayBodyForTest(),
             QStringLiteral("New message from ") + iso(QStringLiteral("Bob")));
    TimelineEvent second = incomingText(QStringLiteral("two"));
    second.eventId = QStringLiteral("$ev2:example.org");
    manager.processEvent(second, context);
    QVERIFY2(manager.lastTrayBodyForTest().startsWith(QStringLiteral("2 ")),
             qPrintable(manager.lastTrayBodyForTest()));
    // Another room has its own card.
    TimelineEvent elsewhere = incomingText();
    elsewhere.roomId = QStringLiteral("!elsewhere:example.org");
    manager.processEvent(elsewhere, context);
    QVERIFY(manager.lastTrayBodyForTest().startsWith(
        QStringLiteral("New message from")));
    // Reading the room starts its count again.
    manager.closeRoomNotifications(QStringLiteral("!room:example.org"));
    manager.processEvent(second, context);
    QVERIFY(manager.lastTrayBodyForTest().startsWith(
        QStringLiteral("New message from")));
}

// Finding 27: inside a snap the host knows the entry as <snap>_lightning and
// cannot resolve a themed icon name looked up inside the snap.
void NotificationManagerTest::theSnapIdentityIsOneTheHostCanResolve()
{
    using M = NotificationManager;
    const M::DesktopIdentity snap = M::desktopIdentityFor(
        QString(), QStringLiteral("/snap/lightning/x1"),
        QStringLiteral("lightning"));
    QCOMPARE(snap.desktopEntry, QStringLiteral("lightning_lightning"));
    QCOMPARE(snap.appIcon,
             QStringLiteral("file:///snap/lightning/x1/meta/gui/lightning.png"));
    const M::DesktopIdentity flatpak = M::desktopIdentityFor(
        QStringLiteral("org.example.Lightning"), QString(), QString());
    QCOMPARE(flatpak.desktopEntry, QStringLiteral("org.example.Lightning"));
    QVERIFY(flatpak.appIcon.isEmpty());
    const M::DesktopIdentity plain =
        M::desktopIdentityFor(QString(), QString(), QString());
    QCOMPARE(plain.desktopEntry, QStringLiteral("lightning"));
    QVERIFY(plain.appIcon.isEmpty());
}

// Finding 21: KWin and other xdg-activation compositors refuse focus to a
// raise without the token the daemon sent before ActionInvoked.
void NotificationManagerTest::aClickHandsTheActivationTokenToTheWindow()
{
    qunsetenv("XDG_ACTIVATION_TOKEN");
    NotificationManager manager;
    manager.setAssumeWaylandForTest(true);
    // Not our card: ignored.
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActivationToken",
                                      Q_ARG(quint32, 99u),
                                      Q_ARG(QString, QStringLiteral("tok-x"))));
    manager.recordPayloadForTest(
        7, messagePayload(QStringLiteral("!room:example.org"),
                          QStringLiteral("$e:x")));
    manager.recordPayloadForTest(
        99, messagePayload(QStringLiteral("!room:example.org"),
                           QStringLiteral("$f:x")));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 99u),
                                      Q_ARG(QString, QStringLiteral("default"))));
    QVERIFY(qEnvironmentVariable("XDG_ACTIVATION_TOKEN").isEmpty());
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActivationToken",
                                      Q_ARG(quint32, 7u),
                                      Q_ARG(QString, QStringLiteral("tok-1"))));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 7u),
                                      Q_ARG(QString, QStringLiteral("default"))));
    QCOMPARE(qEnvironmentVariable("XDG_ACTIVATION_TOKEN"),
             QStringLiteral("tok-1"));
    qunsetenv("XDG_ACTIVATION_TOKEN");
}

// Findings 28/109: the call card left out suppress-sound, so a daemon with a
// default sound played it over Lightning's ringer, and after Silence.
void NotificationManagerTest::theSilentCallCardSuppressesTheDesktopsSound()
{
    const QVariantMap silent = NotificationManager::callSoundHints(false);
    QCOMPARE(silent.value(QStringLiteral("suppress-sound")).toBool(), true);
    QVERIFY(!silent.contains(QStringLiteral("sound-name")));
    const QVariantMap ringing = NotificationManager::callSoundHints(true);
    QCOMPARE(ringing.value(QStringLiteral("sound-name")).toString(),
             QStringLiteral("phone-incoming-call"));
    QVERIFY(!ringing.contains(QStringLiteral("suppress-sound")));
}

// Finding 41: the stuck-ringer re-announcement raised a second balloon for
// the same call, which no one can withdraw.
void NotificationManagerTest::theSameCallIsNeverBalloonedTwice()
{
    NotificationManager manager;
    manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), /*sound=*/false, 60);
    QCOMPARE(manager.callTrayAttemptsForTest(), 1);
    manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), /*sound=*/true, 60);
    QVERIFY2(manager.callTrayAttemptsForTest() == 1,
             "the same call was ballooned twice");
    manager.stopIncomingCall(QStringLiteral("call-1"));
}

// Finding 82: swiping the call card away left Lightning's ringer ringing.
void NotificationManagerTest::dismissingTheCallCardStopsTheRinger()
{
    NotificationManager manager;
    QSignalSpy dismissed(&manager, &NotificationManager::callCardDismissed);
    QSignalSpy declined(&manager, &NotificationManager::callDeclineRequested);
    manager.showIncomingCall(QStringLiteral("!r:x"), QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), true, 60);
    manager.setActiveCallNotificationIdForTest(42);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                                      Q_ARG(quint32, 42u),
                                      Q_ARG(quint32, 2u)));
    QCOMPARE(dismissed.count(), 1);
    QCOMPARE(dismissed.first().at(0).toString(), QStringLiteral("call-1"));
    QCOMPARE(declined.count(), 0);
    QCOMPARE(manager.activeCallNotificationIdForTest(), 0u);
    // Closed by us (reason 3) is not the user dismissing it.
    manager.setActiveCallNotificationIdForTest(43);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                                      Q_ARG(quint32, 43u),
                                      Q_ARG(quint32, 3u)));
    QCOMPARE(dismissed.count(), 1);
    manager.stopIncomingCall(QString());
}

// Finding 83: an expired call card lost its id, so it could never be
// withdrawn and its buttons did nothing.
void NotificationManagerTest::anExpiredCallCardIsKeptForWithdrawal()
{
    NotificationManager manager;
    QSignalSpy accepted(&manager, &NotificationManager::callAcceptRequested);
    manager.showIncomingCall(QStringLiteral("!r:x"), QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), true, 60,
                             /*acceptOffered=*/true, /*rtcLane=*/true);
    manager.setActiveCallNotificationIdForTest(42);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                                      Q_ARG(quint32, 42u),
                                      Q_ARG(quint32, 1u)));
    QVERIFY(!manager.callRingActiveForTest());
    QVERIFY2(manager.activeCallNotificationIdForTest() == 42u,
             "an expired call card forgot its id");
    // Its Join still answers while the call lasts.
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 42u),
                                      Q_ARG(QString, QStringLiteral("accept"))));
    QCOMPARE(accepted.count(), 1);
}

// Finding 81: a missed call in a room already read was withdrawn by the next
// room-list update, leaving no record of the call.
void NotificationManagerTest::aMissedCallSurvivesTheReadWalk()
{
    const QString room = QStringLiteral("!dm:example.org");
    NotificationManager manager;
    // showGeneric's payload: a room, no event.
    manager.recordPayloadForTest(9, messagePayload(room, QString()));
    manager.observeRoomUnread(room, false);
    QVERIFY2(manager.pendingPayloadCountForTest() == 1,
             "the read walk withdrew a missed-call notice");
    manager.closeRoomNotifications(room);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

// Finding 35: Qt reports a click on any balloon as the same signal, so the
// latest payload answered for all of them and opened the wrong room.
void NotificationManagerTest::twoRoomsOfBalloonsOpenNeitherByGuess()
{
    NotificationManager manager;
    // Where a balloon cannot be taken down (macOS); Windows: see
    // aSecondRoomWaitsForTheBalloonOnScreen.
    manager.setBalloonWithdrawableForTest(false);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!a:x"), QStringLiteral("$a:x")));
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!b:x"), QStringLiteral("$b:x")));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked"));
    QCOMPARE(opened.count(), 1);
    QVERIFY2(opened.first().at(0).toString().isEmpty(),
             "a click on one of two rooms' balloons guessed the latest room");
    // One room's balloons: that room.
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!a:x"), QStringLiteral("$a2:x")));
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!a:x"), QStringLiteral("$a3:x")));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked"));
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toString(), QStringLiteral("!a:x"));
}

// Finding 110: nothing told the app a message card went out, so a minimised
// window could not ask the taskbar for attention.
void NotificationManagerTest::aDeliveredMessageAsksForAttention()
{
    NotificationManager manager;
    QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);
    TimelineEvent mention = incomingText();
    mention.mentionsMe = true;
    manager.processEvent(mention, baseContext());
    QCOMPARE(delivered.count(), 1);
    QCOMPARE(delivered.first().at(0).toString(),
             QStringLiteral("!room:example.org"));
    QCOMPARE(delivered.first().at(1).toBool(), true);
    // A notice is not a message; a muted room delivers nothing.
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"));
    auto muted = baseContext();
    muted.roomMode = NotificationManager::Muted;
    manager.processEvent(incomingText(), muted);
    QCOMPARE(delivered.count(), 1);
}

// ── Live accounts, Phase 0 (WP0-N) ─────────────────────────────────────────
// The click-payload table, its delivery generation and the avatar waits are
// kept per account. The empty account is the foreground one and must behave
// exactly as the single table did; the others must not disturb each other.

namespace {
const QString kAnn = QStringLiteral("@ann:example.org");
const QString kBea = QStringLiteral("@bea:example.org");

// Parks every delivery waiting for its avatar: the only way to read a built
// payload without a daemon.
void parkDeliveries(NotificationManager &manager)
{
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [](const QString &) { return false; });
}

NotificationManager::Context parkedContext(const QString &account = QString())
{
    auto context = baseContext();
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");
    context.accountUserId = account;
    return context;
}

TimelineEvent textIn(const QString &room, const QString &eventId)
{
    TimelineEvent event = incomingText();
    event.roomId = room;
    event.eventId = eventId;
    return event;
}

// The group key deliverDecision() writes into a message card's payload.
QString messageGroup(const QString &room, const QString &thread = QString())
{
    const QChar separator(0x1f);
    return QStringLiteral("m") + separator + room + separator + thread;
}

QString line(const QStringList &parts)
{
    return parts.join(QLatin1Char('|'));
}

// Every routing signal, one line each, in the order they were emitted.
void recordRouting(NotificationManager &manager, QStringList &log)
{
    QObject::connect(&manager, &NotificationManager::openRequestedForAccount,
                     &manager,
                     [&log](const QString &account, const QString &room,
                            const QString &event, const QString &thread) {
                         log << line({ QStringLiteral("account"), account,
                                       room, event, thread });
                     });
    QObject::connect(&manager, &NotificationManager::openRequested, &manager,
                     [&log](const QString &room, const QString &event,
                            const QString &thread) {
                         log << line({ QStringLiteral("open"), room, event,
                                       thread });
                     });
    QObject::connect(&manager, &NotificationManager::markReadRequested,
                     &manager,
                     [&log](const QString &account, const QString &room,
                            const QString &event) {
                         log << line({ QStringLiteral("read"), account, room,
                                       event });
                     });
    QObject::connect(&manager, &NotificationManager::replyRequested, &manager,
                     [&log](const QString &account, const QString &room,
                            const QString &thread, const QString &text) {
                         log << line({ QStringLiteral("reply"), account, room,
                                       thread, text });
                     });
}

void act(NotificationManager &manager, quint32 id, const char *action)
{
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, id),
                                      Q_ARG(QString,
                                            QString::fromLatin1(action))));
}

// The sign-out script both clears are put through: 66 cards (two evicted),
// an account switch, actions before and after the clear.
QStringList runSignOutScript(bool throughClearPendingFor)
{
    QStringList log;
    NotificationManager manager;
    recordRouting(manager, log);
    manager.setAccountUserId(kAnn);
    for (quint32 id = 1; id <= 66; ++id) {
        manager.recordPayloadForTest(
            id, messagePayload(QStringLiteral("!r%1").arg(id),
                               QStringLiteral("$e%1").arg(id), QString(),
                               kAnn));
    }
    manager.setAccountUserId(kBea);
    act(manager, 1, "mark-read");   // evicted: Ann's tombstone answers
    act(manager, 66, "default");    // the newest still routes
    if (throughClearPendingFor)
        manager.clearPendingFor(QString());
    else
        manager.clearPending();
    act(manager, 65, "mark-read");  // withdrawn by the clear
    act(manager, 64, "default");    // raises the window only
    QMetaObject::invokeMethod(&manager, "onNotificationReplied",
                              Q_ARG(quint32, 63u),
                              Q_ARG(QString, QStringLiteral("hi")));
    act(manager, 2, "mark-read");   // the other evicted card
    log << QStringLiteral("pending=%1")
               .arg(manager.pendingPayloadCountForTest())
        << QStringLiteral("generation=%1")
               .arg(manager.deliveryGenerationForTest(QString()));
    return log;
}
} // namespace

// The foreground account's payload, eviction, tombstones and clear, written
// out as the single table produced them. Not compared against clearPending(),
// which is now this clear itself.
void NotificationManagerTest::anEmptyAccountKeepsTodaysPayloadsEvictionAndTombstones()
{
    // The payload: stamped with the manager's account when the context names
    // none, as it always was. Compared whole, so a key added or lost fails.
    NotificationManager manager;
    manager.setAccountUserId(kAnn);
    parkDeliveries(manager);
    const auto context = parkedContext();
    QVERIFY(context.accountUserId.isEmpty());
    manager.processEvent(incomingText(), context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QCOMPARE(manager.avatarWaitCountForTest(QString()), 1);
    const QVariantMap expected{
        { QStringLiteral("roomId"), QStringLiteral("!room:example.org") },
        { QStringLiteral("eventId"), QStringLiteral("$ev:example.org") },
        { QStringLiteral("threadRootId"), QString() },
        { QStringLiteral("accountUserId"), kAnn },
        { QStringLiteral("mention"), false },
        { QStringLiteral("group"),
          messageGroup(QStringLiteral("!room:example.org")) },
    };
    QVERIFY2(manager.avatarWaitPayloadForTest(0) == expected,
             "the foreground account's click payload is not the one the "
             "single table built");

    // A label is carried but read by nothing yet.
    auto labelled = context;
    labelled.accountLabel = QStringLiteral("Work");
    manager.processEvent(
        textIn(QStringLiteral("!other:example.org"),
               QStringLiteral("$ev2:example.org")),
        labelled);
    QCOMPARE(manager.avatarWaitCountForTest(), 2);
    QCOMPARE(manager.avatarWaitPayloadForTest(1)
                 .value(QStringLiteral("accountUserId")).toString(),
             kAnn);

    // The new fields take part in a Context's equality.
    QVERIFY(baseContext() == baseContext());
    auto other = baseContext();
    other.accountUserId = kBea;
    QVERIFY(!(other == baseContext()));
    other = baseContext();
    other.accountLabel = QStringLiteral("Work");
    QVERIFY(!(other == baseContext()));

    // The clear drops the foreground's waits and moves its generation on.
    const quint64 before = manager.deliveryGenerationForTest(QString());
    manager.clearPendingFor(QString());
    QCOMPARE(manager.avatarWaitCountForTest(), 0);
    QCOMPARE(manager.deliveryGenerationForTest(QString()), before + 1);

    // Eviction is FIFO and the evicted cards are tombstoned for the account
    // they were raised for; the clear tombstones the rest. The golden lines
    // are what the single table emitted for this script.
    const QStringList golden{
        line({ QStringLiteral("read"), kAnn, QString(), QString() }),
        line({ QStringLiteral("account"), kAnn, QStringLiteral("!r66"),
               QStringLiteral("$e66"), QString() }),
        line({ QStringLiteral("open"), QStringLiteral("!r66"),
               QStringLiteral("$e66"), QString() }),
        line({ QStringLiteral("read"), kAnn, QString(), QString() }),
        line({ QStringLiteral("account"), QString(), QString(), QString(),
               QString() }),
        line({ QStringLiteral("open"), QString(), QString(), QString() }),
        line({ QStringLiteral("reply"), kAnn, QString(), QString(),
               QStringLiteral("hi") }),
        line({ QStringLiteral("read"), kAnn, QString(), QString() }),
        QStringLiteral("pending=0"),
        QStringLiteral("generation=1"),
    };
    const QStringList actual = runSignOutScript(true);
    QVERIFY2(actual == golden,
             qPrintable(QStringLiteral("sign-out script diverged:\n%1")
                            .arg(actual.join(QLatin1Char('\n')))));

    // The third oldest card was not evicted with the first two.
    NotificationManager fifo;
    QStringList log;
    recordRouting(fifo, log);
    for (quint32 id = 1; id <= 66; ++id) {
        fifo.recordPayloadForTest(
            id, messagePayload(QStringLiteral("!r%1").arg(id),
                               QStringLiteral("$e%1").arg(id), QString(),
                               kAnn));
    }
    QCOMPARE(fifo.pendingPayloadCountForTest(), 64);
    act(fifo, 3, "mark-read");
    QCOMPARE(log, QStringList{ line({ QStringLiteral("read"), kAnn,
                                      QStringLiteral("!r3"),
                                      QStringLiteral("$e3") }) });
}

// clearPending() is kept for its callers and must stay the foreground
// account's clear, line for line.
void NotificationManagerTest::clearPendingIsExactlyTheForegroundAccountsClear()
{
    const QStringList viaClearPending = runSignOutScript(false);
    const QStringList viaClearPendingFor = runSignOutScript(true);
    QCOMPARE(viaClearPending.size(), 10);
    QCOMPARE(viaClearPending, viaClearPendingFor);

    // The process-wide state goes with the foreground's clear, as before: the
    // ring, held messages and the balloon.
    NotificationManager manager;
    manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), true, 60);
    TimelineEvent utd = incomingText();
    utd.undecryptable = true;
    manager.processEvent(utd, baseContext());
    QCOMPARE(manager.heldUndecryptableCountForTest(), 1);
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!tray:example.org"),
                       QStringLiteral("$t:example.org")));
    manager.clearPendingFor(QString());
    QVERIFY(!manager.callRingActiveForTest());
    QCOMPARE(manager.heldUndecryptableCountForTest(), 0);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
    QVERIFY(opened.first().at(0).toString().isEmpty());
}

// openRequestedForAccount precedes every openRequested, once, with the same
// normalised ids and the account the card was raised for.
void NotificationManagerTest::everyOpenNamesItsAccountFirst()
{
    const QString room = QStringLiteral("!room:example.org");
    const QString root = QStringLiteral("$root:example.org");
    QStringList log;
    NotificationManager manager;
    // The per-room click guard (no balloon withdrawal), on every host.
    manager.setBalloonWithdrawableForTest(false);
    recordRouting(manager, log);
    manager.setAccountUserId(kAnn);

    // A card in the foreground table, raised for a thread through its
    // composite timeline id: normalised in both signals.
    manager.recordPayloadForTest(
        5, messagePayload(MatrixClient::threadTimelineId(room, root),
                          QStringLiteral("$a"), QString(), kAnn));
    act(manager, 5, "default");
    // A card in another account's table is found by its id.
    manager.recordPayloadForTest(
        kBea, 6, messagePayload(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b"), QString(), kBea));
    act(manager, 6, "default");
    // A balloon's payload names its account too.
    manager.deliverThroughTrayForTest(
        kBea, messagePayload(QStringLiteral("!t:example.org"),
                             QStringLiteral("$t"), QString(), kBea));
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    // Two rooms of balloons: the window only, so no account either.
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!x:example.org"),
                       QStringLiteral("$x"), QString(), kAnn));
    manager.deliverThroughTrayForTest(
        kBea, messagePayload(QStringLiteral("!y:example.org"),
                             QStringLiteral("$y"), QString(), kBea));
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    // A withdrawn card: the window only.
    manager.recordPayloadForTest(
        7, messagePayload(room, QStringLiteral("$c"), QString(), kAnn));
    manager.clearPendingFor(QString());
    act(manager, 7, "default");

    const QStringList expected{
        line({ QStringLiteral("account"), kAnn, room, QStringLiteral("$a"),
               root }),
        line({ QStringLiteral("open"), room, QStringLiteral("$a"), root }),
        line({ QStringLiteral("account"), kBea,
               QStringLiteral("!b:example.org"), QStringLiteral("$b"),
               QString() }),
        line({ QStringLiteral("open"), QStringLiteral("!b:example.org"),
               QStringLiteral("$b"), QString() }),
        line({ QStringLiteral("account"), kBea,
               QStringLiteral("!t:example.org"), QStringLiteral("$t"),
               QString() }),
        line({ QStringLiteral("open"), QStringLiteral("!t:example.org"),
               QStringLiteral("$t"), QString() }),
        line({ QStringLiteral("account"), QString(), QString(), QString(),
               QString() }),
        line({ QStringLiteral("open"), QString(), QString(), QString() }),
        line({ QStringLiteral("account"), QString(), QString(), QString(),
               QString() }),
        line({ QStringLiteral("open"), QString(), QString(), QString() }),
    };
    QVERIFY2(log == expected,
             qPrintable(QStringLiteral("routing diverged:\n%1")
                            .arg(log.join(QLatin1Char('\n')))));
}

// Clearing one account withdraws its cards only: another account's table,
// waits and held messages survive, and the foreground's ring keeps ringing.
void NotificationManagerTest::clearingOneAccountSparesAnothersCards()
{
    const QString a = QStringLiteral("@a:example.org");
    const QString b = QStringLiteral("@b:example.org");
    QStringList log;
    NotificationManager manager;
    recordRouting(manager, log);
    manager.setAccountUserId(kAnn);
    parkDeliveries(manager);

    manager.recordPayloadForTest(
        a, 1, messagePayload(QStringLiteral("!ra"), QStringLiteral("$a1"),
                             QString(), a));
    manager.recordPayloadForTest(
        b, 2, messagePayload(QStringLiteral("!rb"), QStringLiteral("$b1"),
                             QString(), b));
    manager.recordPayloadForTest(
        b, 4, messagePayload(QStringLiteral("!rb2"), QStringLiteral("$b2"),
                             QString(), b));
    manager.recordPayloadForTest(
        3, messagePayload(QStringLiteral("!rf"), QStringLiteral("$f1"),
                          QString(), kAnn));
    manager.processEvent(textIn(QStringLiteral("!ra"), QStringLiteral("$wa")),
                         parkedContext(a));
    manager.processEvent(textIn(QStringLiteral("!rb"), QStringLiteral("$wb")),
                         parkedContext(b));
    QCOMPARE(manager.avatarWaitCountForTest(a), 1);
    QCOMPARE(manager.avatarWaitCountForTest(b), 1);
    QCOMPARE(manager.avatarWaitCountForTest(), 0);
    // A message of each held for its key.
    TimelineEvent utdA = textIn(QStringLiteral("!ra"), QStringLiteral("$ua"));
    utdA.undecryptable = true;
    TimelineEvent utdB = textIn(QStringLiteral("!rb"), QStringLiteral("$ub"));
    utdB.undecryptable = true;
    auto heldA = baseContext();
    heldA.accountUserId = a;
    auto heldB = baseContext();
    heldB.accountUserId = b;
    manager.processEvent(utdA, heldA);
    manager.processEvent(utdB, heldB);
    QCOMPARE(manager.heldUndecryptableCountForTest(), 2);
    manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), true, 60);
    QVERIFY(manager.callRingActiveForTest());
    const quint64 generationA = manager.deliveryGenerationForTest(a);
    const quint64 generationB = manager.deliveryGenerationForTest(b);
    const quint64 generationF = manager.deliveryGenerationForTest(QString());

    manager.clearPendingFor(a);

    QCOMPARE(manager.pendingPayloadCountForTest(a), 0);
    QCOMPARE(manager.pendingPayloadCountForTest(b), 2);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    QCOMPARE(manager.avatarWaitCountForTest(a), 0);
    QCOMPARE(manager.avatarWaitCountForTest(b), 1);
    QCOMPARE(manager.heldUndecryptableCountForTest(), 1);
    QCOMPARE(manager.deliveryGenerationForTest(a), generationA + 1);
    QCOMPARE(manager.deliveryGenerationForTest(b), generationB);
    QCOMPARE(manager.deliveryGenerationForTest(QString()), generationF);
    QVERIFY2(manager.callRingActiveForTest(),
             "clearing another account stopped the foreground's call ring");
    QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-1"));

    // B's card still routes to its room; A's raises the window only.
    act(manager, 2, "default");
    act(manager, 1, "default");
    QCOMPARE(log, (QStringList{
        line({ QStringLiteral("account"), b, QStringLiteral("!rb"),
               QStringLiteral("$b1"), QString() }),
        line({ QStringLiteral("open"), QStringLiteral("!rb"),
               QStringLiteral("$b1"), QString() }),
        line({ QStringLiteral("account"), QString(), QString(), QString(),
               QString() }),
        line({ QStringLiteral("open"), QString(), QString(), QString() }),
    }));

    // The foreground's clear is today's: its table and the process-wide
    // state, and not another account's table.
    manager.clearPendingFor(QString());
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    QCOMPARE(manager.pendingPayloadCountForTest(b), 1);
    QCOMPARE(manager.avatarWaitCountForTest(b), 1);
    QVERIFY(!manager.callRingActiveForTest());
}

// The 64-card cap is per account: one account's 65th card evicts that
// account's oldest, never another's; the same for parked avatar waits.
void NotificationManagerTest::aSixtyFifthCardEvictsOnlyThatAccountsOldest()
{
    const QString a = QStringLiteral("@a:example.org");
    const QString b = QStringLiteral("@b:example.org");
    QStringList log;
    NotificationManager manager;
    recordRouting(manager, log);
    manager.setAccountUserId(kAnn);
    for (quint32 id = 101; id <= 110; ++id) {
        manager.recordPayloadForTest(
            b, id, messagePayload(QStringLiteral("!b%1").arg(id),
                                  QStringLiteral("$b%1").arg(id), QString(),
                                  b));
    }
    manager.recordPayloadForTest(
        200, messagePayload(QStringLiteral("!f"), QStringLiteral("$f"),
                            QString(), kAnn));
    for (quint32 id = 1; id <= 64; ++id) {
        manager.recordPayloadForTest(
            a, id, messagePayload(QStringLiteral("!a%1").arg(id),
                                  QStringLiteral("$a%1").arg(id), QString(),
                                  a));
    }
    QCOMPARE(manager.pendingPayloadCountForTest(a), 64);
    QCOMPARE(manager.pendingPayloadCountForTest(b), 10);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    manager.recordPayloadForTest(
        a, 65, messagePayload(QStringLiteral("!a65"), QStringLiteral("$a65"),
                              QString(), a));
    QCOMPARE(manager.pendingPayloadCountForTest(a), 64);
    QCOMPARE(manager.pendingPayloadCountForTest(b), 10);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    // A's oldest went, tombstoned for A (the different-account refusal);
    // A's second, B's oldest and the foreground's card all still route.
    act(manager, 1, "mark-read");
    act(manager, 2, "mark-read");
    act(manager, 101, "mark-read");
    act(manager, 200, "mark-read");
    QCOMPARE(log, (QStringList{
        line({ QStringLiteral("read"), a, QString(), QString() }),
        line({ QStringLiteral("read"), a, QStringLiteral("!a2"),
               QStringLiteral("$a2") }),
        line({ QStringLiteral("read"), b, QStringLiteral("!b101"),
               QStringLiteral("$b101") }),
        line({ QStringLiteral("read"), kAnn, QStringLiteral("!f"),
               QStringLiteral("$f") }),
    }));

    // Avatar waits: B's one wait survives the foreground's 65th.
    NotificationManager waits;
    parkDeliveries(waits);
    waits.processEvent(textIn(QStringLiteral("!wb"), QStringLiteral("$wb")),
                       parkedContext(b));
    for (int i = 1; i <= 64; ++i) {
        waits.processEvent(textIn(QStringLiteral("!w%1").arg(i),
                                  QStringLiteral("$w%1").arg(i)),
                           parkedContext());
    }
    QCOMPARE(waits.avatarWaitCountForTest(), 64);
    QVERIFY2(waits.avatarWaitCountForTest(b) == 1,
             "the foreground's 64th wait flushed another account's");
    waits.processEvent(textIn(QStringLiteral("!w65"), QStringLiteral("$w65")),
                       parkedContext());
    QCOMPARE(waits.avatarWaitCountForTest(), 64);
    QCOMPARE(waits.avatarWaitCountForTest(b), 1);
    // The foreground's own oldest is the one that left.
    QCOMPARE(waits.avatarWaitPayloadForTest(0)
                 .value(QStringLiteral("eventId")).toString(),
             QStringLiteral("$w2"));
}

// A Notify reply is judged by its own account's generation: clearing A turns
// away A's late replies and none of B's.
void NotificationManagerTest::aClearOfOneAccountDoesNotRejectAnothersReply()
{
    const QString a = QStringLiteral("@a:example.org");
    const QString b = QStringLiteral("@b:example.org");
    QStringList log;
    NotificationManager manager;
    recordRouting(manager, log);
    manager.setAccountUserId(kAnn);
    manager.recordPayloadForTest(
        a, 1, messagePayload(QStringLiteral("!ra"), QStringLiteral("$a1"),
                             QString(), a));
    manager.recordPayloadForTest(
        b, 2, messagePayload(QStringLiteral("!rb"), QStringLiteral("$b1"),
                             QString(), b));
    // The generations the in-flight Notify calls were sent with.
    const quint64 generationA = manager.deliveryGenerationForTest(a);
    const quint64 generationB = manager.deliveryGenerationForTest(b);
    const quint64 generationF = manager.deliveryGenerationForTest(QString());

    manager.clearPendingFor(a);
    QCOMPARE(manager.deliveryGenerationForTest(a), generationA + 1);
    QCOMPARE(manager.deliveryGenerationForTest(b), generationB);
    QCOMPARE(manager.deliveryGenerationForTest(QString()), generationF);

    // B's card answered after A's clear is still B's to route.
    manager.notifyReplyForTest(
        b, generationB, 7,
        messagePayload(QStringLiteral("!rb7"), QStringLiteral("$b7"),
                       QString(), b));
    QVERIFY2(manager.pendingPayloadCountForTest(b) == 2,
             "clearing account A rejected account B's Notify reply");
    // A's card from before its clear is closed and tombstoned, not routed.
    manager.notifyReplyForTest(
        a, generationA, 8,
        messagePayload(QStringLiteral("!ra8"), QStringLiteral("$a8"),
                       QString(), a));
    QCOMPARE(manager.pendingPayloadCountForTest(a), 0);
    // The foreground's reply is judged by its own table.
    manager.notifyReplyForTest(
        QString(), generationF, 9,
        messagePayload(QStringLiteral("!rf9"), QStringLiteral("$f9"),
                       QString(), kAnn));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    // And A's next card, sent after the clear, is A's again.
    manager.notifyReplyForTest(
        a, generationA + 1, 10,
        messagePayload(QStringLiteral("!ra10"), QStringLiteral("$a10"),
                       QString(), a));
    QCOMPARE(manager.pendingPayloadCountForTest(a), 1);

    act(manager, 8, "mark-read");
    act(manager, 7, "mark-read");
    QCOMPARE(log, (QStringList{
        line({ QStringLiteral("read"), a, QString(), QString() }),
        line({ QStringLiteral("read"), b, QStringLiteral("!rb7"),
               QStringLiteral("$b7") }),
    }));
}

// The balloon (Windows, macOS) carries the account in its payload, so its
// click names it like a daemon card's does.
void NotificationManagerTest::aBalloonCarriesTheAccountItWasRaisedFor()
{
    NotificationManager manager;
    manager.setAccountUserId(kAnn);
    auto other = baseContext();
    other.accountUserId = kBea;
    manager.processEvent(incomingText(), other);
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    QCOMPARE(manager.lastTrayPayloadForTest()
                 .value(QStringLiteral("accountUserId")).toString(),
             kBea);
    // The foreground's names the manager's account, as its payload always
    // did.
    manager.processEvent(textIn(QStringLiteral("!f:example.org"),
                                QStringLiteral("$f:example.org")),
                         baseContext());
    QCOMPARE(manager.trayAttemptsForTest(), 2);
    QCOMPARE(manager.lastTrayPayloadForTest()
                 .value(QStringLiteral("accountUserId")).toString(),
             kAnn);
    // So does the call's.
    manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), false, 60);
    QCOMPARE(manager.lastCallTrayPayloadForTest()
                 .value(QStringLiteral("accountUserId")).toString(),
             kAnn);
    manager.stopIncomingCall(QStringLiteral("call-1"));

    QStringList log;
    recordRouting(manager, log);
    const QVariantMap beas = messagePayload(
        QStringLiteral("!room:example.org"), QStringLiteral("$ev:example.org"),
        QString(), kBea);
    manager.deliverThroughTrayForTest(kBea, beas);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    // Another account's clear leaves it.
    manager.deliverThroughTrayForTest(kBea, beas);
    manager.clearPendingFor(QStringLiteral("@cy:example.org"));
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    // Its own account's clear leaves only the window to raise.
    manager.deliverThroughTrayForTest(kBea, beas);
    manager.clearPendingFor(kBea);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    const QString opened = line({ QStringLiteral("account"), kBea,
                                  QStringLiteral("!room:example.org"),
                                  QStringLiteral("$ev:example.org"),
                                  QString() });
    const QString openedRoom = line({ QStringLiteral("open"),
                                      QStringLiteral("!room:example.org"),
                                      QStringLiteral("$ev:example.org"),
                                      QString() });
    QCOMPARE(log, (QStringList{
        opened, openedRoom, opened, openedRoom,
        line({ QStringLiteral("account"), QString(), QString(), QString(),
               QString() }),
        line({ QStringLiteral("open"), QString(), QString(), QString() }),
    }));
}

// ── Live Windows test 2026-10-16 (pipeline 324) ────────────────────────────

// FAIL 2c: with balloons from two rooms, a click opened Home, and a click on
// an older balloon Windows still had queued never arrived. Only one balloon is
// on screen now; the second room waits, so the click is the one shown.
void NotificationManagerTest::aSecondRoomWaitsForTheBalloonOnScreen()
{
    NotificationManager manager;
    manager.setTrayShowsForTest(true);
    manager.setBalloonWithdrawableForTest(true);
    manager.setBalloonHoldMsForTest(60000);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                QStringLiteral("$a1:example.org")),
                         baseContext());
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    QVERIFY(manager.balloonOnScreenForTest());
    manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b1:example.org")),
                         baseContext());
    QVERIFY2(manager.trayAttemptsForTest() == 1,
             "a second room raised a second balloon while the first was on "
             "screen: Windows queues it, and a click on either reaches the app "
             "as the same signal");
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 1);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
    QVERIFY2(opened.at(0).at(0).toString() == QStringLiteral("!a:example.org"),
             qPrintable(QStringLiteral("the click on room A's balloon opened "
                                       "\"%1\"")
                            .arg(opened.at(0).at(0).toString())));
    // The click freed the screen for the room that waited.
    QCOMPARE(manager.trayAttemptsForTest(), 2);
    QCOMPARE(manager.lastTrayPayloadForTest()
                 .value(QStringLiteral("roomId")).toString(),
             QStringLiteral("!b:example.org"));
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toString(), QStringLiteral("!b:example.org"));
}

// What waits behind the balloon on screen goes out when its time is up, as
// ONE balloon: the room's latest message when it is all one room's, otherwise
// a count whose click opens no room.
void NotificationManagerTest::waitingBalloonsGoOutAsOneSummary()
{
    {
        NotificationManager manager;
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(50);
        manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                    QStringLiteral("$a1:example.org")),
                             baseContext());
        manager.setBalloonHoldMsForTest(60000);
        manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                    QStringLiteral("$b1:example.org")),
                             baseContext());
        manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                    QStringLiteral("$b2:example.org")),
                             baseContext());
        manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                    QStringLiteral("$c1:example.org")),
                             baseContext());
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 2);
        QCOMPARE(manager.pendingBalloonMessagesForTest(), 3);
        QTRY_COMPARE(manager.trayAttemptsForTest(), 2);
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 0);
        QCOMPARE(manager.lastTrayTitleForTest(), QStringLiteral("Lightning"));
        QCOMPARE(manager.lastTrayBodyForTest(),
                 QStringLiteral("3 new messages in 2 rooms"));
        QSignalSpy opened(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.count(), 1);
        QVERIFY2(opened.at(0).at(0).toString().isEmpty(),
                 "a summary of two rooms opened one of them by guess");
    }
    {
        NotificationManager manager;
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(50);
        manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                    QStringLiteral("$a1:example.org")),
                             baseContext());
        manager.setBalloonHoldMsForTest(60000);
        manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                    QStringLiteral("$b1:example.org")),
                             baseContext());
        manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                    QStringLiteral("$b2:example.org")),
                             baseContext());
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 1);
        QTRY_COMPARE(manager.trayAttemptsForTest(), 2);
        // One room: its latest message, routed to it.
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$b2:example.org"));
        QSignalSpy opened(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("!b:example.org"));
    }
}

// A mention does not wait: it replaces the balloon on screen at once and
// covers what its own room had waiting; the balloon it displaced goes back
// into the summary (review L2). Queue entries go only once the mention is
// actually shown (L3).
void NotificationManagerTest::aMentionBreaksThroughTheBalloonOnScreen()
{
    NotificationManager manager;
    manager.setTrayShowsForTest(true);
    manager.setBalloonWithdrawableForTest(true);
    manager.setBalloonHoldMsForTest(60000);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                QStringLiteral("$a1:example.org")),
                         baseContext());
    manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b1:example.org")),
                         baseContext());
    manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                QStringLiteral("$c1:example.org")),
                         baseContext());
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 2);
    TimelineEvent mention = textIn(QStringLiteral("!b:example.org"),
                                   QStringLiteral("$b2:example.org"));
    mention.mentionsMe = true;
    manager.processEvent(mention, baseContext());
    QVERIFY2(manager.trayAttemptsForTest() == 2,
             "a mention waited behind the balloon on screen");
    QCOMPARE(manager.lastTrayPayloadForTest()
                 .value(QStringLiteral("eventId")).toString(),
             QStringLiteral("$b2:example.org"));
    // B's waiting message is covered by the mention; A (displaced, never
    // clicked) and C wait.
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 2);
    QCOMPARE(manager.pendingBalloonMessagesForTest(), 2);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("!b:example.org"));
    // Then A and C, as one summary.
    QCOMPARE(manager.trayAttemptsForTest(), 3);
    QVERIFY(manager.lastTrayPayloadForTest()
                .value(QStringLiteral("roomId")).toString().isEmpty());
    QCOMPARE(manager.lastTrayBodyForTest(),
             QStringLiteral("2 new messages in 2 rooms"));

    // A mention that could not be shown removes nothing from the queue.
    NotificationManager failing;
    failing.setBalloonWithdrawableForTest(true);
    failing.setBalloonHoldMsForTest(60000);
    failing.setTrayShowsForTest(true);
    failing.processEvent(textIn(QStringLiteral("!a:example.org"),
                                QStringLiteral("$a1:example.org")),
                         baseContext());
    failing.processEvent(textIn(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b1:example.org")),
                         baseContext());
    failing.setTrayShowsForTest(false); // the tray refuses the next show
    TimelineEvent lost = textIn(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b2:example.org"));
    lost.mentionsMe = true;
    failing.processEvent(lost, baseContext());
    QCOMPARE(failing.trayAttemptsForTest(), 2);
    // B's entry is kept, A is back in the queue, nothing is dropped.
    QCOMPARE(failing.pendingBalloonEntriesForTest(), 2);
}

// Where nothing can take a balloon down (macOS, a Linux tray), a click may
// come from ANY balloon still on screen or in Notification Center: every
// balloon is shown at once, a mention has nothing to break through, and a
// click with more than one room raised since the last one opens no room
// (review H1).
void NotificationManagerTest::withoutWithdrawalEveryBalloonShowsAndTwoRoomsOpenHome()
{
    NotificationManager manager;
    manager.setTrayShowsForTest(true);
    manager.setBalloonWithdrawableForTest(false);
    manager.setBalloonHoldMsForTest(60000);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                QStringLiteral("$a1:example.org")),
                         baseContext());
    manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b1:example.org")),
                         baseContext());
    QVERIFY2(manager.trayAttemptsForTest() == 2,
             "without balloon withdrawal a second room was held back");
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 0);
    QVERIFY(!manager.balloonOnScreenForTest());
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
    QVERIFY2(opened.at(0).at(0).toString().isEmpty(),
             "a click with two rooms' balloons out guessed the latest room");
    // One room since the last click: that room.
    manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                QStringLiteral("$c1:example.org")),
                         baseContext());
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toString(), QStringLiteral("!c:example.org"));
}

// Signing out (or switching account) while balloons wait: nothing waits any
// more and nothing is shown when the time would have run out. Clearing ANOTHER
// account drops its entries only; if its balloon was on screen, the rest goes
// out at once (review M2).
void NotificationManagerTest::anAccountClearEmptiesOnlyItsBalloons()
{
    {
        NotificationManager manager;
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(50);
        manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                    QStringLiteral("$a1:example.org")),
                             baseContext());
        manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                    QStringLiteral("$b1:example.org")),
                             baseContext());
        manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                    QStringLiteral("$c1:example.org")),
                             baseContext());
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 2);
        manager.clearPendingFor(QString());
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 0);
        QVERIFY(!manager.balloonOnScreenForTest());
        QTest::qWait(200);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
    }
    auto bea = baseContext();
    bea.accountUserId = kBea;
    {
        // Bea's balloon on screen, one entry each waiting.
        NotificationManager manager;
        manager.setAccountUserId(kAnn);
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(60000);
        manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                    QStringLiteral("$a1:example.org")),
                             bea);
        manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                    QStringLiteral("$b1:example.org")),
                             baseContext());
        manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                    QStringLiteral("$c1:example.org")),
                             bea);
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 2);
        manager.clearPendingFor(kBea);
        // Ann's room goes out at once, alone.
        QCOMPARE(manager.trayAttemptsForTest(), 2);
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!b:example.org"));
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 0);
        QVERIFY(manager.balloonOnScreenForTest());
    }
    {
        // Ann's balloon on screen: clearing Bea leaves it, and Ann's entry.
        NotificationManager manager;
        manager.setAccountUserId(kAnn);
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(60000);
        manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                    QStringLiteral("$a1:example.org")),
                             baseContext());
        manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                    QStringLiteral("$b1:example.org")),
                             bea);
        manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                    QStringLiteral("$c1:example.org")),
                             baseContext());
        manager.clearPendingFor(kBea);
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 1);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QSignalSpy opened(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("!a:example.org"));
    }
}

// The call's balloon is raised once per call and never again, so a mention
// waits behind it rather than taking it off the screen.
void NotificationManagerTest::aMentionWaitsBehindTheCallBalloon()
{
    NotificationManager manager;
    manager.setTrayShowsForTest(true);
    manager.setBalloonWithdrawableForTest(true);
    manager.setBalloonHoldMsForTest(60000);
    manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), false, 60);
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    TimelineEvent mention = textIn(QStringLiteral("!b:example.org"),
                                   QStringLiteral("$b1:example.org"));
    mention.mentionsMe = true;
    manager.processEvent(mention, baseContext());
    QVERIFY2(manager.trayAttemptsForTest() == 1,
             "a mention took the call's one balloon off the screen");
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 1);
    manager.stopIncomingCall(QStringLiteral("call-1"));
}

// A room read, or a card withdrawn, leaves the summary; and a balloon whose
// time is up routes nowhere (it is off the screen).
void NotificationManagerTest::aReadRoomLeavesTheBalloonSummary()
{
    NotificationManager manager;
    manager.setTrayShowsForTest(true);
    manager.setBalloonWithdrawableForTest(true);
    manager.setAwaitUnreadMsForTest(0);
    manager.setBalloonHoldMsForTest(200);
    manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                QStringLiteral("$a1:example.org")),
                         baseContext());
    manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b1:example.org")),
                         baseContext());
    manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                QStringLiteral("$c1:example.org")),
                         baseContext());
    manager.processEvent(textIn(QStringLiteral("!d:example.org"),
                                QStringLiteral("$d1:example.org")),
                         baseContext());
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 3);
    manager.observeRoomUnread(QStringLiteral("!b:example.org"), false);
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 2);
    manager.closeRoomNotifications(QStringLiteral("!c:example.org"));
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 1);
    manager.withdrawEvent(QStringLiteral("$d1:example.org"));
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 0);
    // Nothing left to show when the balloon's time is up, and it no longer
    // routes: a late click raises the window and opens no room.
    QTRY_VERIFY(!manager.balloonOnScreenForTest());
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
    QVERIFY(opened.at(0).at(0).toString().isEmpty());
}

// The hold outlasts Windows' notification duration, so a balloon is gone
// before the next one is raised (Windows otherwise queues it).
void NotificationManagerTest::theBalloonHoldFollowsTheNotificationDuration()
{
    QCOMPARE(TrayIcon::balloonHoldMsFor(5), 6000);
    QCOMPARE(TrayIcon::balloonHoldMsFor(30), 31000);
    // Unknown: Windows' default.
    QCOMPARE(TrayIcon::balloonHoldMsFor(0), 6000);
    QCOMPARE(TrayIcon::balloonHoldMsFor(-1), 6000);
    // Capped at the longest choice Windows offers.
    QCOMPARE(TrayIcon::balloonHoldMsFor(100000), 301000);
    QVERIFY(TrayIcon::balloonHoldMs() >= 2000);
}

// The title named one sender "@six:example.org in A" in one card and "six in
// B" in the next: the background-sync path fills the event's name with the
// bare user id when the member list does not know the sender, the timeline
// path leaves it empty. A user id is no name; the member name, then the
// profile name, then the localpart.
void NotificationManagerTest::theSenderIsNamedTheSameWhicheverPathDeliveredIt()
{
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    TimelineEvent fromSync = incomingText();
    fromSync.sender = QStringLiteral("@six:example.org");
    fromSync.senderDisplayName = QStringLiteral("@six:example.org");
    TimelineEvent fromTimeline = fromSync;
    fromTimeline.senderDisplayName.clear();
    const QString expected = QStringLiteral("%1 in %2")
        .arg(iso(QStringLiteral("six")), iso(QStringLiteral("Lightning Dev")));
    QCOMPARE(NotificationManager::decide(fromSync, context).title, expected);
    QCOMPARE(NotificationManager::decide(fromTimeline, context).title, expected);
    // Sender only and an emote name the sender too.
    context.previewMode = NotificationManager::SenderOnly;
    QVERIFY(!NotificationManager::decide(fromSync, context)
                 .body.contains(QStringLiteral("example.org")));

    // The room member's name, from the context when the event has none (or
    // only the id, as displayNameFor() answers for a stranger).
    context.senderMemberName = QStringLiteral("@six:example.org");
    context.senderProfileName = QStringLiteral("Six (profile)");
    QCOMPARE(NotificationManager::senderLabel(fromSync, context),
             QStringLiteral("Six (profile)"));
    context.senderMemberName = QStringLiteral("Six in this room");
    QCOMPARE(NotificationManager::senderLabel(fromSync, context),
             QStringLiteral("Six in this room"));
    QCOMPARE(NotificationManager::senderLabel(fromTimeline, context),
             QStringLiteral("Six in this room"));
    // The event's own name (the SDK's member profile) comes first.
    TimelineEvent named = fromSync;
    named.senderDisplayName = QStringLiteral("Sixth");
    QCOMPARE(NotificationManager::senderLabel(named, context),
             QStringLiteral("Sixth"));
    // Only an id with no localpart is shown whole.
    TimelineEvent odd = fromTimeline;
    odd.sender = QStringLiteral("@:example.org");
    QCOMPARE(NotificationManager::senderLabel(odd, baseContext()),
             QStringLiteral("@:example.org"));
}

// Review round 2: a break-through re-queues what the displaced balloon stood
// for, so that must follow withdrawals and account clears as the queue does,
// or a read room (or a cleared account's room) comes back in the summary.
void NotificationManagerTest::aDisplacedBalloonBringsBackNothingReadOrCleared()
{
    {
        // A on screen, A read, then a mention from B: nothing comes back.
        NotificationManager manager;
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(60000);
        manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                    QStringLiteral("$a1:example.org")),
                             baseContext());
        manager.closeRoomNotifications(QStringLiteral("!a:example.org"));
        TimelineEvent mention = textIn(QStringLiteral("!b:example.org"),
                                       QStringLiteral("$b1:example.org"));
        mention.mentionsMe = true;
        manager.processEvent(mention, baseContext());
        QCOMPARE(manager.trayAttemptsForTest(), 2);
        QVERIFY2(manager.pendingBalloonEntriesForTest() == 0,
                 "a room already read came back into the summary when a "
                 "mention displaced its balloon");
    }
    {
        // A summary across two accounts on screen; Bea is cleared; a
        // mention displaces the summary: only Ann's room waits again.
        NotificationManager manager;
        manager.setAccountUserId(kAnn);
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(50);
        manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                    QStringLiteral("$a1:example.org")),
                             baseContext());
        manager.setBalloonHoldMsForTest(60000);
        auto bea = baseContext();
        bea.accountUserId = kBea;
        manager.processEvent(textIn(QStringLiteral("!b:example.org"),
                                    QStringLiteral("$b1:example.org")),
                             baseContext());
        manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                    QStringLiteral("$c1:example.org")),
                             bea);
        QTRY_COMPARE(manager.trayAttemptsForTest(), 2); // the summary
        QCOMPARE(manager.lastTrayBodyForTest(),
                 QStringLiteral("2 new messages in 2 rooms"));
        manager.clearPendingFor(kBea);
        // The count stays (it names no room); Bea's room is gone from what
        // it stands for.
        QCOMPARE(manager.trayAttemptsForTest(), 2);
        TimelineEvent mention = textIn(QStringLiteral("!d:example.org"),
                                       QStringLiteral("$d1:example.org"));
        mention.mentionsMe = true;
        manager.processEvent(mention, baseContext());
        QCOMPARE(manager.trayAttemptsForTest(), 3);
        QVERIFY2(manager.pendingBalloonEntriesForTest() == 1,
                 "a cleared account's room came back into the summary");
        QSignalSpy opened(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("!d:example.org"));
        // What waited is Ann's B, alone.
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!b:example.org"));
    }
    {
        // The call's balloon displacing a message balloon re-queues it too.
        NotificationManager manager;
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(60000);
        manager.processEvent(textIn(QStringLiteral("!a:example.org"),
                                    QStringLiteral("$a1:example.org")),
                             baseContext());
        manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), false, 60);
        QCOMPARE(manager.trayAttemptsForTest(), 2);
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 1);
        manager.stopIncomingCall(QStringLiteral("call-1"));
    }
}

QTEST_MAIN(NotificationManagerTest)
#include "NotificationManagerTest.moc"
