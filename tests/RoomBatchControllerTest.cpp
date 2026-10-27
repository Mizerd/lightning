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
    QList<RoomInfo> rows;
    QStringList readRequests;
    QStringList favouriteRequests;
    QStringList leaveRequests;
    bool supportsMarkRoomRead() const override { return true; }
    bool supportsRoomFavourites() const override { return true; }
    QList<RoomInfo> rooms() const override { return rows; }
    void markRoomRead(const QString &id) override { readRequests.append(id); }
    void setRoomFavourite(const QString &id, bool) override { favouriteRequests.append(id); }
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
};
QTEST_MAIN(RoomBatchControllerTest)
#include "RoomBatchControllerTest.moc"
