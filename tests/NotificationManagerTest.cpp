// v0.6.0 checkpoint 11: the pure notification decision policy — privacy
// modes, mention handling, local per-room modes, active-room suppression,
// and the never-expose-ciphertext contract. No DBus, no homeserver.

#include "notifications/NotificationManager.h"

#include "matrix/TimelineEvent.h"

#include <QtTest/QtTest>

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

        // Poll: question only — never the MSC3381 answer-list fallback.
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

    // Undecryptable events NEVER leak anything, even in the fullest mode.
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

    // Reported from a real desktop: opening a room for the first time after
    // a restart delivered a notification for every message it loaded. The
    // cause is that opening a room subscribes it in sliding sync, so its
    // recent history arrives as ordinary live appends — while the view is
    // still hydrating, which is exactly when roomVisibleAtLatest cannot be
    // true, because that flag requires a settled, stuck-to-bottom view.
    void hydratingOpenRoomSuppresses()
    {
        auto context = baseContext();
        context.roomVisibleAtLatest = false; // the view has not settled yet
        context.roomHydrating = true;
        QVERIFY(!NotificationManager::decide(incomingText(), context).notify);
        // A mention in the room being read is suppressed on the same ground
        // — the message is on screen the moment the view settles.
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

    // Thread replies notify like messages; the click payload identity is the
    // event's thread root (delivery-side contract: payload = room, event,
    // threadRootId only — no tokens are even available to this layer).
    // v0.6.1 (thread work): thread replies are hidden from the main timeline,
    // but a reply still reaches the notification path through the sync-level
    // room-message handler (real room id + m.thread root), independent of any
    // rendered main-timeline row. It is then subject to the SAME push-rule
    // policy as any message, and its click payload routes to the thread.
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

        // Mentions-only: a plain thread reply is silent; a mentioning one is not.
        context.roomMode = NotificationManager::MentionsOnly;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
        TimelineEvent mentioningReply = reply;
        mentioningReply.mentionsMe = true;
        QVERIFY(NotificationManager::decide(mentioningReply, context).notify);

        // Initial-sync backlog: never (no cold-start storm from threads).
        context.roomMode = NotificationManager::AllMessages;
        context.initialSyncComplete = false;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
        context.initialSyncComplete = true;

        // Active, visible-at-latest room: suppressed.
        context.roomVisibleAtLatest = true;
        QVERIFY(!NotificationManager::decide(reply, context).notify);
    }

    // The click payload for a thread reply carries the thread root id, so
    // activating the notification opens the correct thread (not just the room).
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

    // v0.6.1 regression (cold-start message storm): events applied before the
    // initial sync completes are backlog history, never fresh activity, and
    // must be suppressed regardless of privacy mode or mention state.
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

    // v0.6.1 regression (cold-start invite storm): pending invites present at
    // launch are seeded silently; only invites seen after the initial sync
    // (and not already announced) raise a notification.
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

    // v0.6.1: notification sound rides on the notify decision and the mode
    // narrows which eligible notifications also sound.
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

    // v0.6.1 regression (click routing lost after 64 pending): the bounded
    // map evicts the OLDEST payloads (FIFO) instead of clearing everything, so
    // the most recent notifications remain clickable.
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

        // Record 66 payloads (ids 1..66) — two past the cap of 64.
        for (quint32 id = 1; id <= 66; ++id)
            manager.recordPayloadForTest(id, payload(QStringLiteral("!r%1")
                                                         .arg(id)));
        QCOMPARE(manager.pendingPayloadCountForTest(), 64);

        // The two oldest (ids 1, 2) were evicted: clicking them routes nowhere.
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
};

QTEST_MAIN(NotificationManagerTest)
#include "NotificationManagerTest.moc"
