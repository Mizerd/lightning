// Live accounts (Labs), Phase 1: notification flood control for background
// accounts, the one card for a message two live accounts both see
// (DECISION 12), withdrawal from a background account's counts, and the
// account label in titles. Pure NotificationManager: no daemon (the session
// bus is pointed at a dead address, so every card takes the tray path and is
// counted there), no AppController.
//
// Each case names the mutation that makes it fail in
// ~/Documents/LLM/Lightning/Tasks/2026-10-16-la1-A.md.

#include "notifications/NotificationManager.h"

#include "matrix/TimelineEvent.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
const QString kAnn = QStringLiteral("@ann:example.org");
const QString kBea = QStringLiteral("@bea:example.org");
const QString kCat = QStringLiteral("@cat:example.org");
const QString kRoomA = QStringLiteral("!a:example.org");
const QString kRoomB = QStringLiteral("!b:example.org");
const QString kRoomC = QStringLiteral("!c:example.org");

TimelineEvent message(const QString &roomId, const QString &eventId,
                      const QString &body = QStringLiteral("hello"))
{
    TimelineEvent event;
    event.eventId = eventId;
    event.roomId = roomId;
    event.sender = QStringLiteral("@dan:example.org");
    event.senderDisplayName = QStringLiteral("Dan");
    event.body = body;
    event.type = TimelineEvent::TextMessage;
    event.status = TimelineEvent::Sent;
    return event;
}

// The foreground account's context: no account, as today.
NotificationManager::Context foregroundContext(
    const QString &label = QStringLiteral("Ann"))
{
    NotificationManager::Context context;
    context.selfUserId = kAnn;
    context.roomName = QStringLiteral("Lightning Dev");
    context.previewMode = NotificationManager::SenderAndMessage;
    context.notificationsEnabled = true;
    context.accountLabel = label;
    return context;
}

// A background account's context, as its host builds it.
NotificationManager::Context hostContext(const QString &userId,
                                         const QString &label)
{
    NotificationManager::Context context = foregroundContext(label);
    context.selfUserId = userId;
    context.accountUserId = userId;
    return context;
}

QString iso(const QString &name)
{
    return QChar(0x2068) + name + QChar(0x2069);
}

QString dot()
{
    return QStringLiteral(" · ");
}

// Every card is counted on the tray path, and a tray click resolves to the
// last balloon's payload (no gate: every balloon is shown).
void prepare(NotificationManager &manager)
{
    manager.setBalloonWithdrawableForTest(false);
    manager.setTrayShowsForTest(true);
}
} // namespace

class NotificationFloodTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // As the notification-manager test's CTest property: no daemon, so
        // the tray path counts every card whatever desktop runs the suite.
        qputenv("DBUS_SESSION_BUS_ADDRESS",
                "unix:path=/nonexistent/lightning-no-daemon");
    }

    // One card per (account, room), updated as "N new messages": the second
    // and third messages for a room whose card is still held by the rate
    // limit merge into it, and it leaves once, counting all three.
    // Fails with: the merge loop in offerHostCard removed (two waiting);
    // the host body branch removed (the text, not a count).
    void aBackgroundAccountsRoomIsOneCardThatCounts()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setHostCardIntervalMsForTest(300);
        const auto bea = hostContext(kBea, QStringLiteral("Bea"));

        manager.processEvent(message(kRoomA, QStringLiteral("$1")), bea);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("accountUserId")).toString(),
                 kBea);
        manager.processEvent(message(kRoomA, QStringLiteral("$2")), bea);
        manager.processEvent(message(kRoomA, QStringLiteral("$3")), bea);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(manager.deferredHostCardCountForTest(), 1);

        QTRY_COMPARE_WITH_TIMEOUT(manager.trayAttemptsForTest(), 2, 3000);
        QCOMPARE(manager.deferredHostCardCountForTest(), 0);
        QCOMPARE(manager.lastTrayBodyForTest(),
                 QStringLiteral("3 new messages"));
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$3"));
        // Nothing else was waiting.
        QTest::qWait(400);
        QCOMPARE(manager.trayAttemptsForTest(), 2);
    }

    // At most one NEW card per interval across every background account:
    // a second account's first card waits behind the first account's, in
    // arrival order. Fails with: the limiter kept per account (Cat's card
    // leaves at once).
    void newCardsAreSpacedAcrossEveryBackgroundAccount()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setHostCardIntervalMsForTest(400);
        QElapsedTimer clock;
        clock.start();
        manager.processEvent(message(kRoomA, QStringLiteral("$b1")),
                             hostContext(kBea, QStringLiteral("Bea")));
        manager.processEvent(message(kRoomB, QStringLiteral("$c1")),
                             hostContext(kCat, QStringLiteral("Cat")));
        manager.processEvent(message(kRoomC, QStringLiteral("$b2")),
                             hostContext(kBea, QStringLiteral("Bea")));
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(manager.deferredHostCardCountForTest(), 2);

        QTRY_COMPARE_WITH_TIMEOUT(manager.trayAttemptsForTest(), 2, 3000);
        QVERIFY2(clock.elapsed() >= 350,
                 "a second background account's new card left inside the "
                 "interval");
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("accountUserId")).toString(),
                 kCat);
        QTRY_COMPARE_WITH_TIMEOUT(manager.trayAttemptsForTest(), 3, 3000);
        QVERIFY(clock.elapsed() >= 750);
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("roomId")).toString(),
                 kRoomC);
    }

    // The foreground account's cards are today's in live-accounts mode: no
    // rate limit and the latest line, not a count. Fails with: hostCard
    // keyed on the mode alone (the foreground's cards wait too).
    void foregroundCardsAreTodaysInLiveMode()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setHostCardIntervalMsForTest(10000);
        const auto ann = foregroundContext();
        manager.processEvent(message(kRoomA, QStringLiteral("$1")), ann);
        manager.processEvent(message(kRoomB, QStringLiteral("$2")), ann);
        manager.processEvent(
            message(kRoomA, QStringLiteral("$3"), QStringLiteral("again")),
            ann);
        QCOMPARE(manager.trayAttemptsForTest(), 3);
        QCOMPARE(manager.deferredHostCardCountForTest(), 0);
        QVERIFY2(manager.lastTrayBodyForTest().contains(QStringLiteral("again")),
                 "the foreground's second message in a room became a count");
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("accountUserId")).toString(),
                 kAnn);
    }

    // DECISION 12: one message seen by two live accounts is ONE card,
    // labelled with both; its open goes to the foreground account, which is
    // one of them. Fails with: the merge in deliverDecision removed (a second
    // card); openAccountFor ignoring the foreground.
    void oneMessageSeenByTwoLiveAccountsIsOneCard()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setAccountLabelsShown(true);
        QSignalSpy opened(&manager,
                          &NotificationManager::openRequestedForAccount);
        const TimelineEvent shared = message(kRoomA, QStringLiteral("$shared"));
        const QString base =
            NotificationManager::decide(shared, foregroundContext()).title;

        manager.processEvent(shared, foregroundContext());
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(manager.lastTrayTitleForTest(), base + dot() + iso(QStringLiteral("Ann")));

        manager.processEvent(shared, hostContext(kBea, QStringLiteral("Bea")));
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(manager.deferredHostCardCountForTest(), 0);
        QCOMPARE(manager.eventCardAccountsForTest(kRoomA, QStringLiteral("$shared")),
                 (QStringList{ kAnn, kBea }));
        QCOMPARE(manager.eventCardTitleForTest(kRoomA, QStringLiteral("$shared")),
                 base + dot() + iso(QStringLiteral("Ann")) + QStringLiteral(", ")
                     + iso(QStringLiteral("Bea")));

        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), kAnn);
        QCOMPARE(opened.at(0).at(1).toString(), kRoomA);
    }

    // A shared card that is not the foreground's opens with the most
    // recently used of its accounts, not with whichever raised it first.
    // Fails with: openAccountFor returning the payload's owner (Cat).
    void aSharedCardOpensWithTheMostRecentlyUsedAccount()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setLiveAccountsMode(true);
        // Recency, newest first: Ann, Bea, Cat. Ann is in front.
        manager.setAccountUserId(kCat);
        manager.setAccountUserId(kBea);
        manager.setAccountUserId(kAnn);
        QSignalSpy opened(&manager,
                          &NotificationManager::openRequestedForAccount);
        const TimelineEvent shared = message(kRoomB, QStringLiteral("$s"));
        manager.processEvent(shared, hostContext(kCat, QStringLiteral("Cat")));
        manager.processEvent(shared, hostContext(kBea, QStringLiteral("Bea")));
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QCOMPARE(manager.eventCardAccountsForTest(kRoomB, QStringLiteral("$s")),
                 (QStringList{ kCat, kBea }));
        QMetaObject::invokeMethod(&manager, "onFallbackMessageClicked");
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), kBea);
    }

    // A background account's counts say a room is read: only that account's
    // cards for the room go, a card still held by the rate limit included,
    // and never the foreground's. Fails with: observeRoomUnreadFor acting on
    // the "" table; withdrawWhere not dropping a held card.
    void aRoomReadElsewhereWithdrawsOnlyThatAccountsCards()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setAwaitUnreadMsForTest(0);
        manager.setHostCardIntervalMsForTest(10000);
        const auto payload = [](const QString &event, const QString &account) {
            return QVariantMap{ { QStringLiteral("roomId"), kRoomA },
                                { QStringLiteral("eventId"), event },
                                { QStringLiteral("threadRootId"), QString() },
                                { QStringLiteral("accountUserId"), account } };
        };
        manager.recordPayloadForTest(kBea, 11, payload(QStringLiteral("$b"), kBea));
        manager.recordPayloadForTest(QString(), 12,
                                     payload(QStringLiteral("$a"), kAnn));
        // A card held by the rate limit, for the same room.
        manager.processEvent(message(kRoomB, QStringLiteral("$first")),
                             hostContext(kBea, QStringLiteral("Bea")));
        manager.processEvent(message(kRoomA, QStringLiteral("$held")),
                             hostContext(kBea, QStringLiteral("Bea")));
        QCOMPARE(manager.deferredHostCardCountForTest(), 1);

        manager.observeRoomUnreadFor(kBea, kRoomA, false);
        QCOMPARE(manager.pendingPayloadCountForTest(kBea), 0);
        QCOMPARE(manager.pendingPayloadCountForTest(QString()), 1);
        QCOMPARE(manager.deferredHostCardCountForTest(), 0);

        // The foreground's own walk, as today, for its own card.
        manager.observeRoomUnread(kRoomA, false);
        QCOMPARE(manager.pendingPayloadCountForTest(QString()), 0);
    }

    // §18 round 1, L14: the tray walk asks every read room on every refresh.
    // Once a room was checked since the account's latest card it costs no
    // pass; a new card for that account makes it look again. Fails with: the
    // generation memo removed (one pass per call, 200).
    void aReadRoomIsCheckedOncePerCardNotPerRefresh()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setAwaitUnreadMsForTest(0);
        const auto payload = [](const QString &room, const QString &event) {
            return QVariantMap{ { QStringLiteral("roomId"), room },
                                { QStringLiteral("eventId"), event },
                                { QStringLiteral("threadRootId"), QString() },
                                { QStringLiteral("accountUserId"), kBea } };
        };
        // A card of Bea's that stays (another room), as after a notice
        // nobody dismissed.
        manager.recordPayloadForTest(kBea, 31, payload(kRoomB, QStringLiteral("$keep")));
        const int before = manager.hostUnreadPassesForTest();
        for (int refresh = 0; refresh < 200; ++refresh)
            manager.observeRoomUnreadFor(kBea, kRoomA, false);
        QCOMPARE(manager.hostUnreadPassesForTest(), before + 1);
        QCOMPARE(manager.pendingPayloadCountForTest(kBea), 1);
        // A new card for Bea, in the room that reads as read: one more pass,
        // and it is withdrawn.
        manager.recordPayloadForTest(kBea, 32, payload(kRoomA, QStringLiteral("$new")));
        manager.observeRoomUnreadFor(kBea, kRoomA, false);
        manager.observeRoomUnreadFor(kBea, kRoomA, false);
        QCOMPARE(manager.hostUnreadPassesForTest(), before + 2);
        QCOMPARE(manager.pendingPayloadCountForTest(kBea), 1);
    }

    // Outside live-accounts mode the twin does nothing at all.
    // Fails with: the mode check removed from observeRoomUnreadFor.
    void theUidTwinIsInertWithTheModeOff()
    {
        NotificationManager manager;
        manager.setAwaitUnreadMsForTest(0);
        manager.recordPayloadForTest(
            kBea, 21,
            QVariantMap{ { QStringLiteral("roomId"), kRoomA },
                         { QStringLiteral("eventId"), QStringLiteral("$x") },
                         { QStringLiteral("threadRootId"), QString() },
                         { QStringLiteral("accountUserId"), kBea } });
        manager.observeRoomUnreadFor(kBea, kRoomA, false);
        QCOMPARE(manager.pendingPayloadCountForTest(kBea), 1);
    }

    // "Room · account" only while more than one account is live, and never
    // with the mode off. Fails with: accountSuffix ignoring
    // m_accountLabelsShown, or ignoring the mode.
    void theTitleNamesTheAccountOnlyWhileSeveralAreLive()
    {
        const TimelineEvent first = message(kRoomA, QStringLiteral("$t1"));
        const QString base =
            NotificationManager::decide(first, foregroundContext()).title;
        {
            NotificationManager manager;
            prepare(manager);
            manager.setAccountUserId(kAnn);
            manager.setLiveAccountsMode(true);
            manager.processEvent(first, foregroundContext());
            QCOMPARE(manager.lastTrayTitleForTest(), base);
            manager.setAccountLabelsShown(true);
            manager.processEvent(message(kRoomB, QStringLiteral("$t2")),
                                 foregroundContext());
            QCOMPARE(manager.lastTrayTitleForTest(),
                     base + dot() + iso(QStringLiteral("Ann")));
            // A label nobody set: the localpart, never the full id.
            manager.processEvent(message(kRoomC, QStringLiteral("$t3")),
                                 hostContext(kBea, QString()));
            QCOMPARE(manager.lastTrayTitleForTest(),
                     base + dot() + iso(QStringLiteral("bea")));
        }
        {
            NotificationManager manager;
            prepare(manager);
            manager.setAccountUserId(kAnn);
            manager.setAccountLabelsShown(true);
            manager.processEvent(first, foregroundContext());
            QCOMPARE(manager.lastTrayTitleForTest(), base);
        }
    }

    // §18 round 1, MUST 8: an edit (processUpdated) of a background
    // account's card still WAITING for its turn in the rate limit updated
    // nothing (refreshCard never walked that queue), and every refreshed copy
    // lost the account suffix. Now the waiting card leaves with the edited
    // text and its label. Fails with: the m_hostCardQueue loop removed (the
    // old text leaves), or decision.title used for the refresh (no label).
    void anEditOfAWaitingBackgroundCardLeavesWithTheTextAndTheLabel()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setAccountLabelsShown(true);
        manager.setHostCardIntervalMsForTest(300);
        const auto bea = hostContext(kBea, QStringLiteral("Bea"));
        manager.processEvent(message(kRoomA, QStringLiteral("$w1")), bea);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        TimelineEvent waiting = message(kRoomB, QStringLiteral("$w2"),
                                        QStringLiteral("before the edit"));
        manager.processEvent(waiting, bea);
        QCOMPARE(manager.deferredHostCardCountForTest(), 1);
        waiting.body = QStringLiteral("after the edit");
        manager.processUpdated(waiting, bea);
        QCOMPARE(manager.trayAttemptsForTest(), 1);   // still waiting its turn
        QTRY_COMPARE_WITH_TIMEOUT(manager.trayAttemptsForTest(), 2, 3000);
        QCOMPARE(manager.lastTrayPayloadForTest().value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$w2"));
        QVERIFY2(manager.lastTrayBodyForTest().contains(QStringLiteral("after the edit")),
                 qPrintable(manager.lastTrayBodyForTest()));
        const QString base = NotificationManager::decide(waiting, bea).title;
        QCOMPARE(manager.lastTrayTitleForTest(), base + dot() + iso(QStringLiteral("Bea")));
    }

    // ... and the balloon ON SCREEN (one-balloon gate), shown again with the
    // edit, keeps the label it was raised with. Fails with decision.title in
    // the shown-entries branch.
    void anEditOfABackgroundBalloonOnScreenKeepsItsLabel()
    {
        NotificationManager manager;
        manager.setTrayShowsForTest(true);
        manager.setBalloonWithdrawableForTest(true);
        manager.setBalloonHoldMsForTest(60000);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setAccountLabelsShown(true);
        const auto bea = hostContext(kBea, QStringLiteral("Bea"));
        TimelineEvent shown = message(kRoomA, QStringLiteral("$b1"),
                                      QStringLiteral("before the edit"));
        manager.processEvent(shown, bea);
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        const QString base = NotificationManager::decide(shown, bea).title;
        const QString labelled = base + dot() + iso(QStringLiteral("Bea"));
        QCOMPARE(manager.lastTrayTitleForTest(), labelled);
        shown.body = QStringLiteral("after the edit");
        manager.processUpdated(shown, bea);
        QCOMPARE(manager.trayAttemptsForTest(), 2);
        QVERIFY(manager.lastTrayBodyForTest().contains(QStringLiteral("after the edit")));
        QCOMPARE(manager.lastTrayTitleForTest(), labelled);
    }

    // With the mode off every path is Phase 0's: background-account
    // contexts are neither limited, counted nor merged. This is the flag-off
    // half of the gate; NotificationManagerTest holds the golden parity.
    // Fails with: any live-accounts branch not gated on the mode.
    void withTheModeOffEveryCardIsPhaseZeros()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setHostCardIntervalMsForTest(10000);
        const TimelineEvent shared = message(kRoomA, QStringLiteral("$same"));
        manager.processEvent(shared, hostContext(kBea, QStringLiteral("Bea")));
        manager.processEvent(shared, hostContext(kCat, QStringLiteral("Cat")));
        manager.processEvent(
            message(kRoomA, QStringLiteral("$next"), QStringLiteral("more")),
            hostContext(kBea, QStringLiteral("Bea")));
        QCOMPARE(manager.trayAttemptsForTest(), 3);
        QCOMPARE(manager.deferredHostCardCountForTest(), 0);
        QVERIFY(manager.lastTrayBodyForTest().contains(QStringLiteral("more")));
        QVERIFY(manager.eventCardTitleForTest(kRoomA, QStringLiteral("$same"))
                    .isEmpty());
    }

    // A background account cleared (removed, signed out): what it had
    // waiting goes, and so does its place on another account's shared card.
    // Fails with: forgetLiveAccountsStateFor not called from clearPendingFor.
    void clearingABackgroundAccountDropsWhatItHadWaiting()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setHostCardIntervalMsForTest(10000);
        const TimelineEvent shared = message(kRoomA, QStringLiteral("$sh"));
        manager.processEvent(shared, foregroundContext());
        manager.processEvent(shared, hostContext(kBea, QStringLiteral("Bea")));
        manager.processEvent(message(kRoomB, QStringLiteral("$b1")),
                             hostContext(kBea, QStringLiteral("Bea")));
        manager.processEvent(message(kRoomC, QStringLiteral("$b2")),
                             hostContext(kBea, QStringLiteral("Bea")));
        QCOMPARE(manager.deferredHostCardCountForTest(), 1);
        manager.clearPendingFor(kBea);
        QCOMPARE(manager.deferredHostCardCountForTest(), 0);
        QCOMPARE(manager.eventCardAccountsForTest(kRoomA, QStringLiteral("$sh")),
                 (QStringList{ kAnn }));
        QTest::qWait(50);
        QCOMPARE(manager.trayAttemptsForTest(), 2);
    }

    // Past the waiting cap the oldest card that is not a mention goes; a
    // mention is the last to go. Fails with: dropping the oldest waiting
    // card whatever it is (the mention, queued first, is lost).
    void aFullWaitingQueueKeepsItsMentions()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setHostCardIntervalMsForTest(20);
        QSignalSpy delivered(&manager, &NotificationManager::messageDelivered);
        const auto bea = hostContext(kBea, QStringLiteral("Bea"));
        manager.processEvent(message(QStringLiteral("!r0:x"), QStringLiteral("$0")),
                             bea);
        TimelineEvent mention =
            message(QStringLiteral("!mention:x"), QStringLiteral("$m"));
        mention.mentionsMe = true;
        manager.processEvent(mention, bea);
        for (int i = 1; i <= 64; ++i) {
            manager.processEvent(
                message(QStringLiteral("!r%1:x").arg(i),
                        QStringLiteral("$e%1").arg(i)),
                bea);
        }
        QCOMPARE(manager.deferredHostCardCountForTest(), 64);
        QTRY_COMPARE_WITH_TIMEOUT(delivered.count(), 65, 10000);
        QStringList rooms;
        bool mentionDelivered = false;
        for (const QList<QVariant> &args : std::as_const(delivered)) {
            rooms.append(args.at(0).toString());
            if (args.at(0).toString() == QStringLiteral("!mention:x"))
                mentionDelivered = args.at(1).toBool();
        }
        QVERIFY2(mentionDelivered, "a waiting mention was dropped");
        QVERIFY2(!rooms.contains(QStringLiteral("!r1:x")),
                 "the oldest non-mention card was not the one dropped");

        // The dropped card's room has no card on its way any more: its next
        // message is a new card counted from one, not "2 new messages" for
        // a card that was never raised (on a daemon it would also have been
        // taken for an in-place update and skipped the rate limit). Fails
        // with: the group reset on a drop removed.
        manager.processEvent(
            message(QStringLiteral("!r1:x"), QStringLiteral("$again")), bea);
        QTRY_COMPARE_WITH_TIMEOUT(delivered.count(), 66, 5000);
        QCOMPARE(delivered.last().at(0).toString(), QStringLiteral("!r1:x"));
        QVERIFY2(!manager.lastTrayBodyForTest().contains(QStringLiteral("new messages")),
                 qPrintable(manager.lastTrayBodyForTest()));
    }

    // A message one live account was mentioned in, already on a card another
    // account raised without a mention: the card becomes a mention (flag,
    // alert, a balloon that breaks through) rather than absorbing it
    // silently. And when that card has moved on to a newer message, the
    // mention is its account's own card. Fails with: the upgrade removed
    // from mergeIntoEventCard() (one tray attempt, no mention); the moved-on
    // case merged silently (no third attempt).
    void aMentionFoldedIntoACardMakesItAMention()
    {
        NotificationManager manager;
        prepare(manager);
        manager.setAccountUserId(kAnn);
        manager.setLiveAccountsMode(true);
        manager.setAccountLabelsShown(true);
        const TimelineEvent shared = message(kRoomA, QStringLiteral("$shared"));
        TimelineEvent sharedMention = shared;
        sharedMention.mentionsMe = true;
        const auto bea = hostContext(kBea, QStringLiteral("Bea"));
        QVERIFY(NotificationManager::decide(sharedMention, bea).mention);

        manager.processEvent(shared, foregroundContext());
        QCOMPARE(manager.trayAttemptsForTest(), 1);
        QVERIFY(!manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("mention")).toBool());

        manager.processEvent(sharedMention, bea);
        QCOMPARE(manager.trayAttemptsForTest(), 2);
        const QVariantMap upgraded = manager.lastTrayPayloadForTest();
        QVERIFY(upgraded.value(QStringLiteral("mention")).toBool());
        QCOMPARE(upgraded.value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$shared"));
        QCOMPARE(upgraded.value(QStringLiteral("accountUserIds")).toStringList(),
                 (QStringList{ kAnn, kBea }));
        QCOMPARE(manager.eventCardAccountsForTest(kRoomA, QStringLiteral("$shared")),
                 (QStringList{ kAnn, kBea }));

        // Moved on: Ann's card for room B now stands for a newer message.
        const TimelineEvent older = message(kRoomB, QStringLiteral("$older"));
        manager.processEvent(older, foregroundContext());
        manager.processEvent(message(kRoomB, QStringLiteral("$newer")),
                             foregroundContext());
        const int before = manager.trayAttemptsForTest();
        TimelineEvent olderMention = older;
        olderMention.mentionsMe = true;
        manager.processEvent(olderMention, bea);
        QCOMPARE(manager.trayAttemptsForTest(), before + 1);
        QCOMPARE(manager.lastTrayPayloadForTest()
                     .value(QStringLiteral("accountUserId")).toString(),
                 kBea);
        QVERIFY(manager.lastTrayPayloadForTest()
                    .value(QStringLiteral("mention")).toBool());
    }
};

QTEST_MAIN(NotificationFloodTest)
#include "NotificationFloodTest.moc"
