
#include "matrix/MatrixClient.h"
#include "models/ReadReceiptCoordinator.h"
#include "models/TimelineModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

const QString kRoomA = QStringLiteral("!a:example.org");
const QString kRoomB = QStringLiteral("!b:example.org");

TimelineEvent makeEvent(const QString &roomId, const QString &eventId,
                        qint64 timestampMs,
                        TimelineEvent::Status status = TimelineEvent::Sent)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.roomId = roomId;
    e.sender = QStringLiteral("@other:example.org");
    e.body = QStringLiteral("x");
    e.timestamp = QDateTime::fromMSecsSinceEpoch(timestampMs);
    e.status = status;
    return e;
}

TimelineEvent makeVirtual(const QString &roomId)
{
    TimelineEvent e;
    e.roomId = roomId;
    e.type = TimelineEvent::DateDivider;
    return e;
}

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QHash<QString, QList<TimelineEvent>> timelines;
    QStringList receiptRooms;
    QStringList receiptEvents;
    QList<RoomInfo> roomInfos;
    QStringList markedReadRooms;
    bool markReadSupported = true;
    bool supportsMarkRoomRead() const override { return markReadSupported; }
    void setRoomMarkedUnread(const QString &roomId, bool unread) override
    {
        for (auto &room : roomInfos) {
            if (room.id == roomId)
                room.markedUnread = unread;
        }
        Q_EMIT roomsChanged();
    }
    void markRoomRead(const QString &roomId) override
    {
        markedReadRooms.append(roomId);
        for (auto &room : roomInfos) {
            if (room.id == roomId)
                room.markedUnread = false;
        }
        Q_EMIT roomsChanged();
    }

    void appendEvent(const TimelineEvent &event)
    {
        timelines[event.roomId].append(event);
        Q_EMIT eventAppended(event.roomId, event);
    }
    void resetTimeline(const QString &roomId, const QList<TimelineEvent> &events)
    {
        timelines[roomId] = events;
        Q_EMIT timelineReset(roomId);
    }

    QList<TimelineEvent> timeline(const QString &roomId) const override
    {
        return timelines.value(roomId);
    }
    void sendReadReceipt(const QString &roomId, const QString &eventId) override
    {
        receiptRooms.append(roomId);
        receiptEvents.append(eventId);
    }
    QStringList fullyReadRooms;
    QStringList fullyReadEvents;
    void setFullyReadMarker(const QString &roomId, const QString &eventId) override
    {
        fullyReadRooms.append(roomId);
        fullyReadEvents.append(eventId);
    }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString userId = QStringLiteral("@me:example.org");
    QString currentUserId() const override { return userId; }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return roomInfos; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

struct Rig {
    FakeClient client;
    TimelineModel model;
    ReadReceiptCoordinator coordinator;

    explicit Rig(int debounceMs = 1)
    {
        model.setClient(&client);
        coordinator.setClient(&client);
        coordinator.setTimelineModel(&model);
        coordinator.setDebounceMs(debounceMs);
    }

    void makeEligible()
    {
        coordinator.setWindowActive(true);
        coordinator.setTimelineVisible(true);
        coordinator.setNearBottom(true);
    }

    void accept(const QString &roomId, const QString &eventId)
    {
        Q_EMIT client.readReceiptAccepted(roomId, eventId);
    }
    void fail(const QString &roomId, const QString &eventId)
    {
        Q_EMIT client.readReceiptFailed(roomId, eventId);
    }
};

}

class ReadReceiptCoordinatorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void receiptSentWhenAllConditionsHold()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = {
            makeEvent(kRoomA, QStringLiteral("$1"), 1000),
            makeEvent(kRoomA, QStringLiteral("$2"), 2000),
        };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();

        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        QCOMPARE(rig.client.receiptEvents.first(), QStringLiteral("$2"));
        QCOMPARE(rig.client.receiptRooms.first(), kRoomA);
    }

    void noReceiptWhileWindowInactive()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.coordinator.setTimelineVisible(true);
        rig.coordinator.setNearBottom(true);
        QVERIFY(!rig.coordinator.receiptPending());
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 0);
    }

    void noReceiptWhileTimelineHidden()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.coordinator.setWindowActive(true);
        rig.coordinator.setNearBottom(true);
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 0);
    }

    void noReceiptWhileScrolledAway()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.coordinator.setWindowActive(true);
        rig.coordinator.setTimelineVisible(true);
        rig.coordinator.setNearBottom(false);
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 0);
    }

    void focusLossDuringDebounceCancels()
    {
        Rig rig(60);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QVERIFY(rig.coordinator.receiptPending());

        rig.coordinator.setWindowActive(false);
        QVERIFY(!rig.coordinator.receiptPending());
        QTest::qWait(100);
        QCOMPARE(rig.client.receiptEvents.size(), 0);
    }

    void scrollAwayDuringDebounceCancels()
    {
        Rig rig(60);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QVERIFY(rig.coordinator.receiptPending());

        rig.coordinator.setNearBottom(false);
        QTest::qWait(100);
        QCOMPARE(rig.client.receiptEvents.size(), 0);
    }

    void roomSwitchDuringDebounceNeverAcksOldRoom()
    {
        Rig rig(60);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$a"), 1000) };
        rig.client.timelines[kRoomB] = { makeEvent(kRoomB, QStringLiteral("$b"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QVERIFY(rig.coordinator.receiptPending());

        rig.model.setRoomId(kRoomB);
        QTRY_VERIFY(!rig.client.receiptEvents.isEmpty());
        QCOMPARE(rig.client.receiptEvents.first(), QStringLiteral("$b"));
        QCOMPARE(rig.client.receiptRooms.first(), kRoomB);
        QVERIFY(!rig.client.receiptEvents.contains(QStringLiteral("$a")));
    }

    void duplicateReceiptSuppressed()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);

        rig.coordinator.reevaluate();
        QVERIFY(!rig.coordinator.receiptPending());
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 1);
    }

    void newEventAfterReceiptIsAckedAgain()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);

        rig.client.appendEvent(makeEvent(kRoomA, QStringLiteral("$2"), 2000));
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$2"));
    }

    void aReactionAfterTheNewestRowGetsItsReceiptResent()
    {
        Rig rig;
        RoomInfo info;
        info.id = kRoomA;
        info.membership = RoomInfo::Joined;
        rig.client.roomInfos = { info };
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.accept(kRoomA, QStringLiteral("$1"));

        rig.client.roomInfos[0].unreadCount = 1;
        rig.coordinator.reevaluate();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$1"));
        rig.accept(kRoomA, QStringLiteral("$1"));

        rig.coordinator.reevaluate();
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 2);

        rig.client.roomInfos[0].unreadCount = 2;
        rig.coordinator.reevaluate();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 3);
    }

    void anUnreadFlagAfterTheNewestRowGetsItsReceiptResentOnce()
    {
        Rig rig;
        RoomInfo info;
        info.id = kRoomA;
        info.membership = RoomInfo::Joined;
        rig.client.roomInfos = { info };
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.accept(kRoomA, QStringLiteral("$1"));

        rig.client.roomInfos[0].hasUnreadMessages = true;
        Q_EMIT rig.client.roomsChanged();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$1"));
        rig.accept(kRoomA, QStringLiteral("$1"));

        Q_EMIT rig.client.roomsChanged();
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 2);
    }

    void aCountThatReturnsToTheSameValueIsResent()
    {
        Rig rig;
        RoomInfo info;
        info.id = kRoomA;
        info.membership = RoomInfo::Joined;
        info.unreadCount = 1;
        rig.client.roomInfos = { info };
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.accept(kRoomA, QStringLiteral("$1"));

        rig.client.roomInfos[0].unreadCount = 0;
        Q_EMIT rig.client.roomsChanged();
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 1);

        rig.client.roomInfos[0].unreadCount = 1;
        Q_EMIT rig.client.roomsChanged();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$1"));
    }

    void theRecordedCountBelongsToItsRoom()
    {
        Rig rig;
        RoomInfo a;
        a.id = kRoomA;
        a.membership = RoomInfo::Joined;
        a.unreadCount = 1;
        RoomInfo b = a;
        b.id = kRoomB;
        rig.client.roomInfos = { a, b };
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$a1"), 1000) };
        rig.client.timelines[kRoomB] = { makeEvent(kRoomB, QStringLiteral("$b1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.accept(kRoomA, QStringLiteral("$a1"));

        rig.model.setRoomId(kRoomB);
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$b1"));
        rig.accept(kRoomB, QStringLiteral("$b1"));

        rig.model.setRoomId(kRoomA);
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 2);

        rig.client.roomInfos[0].unreadCount = 2;
        Q_EMIT rig.client.roomsChanged();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 3);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$a1"));
    }

    void localEchoesFailedSendsAndVirtualRowsAreIneligible()
    {
        Rig rig;
        QList<TimelineEvent> events = {
            makeEvent(kRoomA, QStringLiteral("$remote"), 1000),
            makeVirtual(kRoomA),
            makeEvent(kRoomA, QStringLiteral("local:9"), 3000),
            makeEvent(kRoomA, QStringLiteral("$failed"), 4000, TimelineEvent::Failed),
        };
        TimelineEvent noId = makeEvent(kRoomA, QString(), 5000);
        events.append(noId);
        rig.client.timelines[kRoomA] = events;
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();

        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        QCOMPARE(rig.client.receiptEvents.first(), QStringLiteral("$remote"));
    }

    void signOutCancelsPendingReceipt()
    {
        Rig rig(60);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QVERIFY(rig.coordinator.receiptPending());

        rig.client.logout();
        QTest::qWait(100);
        QCOMPARE(rig.client.receiptEvents.size(), 0);
    }

    void receiptNeverRegressesToOlderEvent()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = {
            makeEvent(kRoomA, QStringLiteral("$old"), 1000),
            makeEvent(kRoomA, QStringLiteral("$new"), 2000),
        };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        QCOMPARE(rig.client.receiptEvents.first(), QStringLiteral("$new"));

        rig.client.resetTimeline(kRoomA,
                                 { makeEvent(kRoomA, QStringLiteral("$old"), 1000) });
        rig.coordinator.reevaluate();
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 1);
    }

    void aNewerEventWithAnEarlierTimestampStillGetsItsReceipt()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = {
            makeEvent(kRoomA, QStringLiteral("$a"), 5000),
        };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);

        rig.client.appendEvent(makeEvent(kRoomA, QStringLiteral("$b"), 3000));
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$b"));
    }

    void markedUnreadClearsOnOpenWithoutNewMessages()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.model.setRoomId(QString());
        RoomInfo room;
        room.id = kRoomA;
        room.markedUnread = true;
        rig.client.roomInfos = { room };
        Q_EMIT rig.client.roomsChanged();
        QVERIFY(!rig.coordinator.receiptPending());
        rig.model.setRoomId(kRoomA);
        QTRY_COMPARE(rig.client.markedReadRooms, QStringList{ kRoomA });
        QVERIFY(!rig.client.roomInfos.first().markedUnread);
        QCOMPARE(rig.client.receiptEvents.size(), 1);
        rig.coordinator.reevaluate();
        QVERIFY(!rig.coordinator.receiptPending());
    }

    void markingTheOpenRoomUnreadIsNotUndone()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        RoomInfo room;
        room.id = kRoomA;
        room.markedUnread = true;
        rig.client.roomInfos = { room };
        Q_EMIT rig.client.roomsChanged();
        rig.coordinator.reevaluate();
        QTest::qWait(400);
        QVERIFY(rig.client.markedReadRooms.isEmpty());
        QVERIFY(rig.client.roomInfos.first().markedUnread);
    }

    void markedUnreadFirstOpenUsesMarkReadAction_data()
    {
        QTest::addColumn<bool>("supported");
        QTest::newRow("SDK-mark-read") << true;
        QTest::newRow("fallback-receipt-and-flag") << false;
    }

    void markedUnreadFirstOpenUsesMarkReadAction()
    {
        QFETCH(bool, supported);
        Rig rig;
        rig.client.markReadSupported = supported;
        RoomInfo room;
        room.id = kRoomA;
        room.markedUnread = true;
        rig.client.roomInfos = { room };
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_VERIFY(!rig.client.roomInfos.first().markedUnread);
        QCOMPARE(rig.client.markedReadRooms, supported ? QStringList{ kRoomA } : QStringList{});
        QCOMPARE(rig.client.receiptEvents, supported ? QStringList{} : QStringList{ QStringLiteral("$1") });
    }

    void markedUnreadCloseCancels_data()
    {
        QTest::addColumn<int>("cancel");
        QTest::newRow("Escape-close") << 0;
        QTest::newRow("hidden") << 1;
        QTest::newRow("inactive") << 2;
        QTest::newRow("scroll-away") << 3;
    }

    void markedUnreadCloseCancels()
    {
        QFETCH(int, cancel);
        Rig rig(60);
        RoomInfo room;
        room.id = kRoomA;
        room.markedUnread = true;
        rig.client.roomInfos = { room };
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QVERIFY(rig.coordinator.receiptPending());
        if (cancel == 0) rig.model.setRoomId(QString());
        if (cancel == 1) rig.coordinator.setTimelineVisible(false);
        if (cancel == 2) rig.coordinator.setWindowActive(false);
        if (cancel == 3) rig.coordinator.setNearBottom(false);
        QTest::qWait(100);
        QVERIFY(rig.client.markedReadRooms.isEmpty());
        QVERIFY(rig.client.receiptEvents.isEmpty());
        QVERIFY(rig.client.roomInfos.first().markedUnread);
    }

    void receiptSentWaitsForTheServerToAccept()
    {
        Rig rig;
        QSignalSpy sent(&rig.coordinator, &ReadReceiptCoordinator::receiptSent);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        QTest::qWait(20);
        QCOMPARE(sent.size(), 0);

        rig.accept(kRoomA, QStringLiteral("$other"));
        rig.accept(kRoomB, QStringLiteral("$1"));
        QCOMPARE(sent.size(), 0);

        rig.accept(kRoomA, QStringLiteral("$1"));
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.at(0).at(0).toString(), kRoomA);
        QCOMPARE(sent.at(0).at(1).toString(), QStringLiteral("$1"));
        QCOMPARE(sent.at(0).at(2).toLongLong(), qint64(1000));
        rig.accept(kRoomA, QStringLiteral("$1"));
        QCOMPARE(sent.size(), 1);
    }

    void aFailedReceiptIsSentAgain()
    {
        Rig rig;
        rig.coordinator.setRetryBaseMs(5);
        QSignalSpy sent(&rig.coordinator, &ReadReceiptCoordinator::receiptSent);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);

        rig.fail(kRoomA, QStringLiteral("$1"));
        QVERIFY(rig.coordinator.retryPending());
        QCOMPARE(sent.size(), 0);
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$1"));

        rig.accept(kRoomA, QStringLiteral("$1"));
        QCOMPARE(sent.size(), 1);
        rig.coordinator.reevaluate();
        QTest::qWait(40);
        QCOMPARE(rig.client.receiptEvents.size(), 2);
    }

    void aFailedReceiptWaitsForItsBackoff()
    {
        Rig rig;
        rig.coordinator.setRetryBaseMs(10000);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.fail(kRoomA, QStringLiteral("$1"));
        for (int i = 0; i < 5; ++i)
            Q_EMIT rig.client.roomsChanged();
        QTest::qWait(40);
        QCOMPARE(rig.client.receiptEvents.size(), 1);
        QVERIFY(rig.coordinator.retryPending());
        rig.client.appendEvent(makeEvent(kRoomA, QStringLiteral("$2"), 2000));
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        QCOMPARE(rig.client.receiptEvents.last(), QStringLiteral("$2"));
    }

    void retriesAreBoundedAndResumeWhenTheReaderReturns()
    {
        Rig rig;
        rig.coordinator.setRetryBaseMs(1);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        const int attempts = 1 + ReadReceiptCoordinator::kMaxReceiptRetries;
        for (int n = 1; n <= attempts; ++n) {
            QTRY_COMPARE(rig.client.receiptEvents.size(), n);
            rig.fail(kRoomA, QStringLiteral("$1"));
        }
        QVERIFY(!rig.coordinator.retryPending());
        Q_EMIT rig.client.roomsChanged();
        rig.coordinator.reevaluate();
        QTest::qWait(80);
        QCOMPARE(rig.client.receiptEvents.size(), attempts);

        rig.coordinator.setWindowActive(false);
        rig.coordinator.setWindowActive(true);
        QTRY_COMPARE(rig.client.receiptEvents.size(), attempts + 1);
    }

    void markReadPathWaitsForTheServerAndRetries()
    {
        Rig rig;
        rig.coordinator.setRetryBaseMs(5);
        QSignalSpy sent(&rig.coordinator, &ReadReceiptCoordinator::receiptSent);
        RoomInfo room;
        room.id = kRoomA;
        room.markedUnread = true;
        rig.client.roomInfos = { room };
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.markedReadRooms.size(), 1);
        QCOMPARE(sent.size(), 0);

        rig.client.roomInfos.first().markedUnread = true;
        Q_EMIT rig.client.markRoomReadFailed(kRoomA);
        QTRY_COMPARE(rig.client.markedReadRooms.size(), 2);
        QCOMPARE(sent.size(), 0);

        Q_EMIT rig.client.readMarkerAdvanced(kRoomA);
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.at(0).at(1).toString(), QStringLiteral("$1"));
    }

    void answersAfterSignOutAnnounceNothing()
    {
        Rig rig;
        QSignalSpy sent(&rig.coordinator, &ReadReceiptCoordinator::receiptSent);
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.client.logout();
        rig.accept(kRoomA, QStringLiteral("$1"));
        rig.fail(kRoomA, QStringLiteral("$1"));
        QTest::qWait(20);
        QCOMPARE(sent.size(), 0);
        QVERIFY(!rig.coordinator.retryPending());
    }

    void receiptsDoNotMoveTheFullyReadMarker()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.accept(kRoomA, QStringLiteral("$1"));
        rig.client.appendEvent(makeEvent(kRoomA, QStringLiteral("$2"), 2000));
        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        rig.accept(kRoomA, QStringLiteral("$2"));
        QVERIFY(rig.client.fullyReadEvents.isEmpty());
    }

    void leavingTheRoomMovesTheFullyReadMarker()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$a"), 1000) };
        rig.client.timelines[kRoomB] = { makeEvent(kRoomB, QStringLiteral("$b"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);

        rig.model.setRoomId(kRoomB);
        QCOMPARE(rig.client.fullyReadRooms, QStringList{ kRoomA });
        QCOMPARE(rig.client.fullyReadEvents, QStringList{ QStringLiteral("$a") });

        QTRY_COMPARE(rig.client.receiptEvents.size(), 2);
        rig.model.setRoomId(kRoomA);
        QCOMPARE(rig.client.fullyReadRooms,
                 (QStringList{ kRoomA, kRoomB }));
        rig.model.setRoomId(QString());
        QCOMPARE(rig.client.fullyReadRooms.size(), 2);
    }

    void windowDeactivationMovesTheFullyReadMarker()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        QVERIFY(rig.client.fullyReadEvents.isEmpty());
        QSignalSpy moved(&rig.coordinator,
                         &ReadReceiptCoordinator::fullyReadMarkerMoved);
        rig.coordinator.setWindowActive(false);
        QCOMPARE(rig.client.fullyReadEvents, QStringList{ QStringLiteral("$1") });
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.at(0).at(0).toString(), kRoomA);
        QCOMPARE(moved.at(0).at(1).toString(), QStringLiteral("$1"));
        rig.coordinator.flushFullyReadMarker();
        QCOMPARE(rig.client.fullyReadEvents.size(), 1);
        QCOMPARE(moved.size(), 1);
    }

    void leavingUnreadRoomLeavesTheFullyReadMarker()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.coordinator.setWindowActive(true);
        rig.coordinator.setTimelineVisible(true);
        rig.coordinator.setNearBottom(false);
        QTest::qWait(20);
        rig.model.setRoomId(kRoomB);
        rig.coordinator.setWindowActive(false);
        QVERIFY(rig.client.fullyReadEvents.isEmpty());
    }

    void signOutForgetsWhereTheReaderWas()
    {
        Rig rig;
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.client.timelines[kRoomB] = { makeEvent(kRoomB, QStringLiteral("$b"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.client.logout();
        const int afterSignOut = int(rig.client.fullyReadEvents.size());
        rig.coordinator.setNearBottom(false);
        rig.model.setRoomId(kRoomA);
        rig.model.setRoomId(kRoomB);
        rig.coordinator.flushFullyReadMarker();
        QCOMPARE(int(rig.client.fullyReadEvents.size()), afterSignOut);
    }

    void anotherAccountNeverGetsTheFullyReadMarker()
    {
        Rig rig;
        Q_EMIT rig.client.loginSucceeded(QStringLiteral("@me:example.org"));
        rig.client.timelines[kRoomA] = { makeEvent(kRoomA, QStringLiteral("$1"), 1000) };
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTRY_COMPARE(rig.client.receiptEvents.size(), 1);
        rig.client.userId = QStringLiteral("@other:example.org");
        rig.coordinator.setNearBottom(false);
        rig.model.setRoomId(QString());
        QVERIFY(rig.client.fullyReadEvents.isEmpty());

        rig.client.userId = QStringLiteral("@me:example.org");
        rig.coordinator.setNearBottom(true);
        rig.model.setRoomId(kRoomA);
        QCOMPARE(rig.client.receiptEvents.size(), 1);
        rig.client.userId = QStringLiteral("@third:example.org");
        Q_EMIT rig.client.loginSucceeded(QStringLiteral("@third:example.org"));
        QSignalSpy sent(&rig.coordinator, &ReadReceiptCoordinator::receiptSent);
        rig.accept(kRoomA, QStringLiteral("$1"));
        QCOMPARE(sent.size(), 0);
        rig.coordinator.setWindowActive(false);
        QVERIFY(rig.client.fullyReadEvents.isEmpty());
    }

    void emptyTimelineSendsNothing()
    {
        Rig rig;
        rig.model.setRoomId(kRoomA);
        rig.makeEligible();
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 0);
    }
};

QTEST_GUILESS_MAIN(ReadReceiptCoordinatorTest)
#include "ReadReceiptCoordinatorTest.moc"
