
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

    QList<QPair<QString, QString>> dispatched;

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

}

class ThreadFacepileBoundTest : public QObject
{
    Q_OBJECT

private:
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

        for (int i = 0; i < 12; ++i)
            mgr.requestParticipants(kRoom, root(i));

        QCOMPARE(client.dispatched.size(), kMaxInFlight);
        QCOMPARE(mgr.participantFetchesInFlightForTest(), kMaxInFlight);
        QCOMPARE(mgr.participantFetchesQueuedForTest(), 12 - kMaxInFlight);
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

        answer(client, kRoom, root(0), QVariantList{});
        QCOMPARE(client.dispatched.size(), kMaxInFlight + 1);
        QCOMPARE(mgr.participantFetchesInFlightForTest(), kMaxInFlight);
        QVERIFY(mgr.participants(kRoom, root(0)).isEmpty());

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

        mgr.requestParticipants(kRoom, root(0));
        mgr.requestParticipants(kRoom, root(0));
        mgr.requestParticipants(kRoom, root(0));
        QCOMPARE(client.dispatched.size(), 1);

        for (int i = 1; i < 10; ++i)
            mgr.requestParticipants(kRoom, root(i));
        const int queued = mgr.participantFetchesQueuedForTest();
        for (int i = 1; i < 10; ++i)
            mgr.requestParticipants(kRoom, root(i));
        QCOMPARE(mgr.participantFetchesQueuedForTest(), queued);

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
        QCOMPARE(mgr.participantFetchesQueuedForTest(), 0);
        QCOMPARE(mgr.participantFetchesInFlightForTest(), kMaxInFlight);

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
        QVERIFY(mgr.participants(kRoom, root(0)).isEmpty());
        QCOMPARE(mgr.participantFetchesQueuedForTest(), 0);
        QCOMPARE(mgr.participantFetchesInFlightForTest(), 0);
    }
};

QTEST_MAIN(ThreadFacepileBoundTest)
#include "ThreadFacepileBoundTest.moc"
