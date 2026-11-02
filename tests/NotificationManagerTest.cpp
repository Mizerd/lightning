
#include "notifications/NotificationManager.h"

#include "app/TrayIcon.h"
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
}

class NotificationManagerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readingARoomWithdrawsItsGhostNotifications();
    void anActionCarriesTheAccountTheCardWasRaisedFor();
    void anInlineReplyKeepsThePayloadUntilTheTextArrives();
    void anEmptyInlineReplySendsNothing();
    void withdrawingSparesTheIncomingCallRing();
    void anExpiredNotificationStaysWithdrawableUntilTheRoomIsRead();
    void aTrayBalloonClickOpensTheRoomItWasRaisedFor();
    void aForeignNotificationActionNeverRaisesTheWindow();
    void signingOutForgetsTheTrayBalloonsClick();
    void anAggregatedEditUpdatesAWaitingCardWithoutAnotherDelivery();
    void anUnchangedSetKeepsTheGroupedCount();
    void aSetNeverOverridesAKnownQuietVerdictOrAUtdTransition();
    void aDismissedCardIsNotRecreatedByAWaitingReplacement();
    void aReplacementForASupersededCardIsDropped();
    void readingARoomDropsAPopupStillWaitingForItsAvatar();
    void aThreadReplyNotifiesForItsRoomNotItsTimelineId();
    void aTrayBalloonClickNeverRoutesToATimelineId();
    void aNotificationWithNoRoomStillBringsTheWindowForward();
    void everySlotTheseCasesDriveByNameStillExists();
    void theSoundPlanSuppressesThePlatformUnlessItWasChosen();
    void soundHintsAreSuppressSoundOrTheThemedNameNeverBoth();
    void aDeliveredNotificationPlaysLightningsChimeOnce();
    void theChimeFollowsThePolicyThatDecidesTheNotification();
    void aMentionPlaysTheMentionChime();
    void theSystemDefaultSourcePlaysNoChimeOfOurs();
    void aTrayBalloonAllowsThePlatformSoundOnlyWhenSystemDefaultWasChosen();
    void aCallBalloonIsSilentWhileLightningRings();
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
    void markAsReadNeverAsksForActivationOrOpensTheApp();
    void aClickWithoutATokenSaysSo();
    void theSilentCallCardSuppressesTheDesktopsSound();
    void theSameCallIsNeverBalloonedTwice();
    void dismissingTheCallCardStopsTheRinger();
    void anExpiredCallCardIsKeptForWithdrawal();
    void aMissedCallSurvivesTheReadWalk();
    void twoRoomsOfBalloonsOpenNeitherByGuess();
    void aDeliveredMessageAsksForAttention();
    void anEmptyAccountKeepsTodaysPayloadsEvictionAndTombstones();
    void clearPendingIsExactlyTheForegroundAccountsClear();
    void everyOpenNamesItsAccountFirst();
    void clearingOneAccountSparesAnothersCards();
    void aSixtyFifthCardEvictsOnlyThatAccountsOldest();
    void aClearOfOneAccountDoesNotRejectAnothersReply();
    void aBalloonCarriesTheAccountItWasRaisedFor();
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
    void anEditFromTheLiveSyncRefreshesAWaitingCardOnlyIfValid();
    void anEditShowsTheBalloonOnScreenAgainWithTheNewText();
    void directMessageNotifiesWithSenderOnlyDefault()
    {
        const auto decision =
            NotificationManager::decide(incomingText(), baseContext());
        QVERIFY(decision.notify);
        QCOMPARE(decision.title, iso(QStringLiteral("Lightning Dev")));
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

        context.roomIsDirect = true;
        const auto dm = NotificationManager::decide(incomingText(), context);
        QCOMPARE(dm.title, iso(QStringLiteral("Bob")));
    }

    void pollAndMultilineBodiesStayOneLine()
    {
        auto context = baseContext();
        context.previewMode = NotificationManager::SenderAndMessage;

        TimelineEvent poll = incomingText(
            QStringLiteral("Best answer?\n1. Yes\n2. No\n3. Big Money"));
        poll.type = TimelineEvent::Poll;
        poll.pollQuestion = QStringLiteral("Best answer?");
        const auto pollDecision = NotificationManager::decide(poll, context);
        QCOMPARE(pollDecision.body, QStringLiteral("Poll: Best answer?"));

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

        QVERIFY(!NotificationManager::decide(incomingText(), context).notify);

        TimelineEvent mention = incomingText(
            QStringLiteral("without your name in the body"));
        mention.mentionsMe = true;
        const auto decision = NotificationManager::decide(mention, context);
        QVERIFY(decision.notify);
        QVERIFY(decision.body.contains(QStringLiteral("mentioned")));

        TimelineEvent roomMention = incomingText();
        roomMention.mentionsRoom = true;
        QVERIFY2(!NotificationManager::decide(roomMention, context).notify,
                 "an unchecked m.mentions.room passed a mentions-only room");
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
        auto dm = baseContext();
        dm.roomIsDirect = true;
        TimelineEvent reply = incomingText();
        reply.threadRootId = QStringLiteral("$root:example.org");
        QVERIFY(!NotificationManager::decide(
                     withVerdict(reply, Verdict::Quiet), dm).notify);
    }

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
        QVERIFY(!NotificationManager::decide(
                     withVerdict(incomingText(), Verdict::Quiet), context)
                     .notify);
    }

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
        context.roomVisibleAtLatest = false;
        QVERIFY(NotificationManager::decide(incomingText(), context).notify);
    }

    void hydratingOpenRoomSuppresses()
    {
        auto context = baseContext();
        context.roomVisibleAtLatest = false;
        context.roomHydrating = true;
        QVERIFY(!NotificationManager::decide(incomingText(), context).notify);
        TimelineEvent mention = incomingText();
        mention.mentionsMe = true;
        QVERIFY(!NotificationManager::decide(mention, context).notify);
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

    void threadRepliesCarryThreadIdentity()
    {
        auto context = baseContext();
        TimelineEvent reply = incomingText(QStringLiteral("thread reply"));
        reply.threadRootId = QStringLiteral("$root:example.org");

        QVERIFY(NotificationManager::decide(reply, context).notify);

        context.roomMode = NotificationManager::Muted;
        QVERIFY(!NotificationManager::decide(reply, context).notify);

        context.roomMode = NotificationManager::MentionsOnly;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
        TimelineEvent mentioningReply = reply;
        mentioningReply.mentionsMe = true;
        QVERIFY(NotificationManager::decide(mentioningReply, context).notify);

        context.roomMode = NotificationManager::AllMessages;
        context.initialSyncComplete = false;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
        context.initialSyncComplete = true;

        context.roomVisibleAtLatest = true;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
    }

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

    void initialSyncBacklogNeverNotifies()
    {
        auto context = baseContext();
        context.initialSyncComplete = false;
        QVERIFY(!NotificationManager::decide(incomingText(), context).notify);

        TimelineEvent mention = incomingText();
        mention.mentionsMe = true;
        QVERIFY(!NotificationManager::decide(mention, context).notify);

        context.initialSyncComplete = true;
        QVERIFY(NotificationManager::decide(incomingText(), context).notify);
    }

    void aResyncedBacklogNeverNotifiesWhateverTheVerdict()
    {
        auto context = baseContext();
        TimelineEvent history = withVerdict(
            incomingText(), TimelineEvent::PushVerdict::Notify, true);
        history.mentionsMe = true;
        history.backlog = true;
        QVERIFY(!NotificationManager::decide(history, context).notify);

        history.pushVerdict = TimelineEvent::PushVerdict::Unknown;
        QVERIFY(!NotificationManager::decide(history, context).notify);
        context.localModeUnsynced = true;
        QVERIFY(!NotificationManager::decide(history, context).notify);

        history.backlog = false;
        QVERIFY(NotificationManager::decide(history, context).notify);
    }

    void aReadThatPredatesTheCardDoesNotWithdrawIt()
    {
        NotificationManager manager;
        const QString room = QStringLiteral("!room:example.org");
        manager.processEvent(incomingText(), baseContext());
        QVERIFY(manager.roomAwaitsUnreadForTest(room));
        manager.recordPayloadForTest(
            7, QVariantMap{ { QStringLiteral("roomId"), room },
                            { QStringLiteral("eventId"),
                              QStringLiteral("$ev:example.org") },
                            { QStringLiteral("threadRootId"), QString() } });

        manager.observeRoomUnread(room, false);
        QCOMPARE(manager.pendingPayloadCountForTest(), 1);
        manager.observeRoomUnread(room, true);
        QVERIFY(!manager.roomAwaitsUnreadForTest(room));
        QCOMPARE(manager.pendingPayloadCountForTest(), 1);
        manager.observeRoomUnread(room, false);
        QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    }

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

    void aReadRoomStillWithdrawsItsCard()
    {
        NotificationManager manager;
        const QString room = QStringLiteral("!elsewhere:example.org");
        const QVariantMap payload{ { QStringLiteral("roomId"), room },
                                   { QStringLiteral("eventId"),
                                     QStringLiteral("$e:example.org") },
                                   { QStringLiteral("threadRootId"),
                                     QString() } };
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

        manager.processEvent(event, baseContext());
        manager.clearPending();
        QVERIFY(!manager.roomAwaitsUnreadForTest(room));
    }

    void invitePolicySuppressesBacklog()
    {
        QVERIFY(!NotificationManager::shouldNotifyInvite(
false,false,
true));
        QVERIFY(NotificationManager::shouldNotifyInvite(true, false, true));
        QVERIFY(!NotificationManager::shouldNotifyInvite(true, true, true));
        QVERIFY(!NotificationManager::shouldNotifyInvite(true, false, false));
    }

    void soundModeGatesEligibleNotifications()
    {
        auto ctx = baseContext();

        ctx.soundMode = NotificationManager::SoundOff;
        TimelineEvent mention = incomingText();
        mention.mentionsMe = true;
        QVERIFY(!NotificationManager::decide(mention, ctx).playSound);

        ctx.soundMode = NotificationManager::SoundMentionsAndDirect;
        const auto plain = NotificationManager::decide(incomingText(), ctx);
        QVERIFY(plain.notify);
        QVERIFY(!plain.playSound);
        QVERIFY(NotificationManager::decide(mention, ctx).playSound);
        auto dm = baseContext();
        dm.soundMode = NotificationManager::SoundMentionsAndDirect;
        dm.roomIsDirect = true;
        QVERIFY(NotificationManager::decide(incomingText(), dm).playSound);

        ctx.soundMode = NotificationManager::SoundAll;
        QVERIFY(NotificationManager::decide(incomingText(), ctx).playSound);
    }

    void suppressedNotificationsNeverSound()
    {
        auto ctx = baseContext();
        ctx.soundMode = NotificationManager::SoundAll;

        auto muted = ctx;
        muted.roomMode = NotificationManager::Muted;
        QVERIFY(!NotificationManager::decide(incomingText(), muted).playSound);

        auto active = ctx;
        active.roomVisibleAtLatest = true;
        QVERIFY(!NotificationManager::decide(incomingText(), active).playSound);

        auto backlog = ctx;
        backlog.initialSyncComplete = false;
        QVERIFY(!NotificationManager::decide(incomingText(), backlog).playSound);

        auto mentionsOnly = ctx;
        mentionsOnly.roomMode = NotificationManager::MentionsOnly;
        QVERIFY(!NotificationManager::decide(incomingText(), mentionsOnly)
                     .playSound);
    }

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

        for (quint32 id = 1; id <= 66; ++id)
            manager.recordPayloadForTest(id, payload(QStringLiteral("!r%1")
                                                         .arg(id)));
        QCOMPARE(manager.pendingPayloadCountForTest(), 64);

        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 1u),
                                  Q_ARG(QString, QStringLiteral("default")));
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.takeFirst().at(0).toString().isEmpty());

        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 66u),
                                  Q_ARG(QString, QStringLiteral("default")));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("!r66"));
        QCOMPARE(manager.pendingPayloadCountForTest(), 63);

        manager.clearPending();
        QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    }

    void callRingTimerFollowsSoundAndStops()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"),
true,60);
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-1"));
        QVERIFY(manager.callRingActiveForTest());
        manager.stopIncomingCall(QStringLiteral("other-call"));
        QVERIFY(manager.callRingActiveForTest());
        manager.stopIncomingCall(QString());
        QVERIFY(!manager.callRingActiveForTest());
        QCOMPARE(manager.activeCallIdForTest(), QString());

        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-2"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"),
false,60);
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
        manager.stopIncomingCall(QStringLiteral("call-1"));
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-2"));
        QVERIFY(manager.callRingActiveForTest());
        manager.stopIncomingCall(QStringLiteral("call-2"));
    }

    void ringDeadlineRetiresTheCard()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true,
-100);
        QVERIFY(manager.callRingActiveForTest());
        QTest::qWait(5100);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.callRingActiveForTest(), 7000);
        QCOMPARE(manager.activeCallIdForTest(), QString());
    }

    void theCallCardOffersAnswerAndDecline()
    {
        QCOMPARE(NotificationManager::callActions(true,
true),
                 (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                               QStringLiteral("accept"), QObject::tr("Join"),
                               QStringLiteral("decline"),
                               QObject::tr("Decline") }));
        QCOMPARE(NotificationManager::callActions(true,false),
                 (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                               QStringLiteral("accept"),
                               QObject::tr("Answer"),
                               QStringLiteral("decline"),
                               QObject::tr("Decline") }));
        QCOMPARE(NotificationManager::callActions(false,
                                                  false),
                 (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                               QStringLiteral("decline"),
                               QObject::tr("Decline") }));
        QCOMPARE(NotificationManager::callActions(true,false,
true),
                 (QStringList{ QStringLiteral("default"), QObject::tr("Open"),
                               QStringLiteral("accept"),
                               QObject::tr("Answer"),
                               QStringLiteral("silence"),
                               QObject::tr("Silence"),
                               QStringLiteral("decline"),
                               QObject::tr("Decline") }));
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
                                 false, false,true);
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
        QCOMPARE(declined.count(), 0);
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-1"));

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

    void silenceIncomingCallQuietensTheCardAndKeepsIt()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60,
                                 true, false,true);
        QVERIFY(manager.callRingActiveForTest());
        QVERIFY(manager.callSoundActiveForTest());
        manager.silenceIncomingCall(QStringLiteral("call-0"));
        manager.silenceIncomingCall(QString());
        QVERIFY(manager.callRingActiveForTest());
        QVERIFY(manager.callSoundActiveForTest());

        manager.silenceIncomingCall(QStringLiteral("call-1"));
        QVERIFY(!manager.callRingActiveForTest());
        QVERIFY(!manager.callSoundActiveForTest());
        QVERIFY(!manager.callSilenceOfferedForTest());
        QCOMPARE(manager.activeCallIdForTest(), QStringLiteral("call-1"));
        manager.setCallAcceptOffered(QStringLiteral("call-1"), false, false);
        QVERIFY(!manager.callSoundActiveForTest());
        QVERIFY(!manager.callRingActiveForTest());
        manager.stopIncomingCall(QString());
        QVERIFY(!manager.callSilenceOfferedForTest());
    }

    void acceptActionMatchesTheDeliveredId()
    {
        NotificationManager manager;
        QSignalSpy accepted(&manager,
                            &NotificationManager::callAcceptRequested);
        manager.showIncomingCall(QStringLiteral("!r:x"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"), true, 60,
true,true);
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
        QCOMPARE(declined.count(), 0);
        manager.stopIncomingCall(QString());
    }

    void anIncomingCallIsAnnouncedWhereThereIsNoFreedesktopDaemon()
    {
        NotificationManager manager;
        QCOMPARE(manager.callTrayAttemptsForTest(), 0);
        manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("bob is calling in Room"),
true,60,
true,true);
        QVERIFY2(manager.callTrayAttemptsForTest() == 1,
                 "an incoming call raised with no reachable notification "
                 "service never reached the tray balloon: on Windows and "
                 "macOS that is the ONLY delivery there is, so the call was "
                 "not announced on the desktop at all");
        QCOMPARE(manager.lastCallTrayPayloadForTest()
                     .value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!ring:example.org"));
        QCOMPARE(manager.lastCallTrayPayloadForTest()
                     .value(QStringLiteral("eventId")).toString(),
                 QString());
        manager.stopIncomingCall(QStringLiteral("call-1"));
    }

    void theCallBalloonIsRaisedOncePerCallNotOncePerRingTick()
    {
        NotificationManager manager;
        manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                                 QStringLiteral("call-1"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"),
true,60);
        QCOMPARE(manager.callTrayAttemptsForTest(), 1);
        for (int i = 0; i < 3; ++i)
            QVERIFY(QMetaObject::invokeMethod(&manager, "onCallRingTick"));
        QCOMPARE(manager.callTrayAttemptsForTest(), 1);
        manager.stopIncomingCall(QStringLiteral("call-1"));
        manager.showIncomingCall(QStringLiteral("!other:example.org"),
                                 QStringLiteral("call-2"),
                                 QStringLiteral("Incoming call"),
                                 QStringLiteral("body"),
true,60);
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
        manager.recordPayloadForTest(100, p);
        for (quint32 id = 1; id <= 63; ++id)
            manager.recordPayloadForTest(id, p);
        manager.recordPayloadForTest(100, p);
        manager.recordPayloadForTest(200, p);
        QCOMPARE(manager.pendingPayloadCountForTest(), 64);
        QSignalSpy spy(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 100u),
                                  Q_ARG(QString, QStringLiteral("default")));
        QCOMPARE(spy.count(), 1);
    }

    void theMacMenuBarBadgeIsCutIntoTheAlphaChannel()
    {
        QPixmap base(44, 44);
        base.fill(QColor(0, 0, 0, 255));

        const QImage plain =
            TrayIcon::macTemplateBadged(base, QString{}).toImage();
        QCOMPARE(plain.pixelColor(43, 43).alpha(), 255);

        const QImage badged =
            TrayIcon::macTemplateBadged(base, QStringLiteral("3"))
                .toImage()
                .convertToFormat(QImage::Format_ARGB32);

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

        QVERIFY2(alphaAt(hub.x() + radius * 0.7, hub.y()) > 200,
                 "the badge disc is not opaque, so the menu bar would paint "
                 "nothing where the count should be");

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

        bool gap = false;
        for (qreal d = radius + 0.5; d < radius + moat && !gap; d += 0.5) {
            const qreal offset = d / std::sqrt(2.0);
            gap = alphaAt(hub.x() - offset, hub.y() - offset) < 40;
        }
        QVERIFY2(gap,
                 "the badge touches the mark it sits on, which in a "
                 "template image is one merged blob");

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

        QCOMPARE(TrayIcon::macTemplateBadged(base, QString{}).size(),
                 base.size());
    }
};

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

    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    manager.closeRoomNotifications(QStringLiteral("!nothing:x"));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    manager.closeRoomNotifications(QString{});
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
}

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

    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!room:example.org"));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$msg:example.org"));
    p.insert(QStringLiteral("threadRootId"), QString());
    p.insert(QStringLiteral("accountUserId"), QStringLiteral("@ann:example.org"));
    manager.recordPayloadForTest(11, p);

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
    QCOMPARE(spy.first().at(2).toString(), QStringLiteral("$root:example.org"));
    QCOMPARE(spy.first().at(3).toString(), QStringLiteral("on my way"));
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
                              Q_ARG(quint32, 7u), Q_ARG(quint32, 1u));
    QCOMPARE(manager.pendingPayloadCountForTest(), 3);
    QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                              Q_ARG(quint32, 8u), Q_ARG(quint32, 2u));
    QCOMPARE(manager.pendingPayloadCountForTest(), 2);
    QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                              Q_ARG(quint32, 9u), Q_ARG(quint32, 3u));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);

    manager.closeRoomNotifications(QStringLiteral("!late:x"));
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

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
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
}

void NotificationManagerTest::aForeignNotificationActionNeverRaisesTheWindow()
{
    for (const bool withdrawable : {true, false}) {
        NotificationManager manager;
        manager.setBalloonWithdrawableForTest(withdrawable);
        QSignalSpy opened(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.count(), 0);
    }
}

void NotificationManagerTest::signingOutForgetsTheTrayBalloonsClick()
{
    NotificationManager manager;
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QStringLiteral("!previous:x"));
    p.insert(QStringLiteral("eventId"), QStringLiteral("$old:example.org"));
    p.insert(QStringLiteral("threadRootId"), QString{});
    manager.deliverThroughTrayForTest(p);

    manager.clearPending();

    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QVERIFY2(opened.isEmpty(),
             "a tray balloon raised for the previous account still opened its "
             "room after clearPending(): the sign-out sweep does not forget "
             "the balloon's click payload");
}

void NotificationManagerTest::anAggregatedEditUpdatesAWaitingCardWithoutAnotherDelivery()
{
    NotificationManager manager;
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [](const QString &) { return false; });
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");
    auto event = incomingText();
    manager.processEvent(event, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);
    event.body = QStringLiteral("latest words");
    event.edited = true;
    manager.processUpdated(event, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("latest words"));
    QCOMPARE(delivered.count(), 0);
    QCOMPARE(manager.avatarWaitPayloadForTest(0).value(QStringLiteral("eventId")).toString(), event.eventId);

    auto raw = event;
    raw.isEdit = true;
    raw.editNewBody = QStringLiteral("unvalidated");
    manager.processUpdated(raw, context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("latest words"));
    event.eventId = QStringLiteral("$older:example.org");
    manager.processUpdated(event, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    event = incomingText();
    manager.processUpdated(event, context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), NotificationManager::decide(event, context).body);
    manager.closeRoomNotifications(event.roomId);
    manager.processUpdated(event, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 0);

    event.type = TimelineEvent::Notice;
    event.pushVerdict = TimelineEvent::PushVerdict::Notify;
    manager.processEvent(event, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    event.pushVerdict = TimelineEvent::PushVerdict::Unknown;
    event.body = QStringLiteral("updated notice");
    manager.processUpdated(event, context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("updated notice"));
    QVERIFY(manager.avatarWaitHasImageForTest(0));
    context.previewMode = NotificationManager::Private;
    event.body = QStringLiteral("private words");
    manager.processUpdated(event, context);
    QVERIFY(!manager.avatarWaitBodyForTest(0).contains(QStringLiteral("private words")));
    QVERIFY2(!manager.avatarWaitHasImageForTest(0),
             "a Private refresh kept the room avatar or initials");
}

void NotificationManagerTest::anUnchangedSetKeepsTheGroupedCount()
{
    NotificationManager manager;
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [](const QString &) { return false; });
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderOnly;
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");
    auto first = incomingText();
    auto second = incomingText();
    second.eventId = QStringLiteral("$second:example.org");
    manager.processEvent(first, context);
    manager.processEvent(second, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 2);
    const QString counted = manager.avatarWaitBodyForTest(1);
    QVERIFY(counted != NotificationManager::decide(second, context).body);
    manager.processUpdated(second, context);
    QCOMPARE(manager.avatarWaitBodyForTest(1), counted);
    QCOMPARE(manager.groupBodyForTest(
                 manager.firstGroupPayloadForTest().value(QStringLiteral("group")).toString()),
             counted);
    second.body = QStringLiteral("edited words");
    second.edited = true;
    manager.processUpdated(second, context);
    QCOMPARE(manager.avatarWaitBodyForTest(1),
             QCoreApplication::translate("Notifications", "%n new messages",
                                         nullptr, 2));
    context.previewMode = NotificationManager::Private;
    manager.processUpdated(second, context);
    QCOMPARE(manager.avatarWaitBodyForTest(1),
             QCoreApplication::translate("Notifications",
                                         "%n new Matrix notifications",
                                         nullptr, 2));
}

void NotificationManagerTest::aDismissedCardIsNotRecreatedByAWaitingReplacement()
{
    NotificationManager manager;
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");
    auto event = incomingText();
    manager.processEvent(event, context);
    manager.notifyReplyForTest(QString(), manager.deliveryGenerationForTest(QString()),
                               55, manager.firstGroupPayloadForTest());
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [](const QString &) { return false; });
    event.body = QStringLiteral("edited while shown");
    manager.processUpdated(event, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                                      Q_ARG(quint32, 55), Q_ARG(quint32, 2)));
    QCOMPARE(manager.avatarWaitCountForTest(), 0);
    QCOMPARE(manager.groupTableSizeForTest(), 0);
    QCOMPARE(manager.droppedReplacementsForTest(), 0);
}

void NotificationManagerTest::aReplacementForASupersededCardIsDropped()
{
    bool failed = false;
    NotificationManager manager;
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");
    auto event = incomingText();
    manager.processEvent(event, context);
    manager.notifyReplyForTest(QString(), manager.deliveryGenerationForTest(QString()),
                               56, manager.firstGroupPayloadForTest());
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [&failed](const QString &) { return failed; });
    event.body = QStringLiteral("edited");
    manager.processUpdated(event, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    auto newer = incomingText(QStringLiteral("newer"));
    newer.eventId = QStringLiteral("$newer:example.org");
    manager.processEvent(newer, context);
    failed = true;
    QTRY_COMPARE_WITH_TIMEOUT(manager.avatarWaitCountForTest(), 0, 10000);
    QCOMPARE(manager.droppedReplacementsForTest(), 1);
}

void NotificationManagerTest::readingARoomDropsAPopupStillWaitingForItsAvatar()
{
    NotificationManager manager;
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [](const QString &) { return false; });

    auto context = baseContext();
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");
    manager.processEvent(incomingText(), context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);

    TimelineEvent other = incomingText();
    other.roomId = QStringLiteral("!other:example.org");
    other.eventId = QStringLiteral("$ev2:example.org");
    manager.processEvent(other, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 2);

    manager.closeRoomNotifications(QStringLiteral("!room:example.org"));
    QVERIFY2(manager.avatarWaitCountForTest() == 1,
             "reading a room left its not-yet-shown notification queued, so it "
             "pops up after the room has already been read");

    manager.closeRoomNotifications(QStringLiteral("!other:example.org"));
    QCOMPARE(manager.avatarWaitCountForTest(), 0);
}

void NotificationManagerTest::aThreadReplyNotifiesForItsRoomNotItsTimelineId()
{
    const QString room = QStringLiteral("!room:example.org");
    const QString root = QStringLiteral("$root:example.org");
    const QString timelineId = MatrixClient::threadTimelineId(room, root);
    QVERIFY2(MatrixClient::isThreadTimelineId(timelineId),
             "the fixture did not build a composite timeline id, so this "
             "case cannot see the defect it exists for");

    NotificationManager manager;
    manager.setAvatarProvider([](const QString &, bool) { return QImage(); },
                              [](const QString &) { return false; });

    auto context = baseContext();
    context.avatarMxc = QStringLiteral("mxc://example.org/roomavatar");

    TimelineEvent reply = incomingText();
    reply.roomId = timelineId;
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

    manager.closeRoomNotifications(room);
    QVERIFY2(manager.avatarWaitCountForTest() == 0,
             "reading the room did not withdraw its thread notification, "
             "because the payload's room id is not the room's id");

    TimelineEvent rootless = incomingText(QStringLiteral("second"));
    rootless.eventId = QStringLiteral("$ev2:example.org");
    rootless.roomId = timelineId;
    rootless.threadRootId.clear();
    manager.processEvent(rootless, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QCOMPARE(manager.avatarWaitPayloadForTest(0)
                 .value(QStringLiteral("threadRootId")).toString(), root);

    manager.closeRoomNotifications(room);
    TimelineEvent plain = incomingText();
    plain.eventId = QStringLiteral("$ev3:example.org");
    manager.processEvent(plain, context);
    QCOMPARE(manager.avatarWaitPayloadForTest(0)
                 .value(QStringLiteral("roomId")).toString(), room);
    QVERIFY(manager.avatarWaitPayloadForTest(0)
                .value(QStringLiteral("threadRootId")).toString().isEmpty());
}

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

void NotificationManagerTest::aNotificationWithNoRoomStillBringsTheWindowForward()
{
    NotificationManager manager;
    QSignalSpy opened(&manager, &NotificationManager::openRequested);

    QVariantMap p;
    p.insert(QStringLiteral("roomId"), QString{});
    p.insert(QStringLiteral("eventId"), QString{});
    p.insert(QStringLiteral("threadRootId"), QString{});
    manager.deliverThroughTrayForTest(p);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");

    QCOMPARE(opened.count(), 1);
    QVERIFY2(opened.at(0).at(0).toString().isEmpty(),
             "a roomless notification invented a room to open");

    QSignalSpy afterClear(&manager, &NotificationManager::openRequested);
    manager.clearPending();
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(afterClear.count(), 0);
}

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
    auto own = M::planSound(true, false, M::SourceLightning);
    QVERIFY(own.playOwn);
    QVERIFY(!own.platformSound);
    QVERIFY(own.suppressPlatform);
    auto system = M::planSound(true, false, M::SourceSystem);
    QVERIFY(!system.playOwn);
    QVERIFY(system.platformSound);
    QVERIFY(!system.suppressPlatform);
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
    manager.setTrayCarriesSoundForTest(true);
    int played = 0;
    manager.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    manager.setSoundSource(NotificationManager::SourceSystem);
    NotificationManager::Context context = baseContext();
    context.soundMode = NotificationManager::SoundAll;
    manager.processEvent(incomingText(), context);
    QCOMPARE(played, 0);
    NotificationManager other;
    other.setTrayCarriesSoundForTest(true);
    other.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    other.setSoundSource(NotificationManager::SourceSystem);
    other.setSoundSource(NotificationManager::SourceLightning);
    other.processEvent(incomingText(QStringLiteral("again")), context);
    QCOMPARE(played, 1);
}

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
        { "lightning, mention", M::SourceLightning, M::SoundMentionsAndDirect,
          true, true, false },
        { "lightning, plain message", M::SourceLightning,
          M::SoundMentionsAndDirect, false, false, false },
        { "lightning, sound off", M::SourceLightning, M::SoundOff, true,
          false, false },
        { "system default, sound off", M::SourceSystem, M::SoundOff, true,
          false, false },
        { "system default, mention", M::SourceSystem, M::SoundAll, true,
          false, true },
    };
    for (const Row &row : rows) {
        NotificationManager manager;
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

void NotificationManagerTest::aCallBalloonIsSilentWhileLightningRings()
{
    NotificationManager ours;
    ours.showIncomingCall(QStringLiteral("!ring:example.org"),
                          QStringLiteral("call-1"),
                          QStringLiteral("Incoming call"),
                          QStringLiteral("body"),
false,60);
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
true,60);
    QCOMPARE(desktop.trayAttemptsForTest(), 1);
    QVERIFY(desktop.lastTrayPlatformSoundForTest());
    desktop.stopIncomingCall(QStringLiteral("call-2"));
}

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
    QCOMPARE(replies.first().at(0).toString(), ann);
    QVERIFY(replies.first().at(1).toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 12u),
                                      Q_ARG(QString, QStringLiteral("mark-read"))));
    QCOMPARE(reads.count(), 1);
    QCOMPARE(reads.first().at(0).toString(), ann);

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

void NotificationManagerTest::theReadWalkSparesThreadRepliesAndNotices()
{
    const QString room = QStringLiteral("!room:example.org");
    const QString root = QStringLiteral("$root:example.org");
    NotificationManager manager;
    manager.recordPayloadForTest(
        1, messagePayload(room, QStringLiteral("$reply:x"), root));
    manager.recordPayloadForTest(
        2, messagePayload(room, QStringLiteral("$plain:x")));
    manager.observeRoomUnread(room, false);
    QVERIFY2(manager.pendingPayloadCountForTest() == 1,
             "the room list's read state withdrew a thread reply's card");
    manager.closeThreadNotifications(room, QStringLiteral("$other:x"));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    manager.closeThreadNotifications(room, root);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    manager.recordPayloadForTest(
        3, messagePayload(room, QStringLiteral("$reply2:x"), root));
    manager.closeRoomNotifications(room);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

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
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    QVERIFY2(played == 0, "the chime played under Do Not Disturb");
    QVERIFY(!manager.lastTrayPlatformSoundForTest());
    manager.setDesktopInhibitedForTest(false);
    TimelineEvent next = incomingText(QStringLiteral("later"));
    next.roomId = QStringLiteral("!later:example.org");
    manager.processEvent(next, context);
    QCOMPARE(played, 1);
}

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
    QCOMPARE(asked, 1);
    QVERIFY2(manager.lastTrayPlatformSoundForTest(),
             "the chime failed and the desktop's sound stayed suppressed");

    NotificationManager bare;
    bare.setTrayCarriesSoundForTest(true);
    bare.setOwnSoundPlayer([](bool) { return false; });
    bare.processEvent(incomingText(), context);
    QVERIFY(!bare.lastTrayPlatformSoundForTest());
}

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
    TimelineEvent again = mention;
    again.roomId = QStringLiteral("!third:example.org");
    manager.processEvent(again, context);
    QCOMPARE(played.size(), 2);
}

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
    auto all = context;
    all.soundMode = NotificationManager::SoundAll;
    QVERIFY(NotificationManager::decide(quietName, all).playSound);
    auto off = context;
    off.soundMode = NotificationManager::SoundOff;
    QVERIFY(!NotificationManager::decide(loudKeyword, off).playSound);
    QVERIFY(NotificationManager::decide(incomingText(), dm).playSound);
}

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

void NotificationManagerTest::aGenericNoticeCanSound()
{
    NotificationManager manager;
    int played = 0;
    manager.setOwnSoundPlayer([&played](bool) { ++played; return true; });
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"),
                        QStringLiteral("!invite:example.org"), QString(),
false);
    QCOMPARE(played, 0);
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"),
                        QStringLiteral("!invite2:example.org"), QString(),
true);
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
}

void NotificationManagerTest::aSetNeverOverridesAKnownQuietVerdictOrAUtdTransition()
{
    using Verdict = TimelineEvent::PushVerdict;
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    NotificationManager manager;
    manager.setUndecryptableHoldMsForTest(0);
    manager.processEvent(undecryptable(), context);
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    manager.notifyReplyForTest(QString(), manager.deliveryGenerationForTest(QString()),
                               77, manager.firstGroupPayloadForTest());
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    const QString group =
        manager.firstGroupPayloadForTest().value(QStringLiteral("group")).toString();
    const QString utdBody = manager.groupBodyForTest(group);
    QCOMPARE(utdBody, QCoreApplication::translate("Notifications", "Encrypted message"));
    const TimelineEvent quiet = withVerdict(decrypted(), Verdict::Quiet);
    manager.processUpdated(quiet, context);
    QCOMPARE(manager.groupBodyForTest(group), utdBody);
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
    manager.processDecrypted(quiet, context);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    QCOMPARE(manager.groupTableSizeForTest(), 0);

    NotificationManager plain;
    auto notified = withVerdict(incomingText(), Verdict::Notify);
    plain.processEvent(notified, context);
    plain.notifyReplyForTest(QString(), plain.deliveryGenerationForTest(QString()),
                             78, plain.firstGroupPayloadForTest());
    auto unknown = incomingText(QStringLiteral("edited"));
    plain.processUpdated(unknown, context);
    QCOMPARE(plain.pendingPayloadCountForTest(), 1);
    plain.processUpdated(withVerdict(unknown, Verdict::Quiet), context);
    QCOMPARE(plain.pendingPayloadCountForTest(), 0);
}

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
    manager.processDecrypted(decrypted(), context);
    manager.processEvent(undecryptable(), context);
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    QCOMPARE(manager.heldUndecryptableCountForTest(), 0);

    NotificationManager late;
    late.setUndecryptableHoldMsForTest(30);
    late.processEvent(undecryptable(QStringLiteral("$late:x")), context);
    QCOMPARE(late.trayAttemptsForTest(), 0);
    QTRY_COMPARE(late.trayAttemptsForTest(), 1);
    QCOMPARE(late.lastTrayBodyForTest(),
             QCoreApplication::translate("Notifications", "Encrypted message"));
}

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
        NotificationManager manager;
        QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);
        manager.processEvent(undecryptable(QStringLiteral("$live:x")), later);
        QCOMPARE(manager.heldUndecryptableCountForTest(), 1);
        manager.processDecrypted(decrypted(QStringLiteral("$live:x")), later);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(delivered.count(), 1);
    }
}

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
    manager.processEvent(undecryptable(QStringLiteral("$secret:x")),
                         baseContext());
    QCOMPARE(manager.heldUndecryptableCountForTest(), 1);
    manager.withdrawEvent(QStringLiteral("$secret:x"));
    QCOMPARE(manager.heldUndecryptableCountForTest(), 0);
    manager.processDecrypted(decrypted(QStringLiteral("$secret:x")),
                             baseContext());
    QCOMPARE(manager.trayAttemptsForTest(), 0);
}

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
    TimelineEvent mentioning = edit;
    mentioning.mentionsMe = true;
    const auto decision = NotificationManager::decide(mentioning, context);
    QVERIFY(decision.notify);
    QCOMPARE(decision.body, QStringLiteral("fixed typo"));
    TimelineEvent ruled =
        withVerdict(edit, TimelineEvent::PushVerdict::Notify);
    QCOMPARE(NotificationManager::decide(ruled, context).body,
             QStringLiteral("fixed typo"));
    ruled.editNewBody.clear();
    QCOMPARE(NotificationManager::decide(ruled, context).body,
             QStringLiteral("fixed typo"));
    ruled.editNewBodyAvailable = true;
    QVERIFY(NotificationManager::decide(ruled, context).body.isEmpty());
}

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
    spoiler.formattedBody =
        QStringLiteral("a <span data-mx-spoiler>secret never closed");
    QVERIFY(!NotificationManager::decide(spoiler, context)
                 .body.contains(QStringLiteral("secret")));
}

void NotificationManagerTest::namesAreIsolatedInTheSentence()
{
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

void NotificationManagerTest::aRoomsSecondMessageUpdatesItsCard()
{
    NotificationManager manager;
    const auto context = baseContext();
    manager.processEvent(incomingText(), context);
    QCOMPARE(manager.lastTrayBodyForTest(),
             QStringLiteral("New message from ") + iso(QStringLiteral("Bob")));
    TimelineEvent second = incomingText(QStringLiteral("two"));
    second.eventId = QStringLiteral("$ev2:example.org");
    manager.processEvent(second, context);
    QVERIFY2(manager.lastTrayBodyForTest().startsWith(QStringLiteral("2 ")),
             qPrintable(manager.lastTrayBodyForTest()));
    TimelineEvent elsewhere = incomingText();
    elsewhere.roomId = QStringLiteral("!elsewhere:example.org");
    manager.processEvent(elsewhere, context);
    QVERIFY(manager.lastTrayBodyForTest().startsWith(
        QStringLiteral("New message from")));
    manager.closeRoomNotifications(QStringLiteral("!room:example.org"));
    manager.processEvent(second, context);
    QVERIFY(manager.lastTrayBodyForTest().startsWith(
        QStringLiteral("New message from")));
}

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

void NotificationManagerTest::aClickHandsTheActivationTokenToTheWindow()
{
    qunsetenv("XDG_ACTIVATION_TOKEN");
    NotificationManager manager;
    manager.setAssumeWaylandForTest(true);
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

void NotificationManagerTest::markAsReadNeverAsksForActivationOrOpensTheApp()
{
    qunsetenv("XDG_ACTIVATION_TOKEN");
    NotificationManager manager;
    manager.setAssumeWaylandForTest(true);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    manager.recordPayloadForTest(
        21, messagePayload(QStringLiteral("!room:example.org"),
                           QStringLiteral("$e:x")));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActivationToken",
                                      Q_ARG(quint32, 21u),
                                      Q_ARG(QString, QStringLiteral("tok-r"))));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 21u),
                                      Q_ARG(QString, QStringLiteral("mark-read"))));
    QCOMPARE(opened.count(), 0);
    QVERIFY(qEnvironmentVariable("XDG_ACTIVATION_TOKEN").isEmpty());
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 21u),
                                      Q_ARG(QString, QStringLiteral("default"))));
    QCOMPARE(opened.count(), 0);
    QVERIFY(qEnvironmentVariable("XDG_ACTIVATION_TOKEN").isEmpty());
    QVERIFY(!manager.lastClickHadActivationToken());
}

void NotificationManagerTest::aClickWithoutATokenSaysSo()
{
    qunsetenv("XDG_ACTIVATION_TOKEN");
    NotificationManager manager;
    manager.setAssumeWaylandForTest(true);
    manager.recordPayloadForTest(
        31, messagePayload(QStringLiteral("!room:example.org"),
                           QStringLiteral("$e:x")));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 31u),
                                      Q_ARG(QString, QStringLiteral("default"))));
    QVERIFY(!manager.lastClickHadActivationToken());
    manager.recordPayloadForTest(
        32, messagePayload(QStringLiteral("!room:example.org"),
                           QStringLiteral("$f:x")));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActivationToken",
                                      Q_ARG(quint32, 32u),
                                      Q_ARG(QString, QStringLiteral("tok-2"))));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 32u),
                                      Q_ARG(QString, QStringLiteral("default"))));
    QVERIFY(manager.lastClickHadActivationToken());
    QVERIFY(!manager.lastClickHadActivationToken());
    qunsetenv("XDG_ACTIVATION_TOKEN");
}

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

void NotificationManagerTest::theSameCallIsNeverBalloonedTwice()
{
    NotificationManager manager;
    manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"),false, 60);
    QCOMPARE(manager.callTrayAttemptsForTest(), 1);
    manager.showIncomingCall(QStringLiteral("!ring:example.org"),
                             QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"),true, 60);
    QVERIFY2(manager.callTrayAttemptsForTest() == 1,
             "the same call was ballooned twice");
    manager.stopIncomingCall(QStringLiteral("call-1"));
}

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
    manager.setActiveCallNotificationIdForTest(43);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                                      Q_ARG(quint32, 43u),
                                      Q_ARG(quint32, 3u)));
    QCOMPARE(dismissed.count(), 1);
    manager.stopIncomingCall(QString());
}

void NotificationManagerTest::anExpiredCallCardIsKeptForWithdrawal()
{
    NotificationManager manager;
    QSignalSpy accepted(&manager, &NotificationManager::callAcceptRequested);
    manager.showIncomingCall(QStringLiteral("!r:x"), QStringLiteral("call-1"),
                             QStringLiteral("Incoming call"),
                             QStringLiteral("body"), true, 60,
true,true);
    manager.setActiveCallNotificationIdForTest(42);
    QVERIFY(QMetaObject::invokeMethod(&manager, "onNotificationClosed",
                                      Q_ARG(quint32, 42u),
                                      Q_ARG(quint32, 1u)));
    QVERIFY(!manager.callRingActiveForTest());
    QVERIFY2(manager.activeCallNotificationIdForTest() == 42u,
             "an expired call card forgot its id");
    QVERIFY(QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                      Q_ARG(quint32, 42u),
                                      Q_ARG(QString, QStringLiteral("accept"))));
    QCOMPARE(accepted.count(), 1);
}

void NotificationManagerTest::aMissedCallSurvivesTheReadWalk()
{
    const QString room = QStringLiteral("!dm:example.org");
    NotificationManager manager;
    manager.recordPayloadForTest(9, messagePayload(room, QString()));
    manager.observeRoomUnread(room, false);
    QVERIFY2(manager.pendingPayloadCountForTest() == 1,
             "the read walk withdrew a missed-call notice");
    manager.closeRoomNotifications(room);
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
}

void NotificationManagerTest::twoRoomsOfBalloonsOpenNeitherByGuess()
{
    NotificationManager manager;
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
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!a:x"), QStringLiteral("$a2:x")));
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!a:x"), QStringLiteral("$a3:x")));
    QVERIFY(QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked"));
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toString(), QStringLiteral("!a:x"));
}

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
    manager.showGeneric(QStringLiteral("Room invitation"),
                        QStringLiteral("You were invited"));
    auto muted = baseContext();
    muted.roomMode = NotificationManager::Muted;
    manager.processEvent(incomingText(), muted);
    QCOMPARE(delivered.count(), 1);
}

namespace {
const QString kAnn = QStringLiteral("@ann:example.org");
const QString kBea = QStringLiteral("@bea:example.org");

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

TimelineEvent liveEditOf(const TimelineEvent &original, const QString &id,
                         const QString &text, qint64 ms,
                         const QString &sender = QString())
{
    TimelineEvent edit = original;
    edit.eventId = id;
    edit.sender = sender.isEmpty() ? original.sender : sender;
    edit.body = QStringLiteral("* ") + text;
    edit.isEdit = true;
    edit.replacesEventId = original.eventId;
    edit.editNewBody = text;
    edit.editNewBodyAvailable = true;
    edit.pushVerdict = TimelineEvent::PushVerdict::Quiet;
    edit.timestamp = QDateTime::fromMSecsSinceEpoch(ms);
    return edit;
}

QString messageGroup(const QString &room, const QString &thread = QString())
{
    const QChar separator(0x1f);
    return QStringLiteral("m") + separator + room + separator + thread;
}

QString line(const QStringList &parts)
{
    return parts.join(QLatin1Char('|'));
}

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
    act(manager, 1, "mark-read");
    act(manager, 66, "default");
    if (throughClearPendingFor)
        manager.clearPendingFor(QString());
    else
        manager.clearPending();
    act(manager, 65, "mark-read");
    act(manager, 64, "default");
    QMetaObject::invokeMethod(&manager, "onNotificationReplied",
                              Q_ARG(quint32, 63u),
                              Q_ARG(QString, QStringLiteral("hi")));
    act(manager, 2, "mark-read");
    log << QStringLiteral("pending=%1")
               .arg(manager.pendingPayloadCountForTest())
        << QStringLiteral("generation=%1")
               .arg(manager.deliveryGenerationForTest(QString()));
    return log;
}
}

void NotificationManagerTest::anEmptyAccountKeepsTodaysPayloadsEvictionAndTombstones()
{
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

    QVERIFY(baseContext() == baseContext());
    auto other = baseContext();
    other.accountUserId = kBea;
    QVERIFY(!(other == baseContext()));
    other = baseContext();
    other.accountLabel = QStringLiteral("Work");
    QVERIFY(!(other == baseContext()));

    const quint64 before = manager.deliveryGenerationForTest(QString());
    manager.clearPendingFor(QString());
    QCOMPARE(manager.avatarWaitCountForTest(), 0);
    QCOMPARE(manager.deliveryGenerationForTest(QString()), before + 1);

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

void NotificationManagerTest::clearPendingIsExactlyTheForegroundAccountsClear()
{
    const QStringList viaClearPending = runSignOutScript(false);
    const QStringList viaClearPendingFor = runSignOutScript(true);
    QCOMPARE(viaClearPending.size(), 10);
    QCOMPARE(viaClearPending, viaClearPendingFor);

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
    QCOMPARE(opened.count(), 0);
}

void NotificationManagerTest::everyOpenNamesItsAccountFirst()
{
    const QString room = QStringLiteral("!room:example.org");
    const QString root = QStringLiteral("$root:example.org");
    QStringList log;
    NotificationManager manager;
    manager.setBalloonWithdrawableForTest(false);
    recordRouting(manager, log);
    manager.setAccountUserId(kAnn);

    manager.recordPayloadForTest(
        5, messagePayload(MatrixClient::threadTimelineId(room, root),
                          QStringLiteral("$a"), QString(), kAnn));
    act(manager, 5, "default");
    manager.recordPayloadForTest(
        kBea, 6, messagePayload(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b"), QString(), kBea));
    act(manager, 6, "default");
    manager.deliverThroughTrayForTest(
        kBea, messagePayload(QStringLiteral("!t:example.org"),
                             QStringLiteral("$t"), QString(), kBea));
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    manager.deliverThroughTrayForTest(
        messagePayload(QStringLiteral("!x:example.org"),
                       QStringLiteral("$x"), QString(), kAnn));
    manager.deliverThroughTrayForTest(
        kBea, messagePayload(QStringLiteral("!y:example.org"),
                             QStringLiteral("$y"), QString(), kBea));
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
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

    manager.clearPendingFor(QString());
    QCOMPARE(manager.pendingPayloadCountForTest(), 0);
    QCOMPARE(manager.pendingPayloadCountForTest(b), 1);
    QCOMPARE(manager.avatarWaitCountForTest(b), 1);
    QVERIFY(!manager.callRingActiveForTest());
}

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
    QCOMPARE(waits.avatarWaitPayloadForTest(0)
                 .value(QStringLiteral("eventId")).toString(),
             QStringLiteral("$w2"));
}

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
    const quint64 generationA = manager.deliveryGenerationForTest(a);
    const quint64 generationB = manager.deliveryGenerationForTest(b);
    const quint64 generationF = manager.deliveryGenerationForTest(QString());

    manager.clearPendingFor(a);
    QCOMPARE(manager.deliveryGenerationForTest(a), generationA + 1);
    QCOMPARE(manager.deliveryGenerationForTest(b), generationB);
    QCOMPARE(manager.deliveryGenerationForTest(QString()), generationF);

    manager.notifyReplyForTest(
        b, generationB, 7,
        messagePayload(QStringLiteral("!rb7"), QStringLiteral("$b7"),
                       QString(), b));
    QVERIFY2(manager.pendingPayloadCountForTest(b) == 2,
             "clearing account A rejected account B's Notify reply");
    manager.notifyReplyForTest(
        a, generationA, 8,
        messagePayload(QStringLiteral("!ra8"), QStringLiteral("$a8"),
                       QString(), a));
    QCOMPARE(manager.pendingPayloadCountForTest(a), 0);
    manager.notifyReplyForTest(
        QString(), generationF, 9,
        messagePayload(QStringLiteral("!rf9"), QStringLiteral("$f9"),
                       QString(), kAnn));
    QCOMPARE(manager.pendingPayloadCountForTest(), 1);
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
    manager.processEvent(textIn(QStringLiteral("!f:example.org"),
                                QStringLiteral("$f:example.org")),
                         baseContext());
    QCOMPARE(manager.trayAttemptsForTest(), 2);
    QCOMPARE(manager.lastTrayPayloadForTest()
                 .value(QStringLiteral("accountUserId")).toString(),
             kAnn);
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
    manager.deliverThroughTrayForTest(kBea, beas);
    manager.clearPendingFor(QStringLiteral("@cy:example.org"));
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
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
    QCOMPARE(manager.trayAttemptsForTest(), 2);
    QCOMPARE(manager.lastTrayPayloadForTest()
                 .value(QStringLiteral("roomId")).toString(),
             QStringLiteral("!b:example.org"));
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toString(), QStringLiteral("!b:example.org"));
}

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
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$b2:example.org"));
        QSignalSpy opened(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("!b:example.org"));
    }
}

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
    QCOMPARE(manager.pendingBalloonEntriesForTest(), 2);
    QCOMPARE(manager.pendingBalloonMessagesForTest(), 2);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("!b:example.org"));
    QCOMPARE(manager.trayAttemptsForTest(), 3);
    QVERIFY(manager.lastTrayPayloadForTest()
                .value(QStringLiteral("roomId")).toString().isEmpty());
    QCOMPARE(manager.lastTrayBodyForTest(),
             QStringLiteral("2 new messages in 2 rooms"));

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
    failing.setTrayShowsForTest(false);
    TimelineEvent lost = textIn(QStringLiteral("!b:example.org"),
                                QStringLiteral("$b2:example.org"));
    lost.mentionsMe = true;
    failing.processEvent(lost, baseContext());
    QCOMPARE(failing.trayAttemptsForTest(), 2);
    QCOMPARE(failing.pendingBalloonEntriesForTest(), 2);
}

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
    manager.processEvent(textIn(QStringLiteral("!c:example.org"),
                                QStringLiteral("$c1:example.org")),
                         baseContext());
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toString(), QStringLiteral("!c:example.org"));
}

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
        QCOMPARE(manager.trayAttemptsForTest(), 2);
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!b:example.org"));
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 0);
        QVERIFY(manager.balloonOnScreenForTest());
    }
    {
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
    QTRY_VERIFY(!manager.balloonOnScreenForTest());
    QCOMPARE(manager.trayAttemptsForTest(), 1);
    QSignalSpy opened(&manager, &NotificationManager::openRequested);
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 0);
}

void NotificationManagerTest::theBalloonHoldFollowsTheNotificationDuration()
{
    QCOMPARE(TrayIcon::balloonHoldMsFor(5), 6000);
    QCOMPARE(TrayIcon::balloonHoldMsFor(30), 31000);
    QCOMPARE(TrayIcon::balloonHoldMsFor(0), 6000);
    QCOMPARE(TrayIcon::balloonHoldMsFor(-1), 6000);
    QCOMPARE(TrayIcon::balloonHoldMsFor(100000), 301000);
    QVERIFY(TrayIcon::balloonHoldMs() >= 2000);
}

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
    context.previewMode = NotificationManager::SenderOnly;
    QVERIFY(!NotificationManager::decide(fromSync, context)
                 .body.contains(QStringLiteral("example.org")));

    context.senderMemberName = QStringLiteral("@six:example.org");
    context.senderProfileName = QStringLiteral("Six (profile)");
    QCOMPARE(NotificationManager::senderLabel(fromSync, context),
             QStringLiteral("Six (profile)"));
    context.senderMemberName = QStringLiteral("Six in this room");
    QCOMPARE(NotificationManager::senderLabel(fromSync, context),
             QStringLiteral("Six in this room"));
    QCOMPARE(NotificationManager::senderLabel(fromTimeline, context),
             QStringLiteral("Six in this room"));
    TimelineEvent named = fromSync;
    named.senderDisplayName = QStringLiteral("Sixth");
    QCOMPARE(NotificationManager::senderLabel(named, context),
             QStringLiteral("Sixth"));
    TimelineEvent odd = fromTimeline;
    odd.sender = QStringLiteral("@:example.org");
    QCOMPARE(NotificationManager::senderLabel(odd, baseContext()),
             QStringLiteral("@:example.org"));
}

void NotificationManagerTest::aDisplacedBalloonBringsBackNothingReadOrCleared()
{
    {
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
        QTRY_COMPARE(manager.trayAttemptsForTest(), 2);
        QCOMPARE(manager.lastTrayBodyForTest(),
                 QStringLiteral("2 new messages in 2 rooms"));
        manager.clearPendingFor(kBea);
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
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!b:example.org"));
    }
    {
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

void NotificationManagerTest::anEditFromTheLiveSyncRefreshesAWaitingCardOnlyIfValid()
{
    NotificationManager manager;
    parkDeliveries(manager);
    auto context = parkedContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    const TimelineEvent original = incomingText(QStringLiteral("orig words"));
    manager.processEvent(original, context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);

    manager.processEvent(liveEditOf(original, QStringLiteral("$e1:example.org"),
                                    QStringLiteral("new words"), 2000),
                         context);
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QVERIFY2(manager.avatarWaitBodyForTest(0) == QStringLiteral("new words"),
             qPrintable(manager.avatarWaitBodyForTest(0)));
    QCOMPARE(manager.avatarWaitPayloadForTest(0)
                 .value(QStringLiteral("eventId")).toString(),
             original.eventId);
    QCOMPARE(delivered.count(), 0);

    manager.processEvent(liveEditOf(original, QStringLiteral("$e2:example.org"),
                                    QStringLiteral("forged"), 3000,
                                    QStringLiteral("@mallory:example.org")),
                         context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("new words"));
    auto elsewhere = liveEditOf(original, QStringLiteral("$e3:example.org"),
                                QStringLiteral("other room"), 3000);
    elsewhere.roomId = QStringLiteral("!other:example.org");
    manager.processEvent(elsewhere, context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("new words"));
    auto stranger = liveEditOf(original, QStringLiteral("$e4:example.org"),
                               QStringLiteral("not this one"), 3000);
    stranger.replacesEventId = QStringLiteral("$older:example.org");
    manager.processEvent(stranger, context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("new words"));
    auto legacy = liveEditOf(original, QStringLiteral("$e5:example.org"),
                             QStringLiteral("fallback only"), 3000);
    legacy.editNewBody.clear();
    legacy.editNewBodyAvailable = false;
    manager.processEvent(legacy, context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("new words"));
    manager.processEvent(liveEditOf(original, QStringLiteral("$e0:example.org"),
                                    QStringLiteral("first draft"), 1000),
                         context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("new words"));
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QCOMPARE(delivered.count(), 0);

    context.previewMode = NotificationManager::Private;
    manager.processEvent(liveEditOf(original, QStringLiteral("$e6:example.org"),
                                    QStringLiteral("private words"), 4000),
                         context);
    QVERIFY(!manager.avatarWaitBodyForTest(0)
                 .contains(QStringLiteral("private words")));

    context.previewMode = NotificationManager::SenderAndMessage;
    manager.setUndecryptableHoldMsForTest(0);
    auto sealed = liveEditOf(original, QStringLiteral("$e7:example.org"),
                             QString(), 5000);
    sealed.body.clear();
    sealed.editNewBodyAvailable = false;
    sealed.undecryptable = true;
    sealed.isEncrypted = true;
    manager.processEvent(sealed, context);
    QVERIFY(!manager.avatarWaitBodyForTest(0).contains(QStringLiteral("* ")));
    auto opened = liveEditOf(original, sealed.eventId,
                             QStringLiteral("decrypted words"), 5000);
    opened.isEncrypted = true;
    manager.processDecrypted(opened, context);
    QCOMPARE(manager.avatarWaitBodyForTest(0), QStringLiteral("decrypted words"));
    QCOMPARE(manager.avatarWaitCountForTest(), 1);
    QCOMPARE(delivered.count(), 0);
}

void NotificationManagerTest::anEditShowsTheBalloonOnScreenAgainWithTheNewText()
{
    auto context = baseContext();
    context.previewMode = NotificationManager::SenderAndMessage;
    const QString roomA = QStringLiteral("!a:example.org");
    const QString roomB = QStringLiteral("!b:example.org");
    {
        NotificationManager manager;
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(60000);
        TimelineEvent a = textIn(roomA, QStringLiteral("$a1:example.org"));
        a.body = QStringLiteral("toast-orig");
        manager.processEvent(a, context);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(manager.lastTrayBodyForTest(), QStringLiteral("toast-orig"));

        manager.processEvent(liveEditOf(a, QStringLiteral("$a2:example.org"),
                                        QStringLiteral("toast EDITED"), 2000),
                             context);
        QVERIFY2(manager.trayAttemptsForTest() == 2,
                 "the balloon on screen kept the text from before the edit");
        QCOMPARE(manager.lastTrayBodyForTest(), QStringLiteral("toast EDITED"));
        QVERIFY(!manager.lastTrayPlatformSoundForTest());
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("eventId")).toString(),
                 a.eventId);
        QVERIFY(manager.balloonOnScreenForTest());

        a.body = QStringLiteral("set text");
        manager.processUpdated(a, context);
        QCOMPARE(manager.trayAttemptsForTest(), 3);
        QCOMPARE(manager.lastTrayBodyForTest(), QStringLiteral("set text"));

        TimelineEvent b = textIn(roomB, QStringLiteral("$b1:example.org"));
        b.body = QStringLiteral("z2-orig");
        manager.processEvent(b, context);
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 1);
        manager.processEvent(liveEditOf(b, QStringLiteral("$b2:example.org"),
                                        QStringLiteral("z2-EDITED"), 2000),
                             context);
        QCOMPARE(manager.trayAttemptsForTest(), 3);
        QCOMPARE(manager.pendingBalloonEntriesForTest(), 1);
        QSignalSpy opened(&manager, &NotificationManager::openRequested);
        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), roomA);
        QCOMPARE(manager.trayAttemptsForTest(), 4);
        QCOMPARE(manager.lastTrayBodyForTest(), QStringLiteral("z2-EDITED"));
    }
    {
        NotificationManager manager;
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(50);
        TimelineEvent a = textIn(roomA, QStringLiteral("$a1:example.org"));
        manager.processEvent(a, context);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QTRY_VERIFY(!manager.balloonOnScreenForTest());
        manager.processEvent(liveEditOf(a, QStringLiteral("$a2:example.org"),
                                        QStringLiteral("late edit"), 2000),
                             context);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
    }
}

QTEST_MAIN(NotificationManagerTest)
#include "NotificationManagerTest.moc"
