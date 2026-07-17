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
    void threadRepliesCarryThreadIdentity()
    {
        auto context = baseContext();
        TimelineEvent reply = incomingText(QStringLiteral("thread reply"));
        reply.threadRootId = QStringLiteral("$root:example.org");
        const auto decision = NotificationManager::decide(reply, context);
        QVERIFY(decision.notify);
    }
};

QTEST_MAIN(NotificationManagerTest)
#include "NotificationManagerTest.moc"
