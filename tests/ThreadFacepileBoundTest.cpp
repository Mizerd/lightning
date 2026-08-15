// v0.7.x: thread-participant fetches are BOUNDED. The room timeline is not
// virtualized, so every loaded thread root's summary card calls
// requestParticipants on the same frame; before this, opening a
// thread-heavy room dispatched one cache-first `/relations` chain per root
// at once. Pins:
//   * at most kMaxConcurrentParticipantFetches are ever in flight, and the
//     remainder wait rather than being dropped;
//   * an answer frees a slot and the queue advances;
//   * a FAILED (empty) answer frees its slot too — otherwise one failure
//     would permanently shrink the pool;
//   * requests are deduplicated against cached, in-flight AND queued roots,
//     so a card that becomes visible repeatedly costs nothing;
//   * a room switch discards QUEUED work for other rooms but leaves
//     in-flight work alone (its answer is keyed by room and cannot
//     contaminate the new room's cache);
//   * a late answer for the previous room populates only that room.
//
// HONEST SCOPE: the C++ bound and its bookkeeping. Real `/relations` cost on
// a live account is NOT exercised here and is NOT TESTED.

#include "matrix/MatrixClient.h"
#include "threads/ThreadManager.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    // Every (room, root) the manager actually dispatched, in order.
    QList<QPair<QString, QString>> dispatched;

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

    void requestThreadParticipants(const QString &roomId,
                                   const QString &rootEventId) override
    {
        dispatched.append({ roomId, rootEventId });
    }
};

QVariantList onePerson()
{
    QVariantMap p;
    p.insert(QStringLiteral("userId"), QStringLiteral("@bob:example.org"));
    p.insert(QStringLiteral("displayName"), QStringLiteral("Bob"));
    p.insert(QStringLiteral("avatarUrl"), QString());
    return QVariantList{ p };
}

const QString kRoom = QStringLiteral("!room:example.org");
const QString kOther = QStringLiteral("!other:example.org");

QString root(int i) { return QStringLiteral("$root%1:example.org").arg(i); }

} // namespace

class ThreadFacepileBoundTest : public QObject
{
    Q_OBJECT

private:
    // The bound the manager enforces. Kept as a local expectation rather
    // than reading the private constant, so a change to it is a deliberate
    // test update rather than a silently-passing tautology.
    static constexpr int kMaxInFlight = 4;

    static void answer(FakeClient &client, const QString &roomId,
                       const QString &rootEventId,
                       const QVariantList &participants)
    {
        Q_EMIT client.threadParticipantsReceived(roomId, rootEventId,
                                                 participants,
                                                 int(participants.size()),
                                                 false);
    }

private Q_SLOTS:
    void burstIsCappedAndTheRestQueue()
    {
        FakeClient client;
        ThreadManager mgr;
        mgr.setClient(&client);
        mgr.setActiveRoom(kRoom);

        // A thread-heavy room: 12 loaded roots all asking on one frame.
        for (int i = 0; i < 12; ++i)
            mgr.requestParticipants(kRoom, root(i));

        QCOMPARE(client.dispatched.size(), kMaxInFlight);
        QCOMPARE(mgr.participantFetchesInFlightForTest(), kMaxInFlight);
        QCOMPARE(mgr.participantFetchesQueuedForTest(), 12 - kMaxInFlight);
        // FIFO: the first roots to ask are the first to go out.
        QCOMPARE(client.dispatched.at(0).second, root(0));
        QCOMPARE(client.dispatched.at(kMaxInFlight - 1).second,
                 root(kMaxInFlight - 1));
    }

    void answersAdvanceTheQueue()
    {
        FakeClient client;
        ThreadManager mgr;
        mgr.setClient(&client);
        mgr.setActiveRoom(kRoom);
        for (int i = 0; i < 12; ++i)
            mgr.requestParticipants(kRoom, root(i));
        QCOMPARE(client.dispatched.size(), kMaxInFlight);

        answer(client, kRoom, root(0), onePerson());
        QCOMPARE(client.dispatched.size(), kMaxInFlight + 1);
        QCOMPARE(client.dispatched.last().second, root(kMaxInFlight));
        QCOMPARE(mgr.participantFetchesInFlightForTest(), kMaxInFlight);

        // Drain the rest: nothing is dropped, only paced.
        int guard = 0;
        while (mgr.participantFetchesQueuedForTest() > 0 && guard++ < 100) {
            const QString next = client.dispatched.last().second;
            answer(client, kRoom, next, onePerson());
        }
        QCOMPARE(mgr.participantFetchesQueuedForTest(), 0);
        QCOMPARE(client.dispatched.size(), 12);
    }

    void failedAnswerStillFreesItsSlot()
    {
        FakeClient client;
        ThreadManager mgr;
        mgr.setClient(&client);
        mgr.setActiveRoom(kRoom);
        for (int i = 0; i < 8; ++i)
            mgr.requestParticipants(kRoom, root(i));
        QCOMPARE(client.dispatched.size(), kMaxInFlight);

        // A failed lookup arrives EMPTY and is deliberately not cached. It
        // must still release the concurrency slot, or one failure per round
        // would shrink the pool until nothing could run.
        answer(client, kRoom, root(0), QVariantList{});
        QCOMPARE(client.dispatched.size(), kMaxInFlight + 1);
        QCOMPARE(mgr.participantFetchesInFlightForTest(), kMaxInFlight);
        QVERIFY(mgr.participants(kRoom, root(0)).isEmpty());

        // Not cached, so it is genuinely retryable — but retrying must
        // respect the cap, not slip past it. (This re-dispatch is also what
        // makes the per-dispatch generation matter: root(0) now has a
        // SECOND in-flight identity, and its first 60 s timeout timer is
        // still pending. That stale timer must not release this one's slot.)
        mgr.requestParticipants(kRoom, root(0));
        QCOMPARE(client.dispatched.size(), kMaxInFlight + 1);
        QCOMPARE(mgr.participantFetchesInFlightForTest(), kMaxInFlight);
    }

    void repeatedRequestsAreDeduplicated()
    {
        FakeClient client;
        ThreadManager mgr;
        mgr.setClient(&client);
        mgr.setActiveRoom(kRoom);

        // In flight: asking again costs nothing.
        mgr.requestParticipants(kRoom, root(0));
        mgr.requestParticipants(kRoom, root(0));
        mgr.requestParticipants(kRoom, root(0));
        QCOMPARE(client.dispatched.size(), 1);

        // Queued: also deduplicated, or a card scrolling in and out would
        // grow the queue without bound.
        for (int i = 1; i < 10; ++i)
            mgr.requestParticipants(kRoom, root(i));
        const int queued = mgr.participantFetchesQueuedForTest();
        for (int i = 1; i < 10; ++i)
            mgr.requestParticipants(kRoom, root(i));
        QCOMPARE(mgr.participantFetchesQueuedForTest(), queued);

        // Cached: also nothing.
        answer(client, kRoom, root(0), onePerson());
        const int after = client.dispatched.size();
        mgr.requestParticipants(kRoom, root(0));
        QCOMPARE(client.dispatched.size(), after);
    }

    void roomSwitchDiscardsQueuedWorkForTheOldRoom()
    {
        FakeClient client;
        ThreadManager mgr;
        mgr.setClient(&client);
        mgr.setActiveRoom(kRoom);
        for (int i = 0; i < 12; ++i)
            mgr.requestParticipants(kRoom, root(i));
        QCOMPARE(mgr.participantFetchesQueuedForTest(), 12 - kMaxInFlight);

        mgr.setActiveRoom(kOther);
        // Those cards are gone; their answers would only make the new
        // room's facepiles wait behind work nothing will read.
        QCOMPARE(mgr.participantFetchesQueuedForTest(), 0);
        // In-flight work is deliberately LEFT RUNNING: it is already paid
        // for and its answer is keyed by room.
        QCOMPARE(mgr.participantFetchesInFlightForTest(), kMaxInFlight);

        // The new room's own requests still queue behind those in-flight
        // slots and start as they free.
        const int before = client.dispatched.size();
        mgr.requestParticipants(kOther, root(100));
        QCOMPARE(client.dispatched.size(), before);
        answer(client, kRoom, root(0), onePerson());
        QCOMPARE(client.dispatched.size(), before + 1);
        QCOMPARE(client.dispatched.last().first, kOther);
    }

    void lateAnswerForThePreviousRoomStaysInThatRoom()
    {
        FakeClient client;
        ThreadManager mgr;
        mgr.setClient(&client);
        mgr.setActiveRoom(kRoom);
        mgr.requestParticipants(kRoom, root(0));
        mgr.setActiveRoom(kOther);

        // The old room's answer lands after the switch. The cache is keyed
        // by (room, root), so it can only ever populate its own room —
        // never the one now on screen.
        answer(client, kRoom, root(0), onePerson());
        QCOMPARE(mgr.participants(kRoom, root(0)).size(), 1);
        QVERIFY(mgr.participants(kOther, root(0)).isEmpty());
    }

    void signOutClearsCacheAndQueue()
    {
        FakeClient client;
        ThreadManager mgr;
        mgr.setClient(&client);
        mgr.setActiveRoom(kRoom);
        for (int i = 0; i < 12; ++i)
            mgr.requestParticipants(kRoom, root(i));
        answer(client, kRoom, root(0), onePerson());
        QCOMPARE(mgr.participants(kRoom, root(0)).size(), 1);

        Q_EMIT client.loggedOut();
        // One account's faces must never be shown under another's.
        QVERIFY(mgr.participants(kRoom, root(0)).isEmpty());
        QCOMPARE(mgr.participantFetchesQueuedForTest(), 0);
        QCOMPARE(mgr.participantFetchesInFlightForTest(), 0);
    }
};

QTEST_MAIN(ThreadFacepileBoundTest)
#include "ThreadFacepileBoundTest.moc"
