// v0.7.x pinned messages (`m.room.pinned_events`) — PinnedMessagesController
// policy. Pins:
//   * snapshot ingestion (entries / ids / total / truncated / canPin) and the
//     complete-id-list contract that answers isPinned();
//   * a FAILED read keeps the last known list rather than erasing it;
//   * canTogglePin offers exactly the action that applies, and nothing at all
//     without the room's real pin permission or while a write is in flight;
//   * a pin/unpin is never applied optimistically — the authoritative list is
//     re-read after the write, on success AND on failure;
//   * a remote pin change re-reads rather than trusting a pushed payload;
//   * room switch and sign-out drop the list, and answers for the previous
//     room can never repaint the current one;
//   * the /state fallback probe is spent once per room, not once per refresh.
//
// HONEST SCOPE: policy and wiring only. Real `m.room.pinned_events` round
// trips, out-of-window event resolution against a homeserver, and Element
// interoperability are NOT exercised here and are NOT TESTED.

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

    // MatrixClient pure virtuals (inert).
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

} // namespace

class PinnedMessagesTest : public QObject
{
    Q_OBJECT

private:
    // Bring a controller up on `room` with one pinned event ($a).
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
        // total (5) deliberately exceeds the resolved entries (1): the bridge
        // caps RESOLUTION, and the UI must be able to say so.
        Q_EMIT client.pinnedReceived(
            client.lastFetchOp, kRoom,
            snapshot(true, { kEventA, kEventB }, { entry(kEventA) },
                     /*total=*/5, /*truncated=*/true));

        QVERIFY(!changed.isEmpty());
        QCOMPARE(ctl.entries().size(), 1);
        QCOMPARE(ctl.total(), 5);
        QVERIFY(ctl.truncated());
        QVERIFY(ctl.canPin());
        QVERIFY(!ctl.loading());
        // isPinned answers from the COMPLETE id list, not from the capped
        // entries — an unresolved pin is still pinned.
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
            snapshot(true, {}, {}, 0, false, /*ok=*/false));

        // Erasing here would make a flaky connection look like "nothing is
        // pinned any more".
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

        // $a is pinned: unpin applies, pin does not.
        QVERIFY(!ctl.canTogglePin(kEventA, true));
        QVERIFY(ctl.canTogglePin(kEventA, false));
        // $b is not pinned: the mirror image.
        QVERIFY(ctl.canTogglePin(kEventB, true));
        QVERIFY(!ctl.canTogglePin(kEventB, false));
        // No event id is never an action.
        QVERIFY(!ctl.canTogglePin(QString(), true));
    }

    void withoutPermissionNothingIsOfferedOrSent()
    {
        FakeClient client;
        PinnedMessagesController ctl;
        ctl.setClient(&client);
        seed(ctl, client, /*canPin=*/false);

        QVERIFY(!ctl.canPin());
        QVERIFY(!ctl.canTogglePin(kEventB, true));
        QVERIFY(!ctl.canTogglePin(kEventA, false));

        // And the dispatch re-checks: a state event known to be
        // unauthorized must never leave the client.
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
        // No snapshot yet: canPin is false, so the menu offers nothing —
        // it must fail closed rather than optimistically offering Pin.
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
        // The list has NOT changed yet: nothing is applied before the room
        // state says so.
        QVERIFY(!ctl.isPinned(kEventB));
        // And while a write is in flight no second action is offered.
        QVERIFY(!ctl.canTogglePin(kEventA, false));

        Q_EMIT client.pinChangeFinished(client.lastWriteOp, kRoom, kEventB,
                                        true, true, true, QString());
        QVERIFY(!ctl.pending());
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(3).toBool());
        QCOMPARE(ctl.error(), QString());
        // Authoritative re-read, rather than a local mutation.
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
        // Never leaves the UI showing a state the room does not have.
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

        // Another client changed the pins. Only the room id crosses; the
        // controller re-reads rather than trusting a pushed payload.
        Q_EMIT client.pinnedEventsChanged(kRoom);
        QCOMPARE(client.fetchCalls, before + 1);

        // A poke for a DIFFERENT room is not this room's business.
        const int after = client.fetchCalls;
        Q_EMIT client.pinnedEventsChanged(kOther);
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

        // The previous room's answer arrives late. It must not repaint the
        // current room — matched on BOTH op id and room.
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

        // Every later refresh in the same room reads what sync delivered;
        // without this a room with no pins would spend one 404-able GET on
        // every poke.
        ctl.refresh();
        QCOMPARE(client.fetchAllowRemote.size(), 2);
        QVERIFY(!client.fetchAllowRemote.at(1));

        // A different room gets its own single probe.
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

        // A second read would only race the first to the same answer.
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

        // A read is in flight (say, a sync poke landed)...
        Q_EMIT client.pinnedEventsChanged(kRoom);
        QCOMPARE(client.fetchCalls, 2);
        const quint64 inFlight = client.lastFetchOp;

        // ...and while it runs, a write completes. Its refresh is answering
        // a NEWER question than the in-flight read can, so it must not be
        // dropped just because a read happens to be running.
        ctl.pin(kEventB);
        Q_EMIT client.pinChangeFinished(client.lastWriteOp, kRoom, kEventB,
                                        true, true, true, QString());
        QCOMPARE(client.fetchCalls, 2); // still coalesced behind the in-flight one

        // When the in-flight read lands, the owed refresh goes out.
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

        // The in-flight read FAILS. The owed refresh must still go out —
        // a failed answer is the least reason to stop asking.
        Q_EMIT client.pinnedReceived(
            inFlight, kRoom, snapshot(true, {}, {}, 0, false, /*ok=*/false));
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
