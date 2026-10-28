#include "app/RoomBatchController.h"
#include "app/RoomInfoController.h"
#include "models/RoomListModel.h"
#include "matrix/MatrixClient.h"
#include <QtTest/QtTest>

class BatchClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override {}
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
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
    void toggleReaction(const QString &, const QString &,
                        const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    QList<RoomInfo> rows;
    QStringList readRequests;
    QStringList favouriteRequests;
    QStringList lowPriorityRequests;
    QStringList leaveRequests;
    bool supportsMarkRoomRead() const override { return true; }
    bool supportsRoomFavourites() const override { return true; }
    QList<RoomInfo> rooms() const override { return rows; }
    void markRoomRead(const QString &id) override { readRequests.append(id); }
    void setRoomFavourite(const QString &id, bool) override { favouriteRequests.append(id); }
    void setRoomLowPriority(const QString &id, bool) override { lowPriorityRequests.append(id); }
    quint64 leaveRoom(const QString &id) override { leaveRequests.append(id); return leaveRequests.size(); }
};

class RoomBatchControllerTest : public QObject
{
    Q_OBJECT
private:
    static RoomInfo room(const QString &id) {
        RoomInfo r; r.id = id; r.name = id; r.membership = RoomInfo::Joined; return r;
    }
private Q_SLOTS:
    void refusedTagsFailImmediatelyForTheActiveRoomAndAction()
    {
        BatchClient client;
        client.rows = {room(QStringLiteral("!a:x")), room(QStringLiteral("!b:x"))};
        RoomListModel rooms; rooms.setClient(&client);
        RoomInfoController info; info.setClient(&client);
        RoomBatchController batch;
        batch.configure(&client, &rooms, &info, [](const QString &, int) {});
        batch.setAccount(QStringLiteral("@alice:x"));
        batch.select(QStringLiteral("!a:x"));
        batch.select(QStringLiteral("!b:x"));
        batch.run(QStringLiteral("favourite"), 1);
        Q_EMIT client.roomActionFailed(QStringLiteral("low_priority"), QStringLiteral("!a:x"));
        Q_EMIT client.roomActionFailed(QStringLiteral("favourite"), QStringLiteral("!b:x"));
        QCOMPARE(batch.pending(), 2);
        Q_EMIT client.roomActionFailed(QStringLiteral("favourite"), QStringLiteral("!a:x"));
        QCOMPARE(batch.failed(), 1);
        QTRY_COMPARE(client.favouriteRequests.size(), 2);
        Q_EMIT client.roomActionFailed(QStringLiteral("favourite"), QStringLiteral("!b:x"));
        QCOMPARE(batch.failed(), 2);
        batch.run(QStringLiteral("low_priority"), 1);
        Q_EMIT client.roomActionFailed(QStringLiteral("favourite"), QStringLiteral("!a:x"));
        QCOMPARE(batch.pending(), 2);
        Q_EMIT client.roomActionFailed(QStringLiteral("low_priority"), QStringLiteral("!a:x"));
        QCOMPARE(batch.failed(), 1);
        batch.setAccount(QStringLiteral("@bob:x"));
        Q_EMIT client.roomActionFailed(QStringLiteral("low_priority"), QStringLiteral("!b:x"));
        QCOMPARE(batch.pending(), 0);
    }

    void outcomesAreCorrelatedAndPartialFailureIsVisible()
    {
        BatchClient client;
        client.rows = {room(QStringLiteral("!a:x")), room(QStringLiteral("!b:x"))};
        RoomListModel rooms; rooms.setClient(&client);
        RoomInfoController info; info.setClient(&client);
        RoomBatchController batch;
        batch.configure(&client, &rooms, &info, [](const QString &, int) {});
        batch.setAccount(QStringLiteral("@alice:x"));
        batch.select(QStringLiteral("!a:x")); batch.select(QStringLiteral("!b:x"));
        batch.run(QStringLiteral("read"));
        QCOMPARE(client.readRequests, QStringList{QStringLiteral("!a:x")});
        QCOMPARE(batch.pending(), 2);
        emit client.markRoomReadFailed(QStringLiteral("!b:x")); // wrong room cannot settle A
        QCOMPARE(batch.pending(), 2);
        emit client.readMarkerAdvanced(QStringLiteral("!a:x"));
        QTRY_COMPARE(client.readRequests.size(), 2);
        QCOMPARE(batch.succeeded(), 1);
        emit client.markRoomReadFailed(QStringLiteral("!b:x"));
        QTRY_COMPARE(batch.failed(), 1);
        QCOMPARE(batch.pending(), 0);
        QCOMPARE(batch.outcomes().at(0).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("succeeded"));
        QCOMPARE(batch.outcomes().at(1).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    }

    void accountSwitchClearsOldResultsAndIgnoresLateAnswer()
    {
        BatchClient client; client.rows = {room(QStringLiteral("!a:x"))};
        RoomListModel rooms; rooms.setClient(&client);
        RoomInfoController info; info.setClient(&client);
        RoomBatchController batch;
        batch.configure(&client, &rooms, &info, [](const QString &, int) {});
        batch.setAccount(QStringLiteral("@alice:x")); batch.select(QStringLiteral("!a:x"));
        batch.run(QStringLiteral("read"));
        batch.setAccount(QStringLiteral("@bob:x"));
        emit client.readMarkerAdvanced(QStringLiteral("!a:x"));
        QCOMPARE(batch.selectionCount(), 0);
        QCOMPARE(batch.succeeded(), 0);
        QCOMPARE(batch.pending(), 0);
        QVERIFY(!batch.selecting());
    }

    void favouriteWaitsForSyncedStateAndLeaveNeedsConfirmation()
    {
        BatchClient client; client.rows = {room(QStringLiteral("!a:x"))};
        RoomListModel rooms; rooms.setClient(&client);
        RoomInfoController info; info.setClient(&client);
        RoomBatchController batch;
        batch.configure(&client, &rooms, &info, [](const QString &, int) {});
        batch.setAccount(QStringLiteral("@alice:x")); batch.select(QStringLiteral("!a:x"));
        batch.run(QStringLiteral("favourite"), 1);
        QCOMPARE(client.favouriteRequests.size(), 1);
        emit client.roomUpdated(QStringLiteral("!a:x"));
        QCOMPARE(batch.pending(), 1);
        client.rows[0].isFavourite = true;
        emit client.roomUpdated(QStringLiteral("!a:x"));
        QCOMPARE(batch.succeeded(), 1);
        batch.requestLeave();
        QVERIFY(batch.awaitingLeaveConfirmation());
        QVERIFY(client.leaveRequests.isEmpty());
        batch.cancelLeave();
        QVERIFY(client.leaveRequests.isEmpty());
        batch.requestLeave(); batch.confirmLeave();
        QCOMPARE(client.leaveRequests, QStringList{QStringLiteral("!a:x")});
        emit client.roomLeaveFinished(1, QStringLiteral("!a:x"), false, QStringLiteral("forbidden"));
        QCOMPARE(batch.failed(), 1);
    }

    void lowPriorityWaitsForTheRoomTag()
    {
        BatchClient client; client.rows = {room(QStringLiteral("!a:x"))};
        RoomListModel rooms; rooms.setClient(&client);
        RoomInfoController info; info.setClient(&client);
        RoomBatchController batch;
        batch.configure(&client, &rooms, &info, [](const QString &, int) {});
        batch.setAccount(QStringLiteral("@alice:x"));
        batch.select(QStringLiteral("!a:x"));
        batch.run(QStringLiteral("low_priority"), 1);
        QCOMPARE(client.lowPriorityRequests, QStringList{QStringLiteral("!a:x")});
        QCOMPARE(batch.pending(), 1);
        emit client.roomsChanged();
        QCOMPARE(batch.pending(), 1);
        client.rows[0].lowPriority = true;
        emit client.roomsChanged();
        QCOMPARE(batch.succeeded(), 1);
    }

    void leaveKeepsSnapshotNamesAndRemovesSuccessfulSelections()
    {
        BatchClient client;
        auto first = room(QStringLiteral("!a:x")); first.name = QStringLiteral("Alpha");
        auto second = room(QStringLiteral("!b:x")); second.name = QStringLiteral("Beta");
        client.rows = {first, second};
        RoomListModel rooms; rooms.setClient(&client);
        RoomInfoController info; info.setClient(&client);
        RoomBatchController batch;
        batch.configure(&client, &rooms, &info, [](const QString &, int) {});
        batch.setAccount(QStringLiteral("@alice:x"));
        batch.select(first.id); batch.select(second.id);
        batch.requestLeave(); batch.confirmLeave();
        client.rows.removeFirst();
        emit client.roomsChanged();
        emit client.roomLeaveFinished(1, first.id, true, {});
        QCOMPARE(batch.selectionCount(), 1);
        QVERIFY(!batch.isSelected(first.id));
        QCOMPARE(batch.outcomes().first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Alpha"));
        QTRY_COMPARE(client.leaveRequests.size(), 2);
        client.rows.clear();
        emit client.roomsChanged();
        emit client.roomLeaveFinished(2, second.id, true, {});
        QCOMPARE(batch.selectionCount(), 0);
        QVERIFY(!batch.selecting());
        QCOMPARE(batch.outcomes().last().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Beta"));
    }
};
QTEST_MAIN(RoomBatchControllerTest)
#include "RoomBatchControllerTest.moc"
