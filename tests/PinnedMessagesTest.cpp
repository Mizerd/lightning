
#include "app/PinnedMessagesController.h"
#include "matrix/MatrixClient.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    bool pinnedSupported = true;
    bool refuseWrites = false;
    int fetchCalls = 0;
    int writeCalls = 0;
    QList<bool> fetchAllowRemote;
    QString lastWriteRoom;
    QString lastWriteEvent;
    bool lastWritePin = false;
    quint64 lastFetchOp = 0;
    quint64 lastWriteOp = 0;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    bool supportsPinnedMessages() const override { return pinnedSupported; }
    quint64 requestPinnedMessages(const QString &, bool allowRemote) override
    {
        if (!pinnedSupported)
            return 0;
        ++fetchCalls;
        fetchAllowRemote.append(allowRemote);
        lastFetchOp = nextOp++;
        return lastFetchOp;
    }
    quint64 setEventPinned(const QString &roomId, const QString &eventId,
                           bool pin) override
    {
        if (refuseWrites)
            return 0;
        ++writeCalls;
        lastWriteRoom = roomId;
        lastWriteEvent = eventId;
        lastWritePin = pin;
        lastWriteOp = nextOp++;
        return lastWriteOp;
    }
};

QVariantMap entry(const QString &eventId, bool available = true,
                  const QString &kind = QStringLiteral("text"))
{
    QVariantMap e;
    e.insert(QStringLiteral("eventId"), eventId);
    e.insert(QStringLiteral("available"), available);
    if (available) {
        e.insert(QStringLiteral("sender"), QStringLiteral("@bob:example.org"));
        e.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Bob"));
        e.insert(QStringLiteral("senderAvatarUrl"), QString());
        e.insert(QStringLiteral("timestampMs"), qlonglong(1700000000000LL));
        e.insert(QStringLiteral("kind"), kind);
        e.insert(QStringLiteral("preview"), QStringLiteral("pinned note"));
    }
    return e;
}

QVariantMap snapshot(bool canPin, const QStringList &ids,
                     const QVariantList &entries, int total = -1,
                     bool truncated = false, bool ok = true)
{
    QVariantMap s;
    s.insert(QStringLiteral("ok"), ok);
    s.insert(QStringLiteral("canPin"), canPin);
    s.insert(QStringLiteral("total"),
             total >= 0 ? total : int(ids.size()));
    s.insert(QStringLiteral("truncated"), truncated);
    s.insert(QStringLiteral("ids"), ids);
    s.insert(QStringLiteral("entries"), entries);
    s.insert(QStringLiteral("category"), QString());
    return s;
}

const QString kRoom = QStringLiteral("!room:example.org");
const QString kOther = QStringLiteral("!other:example.org");
const QString kEventA = QStringLiteral("$a:example.org");
const QString kEventB = QStringLiteral("$b:example.org");

}

class PinnedMessagesTest : public QObject
{
    Q_OBJECT

private:
    static void seed(PinnedMessagesController &ctl, FakeClient &client,
                     bool canPin = true)
    {
        ctl.setRoomId(kRoom);
        QVERIFY(client.fetchCalls >= 1);
        Q_EMIT client.pinnedReceived(
            client.lastFetchOp, kRoom,
            snapshot(canPin, { kEventA }, { entry(kEventA) }));
    }

private Q_SLOTS:
    void snapshotIngestionExposesEntriesAndIds()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);

        QSignalSpy changed(&ctl, &PinnedMessagesController::stateChanged);
        Q_EMIT client.pinnedReceived(
            client.lastFetchOp, kRoom,
            snapshot(true, { kEventA, kEventB }, { entry(kEventA) },
5,true));

        QVERIFY(!changed.isEmpty());
        QCOMPARE(ctl.entries().size(), 1);
        QCOMPARE(ctl.total(), 5);
        QVERIFY(ctl.truncated());
        QVERIFY(ctl.canPin());
        QVERIFY(!ctl.loading());
        QVERIFY(ctl.isPinned(kEventA));
        QVERIFY(ctl.isPinned(kEventB));
        QVERIFY(!ctl.isPinned(QStringLiteral("$nope:example.org")));
        QVERIFY(!ctl.isPinned(QString()));
    }

    void failedReadKeepsTheLastKnownList()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QCOMPARE(ctl.entries().size(), 1);

        ctl.refresh();
        Q_EMIT client.pinnedReceived(
            client.lastFetchOp, kRoom,
            snapshot(true, {}, {}, 0, false,false));

        QCOMPARE(ctl.entries().size(), 1);
        QVERIFY(ctl.isPinned(kEventA));
        QVERIFY(!ctl.loading());
    }

    void togglePinOffersOnlyTheActionThatApplies()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        QVERIFY(!ctl.canTogglePin(kEventA, true));
        QVERIFY(ctl.canTogglePin(kEventA, false));
        QVERIFY(ctl.canTogglePin(kEventB, true));
        QVERIFY(!ctl.canTogglePin(kEventB, false));
        QVERIFY(!ctl.canTogglePin(QString(), true));
    }

    void withoutPermissionNothingIsOfferedOrSent()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client,false);

        QVERIFY(!ctl.canPin());
        QVERIFY(!ctl.canTogglePin(kEventB, true));
        QVERIFY(!ctl.canTogglePin(kEventA, false));

        ctl.pin(kEventB);
        ctl.unpin(kEventA);
        QCOMPARE(client.writeCalls, 0);
    }

    void beforeTheFirstSnapshotNothingIsOffered()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);
        QVERIFY(!ctl.canPin());
        QVERIFY(!ctl.canTogglePin(kEventB, true));
        ctl.pin(kEventB);
        QCOMPARE(client.writeCalls, 0);
    }

    void writeIsNotOptimisticAndRefetchesOnSuccess()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        const int fetchesBefore = client.fetchCalls;

        QSignalSpy finished(&ctl,
                            &PinnedMessagesController::pinActionFinished);
        ctl.pin(kEventB);
        QCOMPARE(client.writeCalls, 1);
        QCOMPARE(client.lastWriteEvent, kEventB);
        QVERIFY(client.lastWritePin);
        QVERIFY(ctl.pending());
        QVERIFY(!ctl.isPinned(kEventB));
        QVERIFY(!ctl.canTogglePin(kEventA, false));

        Q_EMIT client.pinChangeFinished(client.lastWriteOp, kRoom, kEventB,
                                        true, true, true, QString());
        QVERIFY(!ctl.pending());
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(3).toBool());
        QCOMPARE(ctl.error(), QString());
        QCOMPARE(client.fetchCalls, fetchesBefore + 1);
    }

    void rejectedWriteReportsAndStillRefetches()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        const int fetchesBefore = client.fetchCalls;

        QSignalSpy finished(&ctl,
                            &PinnedMessagesController::pinActionFinished);
        ctl.pin(kEventB);
        Q_EMIT client.pinChangeFinished(client.lastWriteOp, kRoom, kEventB,
                                        true, false, false,
                                        QStringLiteral("forbidden"));

        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(3).toBool());
        QVERIFY(!ctl.error().isEmpty());
        QVERIFY(!ctl.isPinned(kEventB));
        QCOMPARE(client.fetchCalls, fetchesBefore + 1);
    }

    void synchronousRefusalIsReportedNotSwallowed()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        client.refuseWrites = true;
        QSignalSpy finished(&ctl,
                            &PinnedMessagesController::pinActionFinished);
        ctl.pin(kEventB);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(3).toBool());
        QVERIFY(!ctl.pending());
        QVERIFY(!ctl.error().isEmpty());
    }

    void remotePinChangeRefetches()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        const int before = client.fetchCalls;

        Q_EMIT client.pinnedEventsChanged(kRoom);
        QCOMPARE(client.fetchCalls, before + 1);

        const int after = client.fetchCalls;
        Q_EMIT client.pinnedEventsChanged(kOther);
        QCOMPARE(client.fetchCalls, after);
    }

    void memberPokeReReadsCanPinForTheCurrentRoomOnly()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);
        Q_EMIT client.pinnedReceived(client.lastFetchOp, kRoom,
                                     snapshot(false, {}, {}));
        QVERIFY(!ctl.canPin());
        const int before = client.fetchCalls;

        Q_EMIT client.roomMemberEventSeen(kRoom);
        Q_EMIT client.roomMemberEventSeen(kRoom);
        Q_EMIT client.roomMemberEventSeen(kRoom);
        QTRY_COMPARE_WITH_TIMEOUT(client.fetchCalls, before + 1, 3000);
        QVERIFY(!client.fetchAllowRemote.last());
        Q_EMIT client.pinnedReceived(client.lastFetchOp, kRoom,
                                     snapshot(true, {}, {}));
        QVERIFY(ctl.canPin());
        QVERIFY(ctl.canTogglePin(kEventA, true));

        const int after = client.fetchCalls;
        Q_EMIT client.roomMemberEventSeen(kOther);
        QTest::qWait(700);
        QCOMPARE(client.fetchCalls, after);
    }

    void roomSwitchDropsTheListAndRejectsTheOldRoomsAnswer()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);
        const quint64 staleOp = client.lastFetchOp;

        ctl.setRoomId(kOther);
        QCOMPARE(ctl.entries().size(), 0);
        QVERIFY(!ctl.canPin());

        Q_EMIT client.pinnedReceived(
            staleOp, kRoom, snapshot(true, { kEventA }, { entry(kEventA) }));
        QCOMPARE(ctl.entries().size(), 0);
        QVERIFY(!ctl.isPinned(kEventA));
    }

    void signOutClearsEverything()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QVERIFY(ctl.isPinned(kEventA));

        Q_EMIT client.loggedOut();
        QCOMPARE(ctl.entries().size(), 0);
        QCOMPARE(ctl.total(), 0);
        QVERIFY(!ctl.canPin());
        QVERIFY(!ctl.isPinned(kEventA));
    }

    void remoteStateProbeIsSpentOncePerRoom()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);

        ctl.setRoomId(kRoom);
        QCOMPARE(client.fetchAllowRemote.size(), 1);
        QVERIFY2(client.fetchAllowRemote.at(0),
                 "the first fetch for a room may take the /state fallback");
        Q_EMIT client.pinnedReceived(client.lastFetchOp, kRoom,
                                     snapshot(true, {}, {}));

        ctl.refresh();
        QCOMPARE(client.fetchAllowRemote.size(), 2);
        QVERIFY(!client.fetchAllowRemote.at(1));

        ctl.setRoomId(kOther);
        QCOMPARE(client.fetchAllowRemote.size(), 3);
        QVERIFY(client.fetchAllowRemote.at(2));
    }

    void oneReadAtATime()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);
        QCOMPARE(client.fetchCalls, 1);

        ctl.refresh();
        ctl.refresh();
        QCOMPARE(client.fetchCalls, 1);

        Q_EMIT client.pinnedReceived(client.lastFetchOp, kRoom,
                                     snapshot(true, {}, {}));
        ctl.refresh();
        QCOMPARE(client.fetchCalls, 2);
    }

    void aRefreshDueDuringAnInFlightReadIsNotLost()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        Q_EMIT client.pinnedEventsChanged(kRoom);
        QCOMPARE(client.fetchCalls, 2);
        const quint64 inFlight = client.lastFetchOp;

        ctl.pin(kEventB);
        Q_EMIT client.pinChangeFinished(client.lastWriteOp, kRoom, kEventB,
                                        true, true, true, QString());
        QCOMPARE(client.fetchCalls, 2);

        Q_EMIT client.pinnedReceived(inFlight, kRoom,
                                     snapshot(true, { kEventA },
                                              { entry(kEventA) }));
        QCOMPARE(client.fetchCalls, 3);
    }

    void anOwedRefreshSurvivesAFailedRead()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        Q_EMIT client.pinnedEventsChanged(kRoom);
        const quint64 inFlight = client.lastFetchOp;
        ctl.pin(kEventB);
        Q_EMIT client.pinChangeFinished(client.lastWriteOp, kRoom, kEventB,
                                        true, true, true, QString());
        QCOMPARE(client.fetchCalls, 2);

        Q_EMIT client.pinnedReceived(
            inFlight, kRoom, snapshot(true, {}, {}, 0, false,false));
        QCOMPARE(client.fetchCalls, 3);
    }

    void unsupportedBackendOffersNothing()
    {
        FakeClient client;
        client.pinnedSupported = false;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);

        QVERIFY(!ctl.supported());
        QCOMPARE(client.fetchCalls, 0);
        QVERIFY(!ctl.canTogglePin(kEventB, true));
        ctl.pin(kEventB);
        QCOMPARE(client.writeCalls, 0);
    }
};

QTEST_MAIN(PinnedMessagesTest)
#include "PinnedMessagesTest.moc"
