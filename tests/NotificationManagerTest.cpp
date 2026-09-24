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

NotificationManager::Context baseContext()
{
    NotificationManager::Context context;
    context.selfUserId = QStringLiteral("@alice:example.org");
    context.roomName = QStringLiteral("Lightning Dev");
    context.previewMode = NotificationManager::SenderOnly;
    context.notificationsEnabled = true;
    return context;
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
    void directMessageNotifiesWithSenderOnlyDefault()
    {
        const auto decision =
            NotificationManager::decide(incomingText(), baseContext());
        QVERIFY(decision.notify);
        QCOMPARE(decision.title, QStringLiteral("Lightning Dev"));
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
        QCOMPARE(dm.title, QStringLiteral("Bob"));
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

        // Room-wide mention also passes MentionsOnly.
        TimelineEvent roomMention = incomingText();
        roomMention.mentionsRoom = true;
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

        // The two oldest (ids 1, 2) were evicted: clicking them routes
        // nowhere.
        QMetaObject::invokeMethod(&manager, "onActionInvoked",
                                  Q_ARG(quint32, 1u),
                                  Q_ARG(QString, QStringLiteral("default")));
        QCOMPARE(spy.count(), 0);

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
    // The balloon is consumed: a second click opens nothing.
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(opened.count(), 1);
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
    QVERIFY2(opened.isEmpty(),
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

    // An empty payload (what sign-out leaves) is still refused.
    QSignalSpy afterClear(&manager, &NotificationManager::openRequested);
    manager.clearPending();
    QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
    QCOMPARE(afterClear.count(), 0);
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

QTEST_MAIN(NotificationManagerTest)
#include "NotificationManagerTest.moc"
