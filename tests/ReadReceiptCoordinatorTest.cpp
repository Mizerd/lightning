// v0.5.11: deterministic tests for the automatic read-receipt coordinator —
// receipts only under the full visibility conjunction (window active,
// timeline visible, near bottom, room open), debounce cancellation on focus
// loss / scroll-away / room switch / sign-out, local-echo and failed-send
// ineligibility, duplicate suppression and never-regress ordering.

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

    // Remaining pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
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
};

} // namespace

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
        // windowActive stays false.
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
        // Only the new room's newest event may ever be acked.
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

        // The timeline resets to a state whose newest readable event is
        // OLDER than the already-acked one (e.g. truncated snapshot).
        rig.client.resetTimeline(kRoomA,
                                 { makeEvent(kRoomA, QStringLiteral("$old"), 1000) });
        rig.coordinator.reevaluate();
        QTest::qWait(20);
        QCOMPARE(rig.client.receiptEvents.size(), 1);
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
