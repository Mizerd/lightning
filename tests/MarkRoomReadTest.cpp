
#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"

#include <QTimeZone>
#include <QtTest/QtTest>

namespace {

RoomInfo makeRoom(const QString &id)
{
    RoomInfo info;
    info.id = id;
    info.name = id;
    info.membership = RoomInfo::Joined;
    return info;
}

TimelineEvent makeEvent(const QString &eventId)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.type = TimelineEvent::TextMessage;
    return e;
}

class CapableClient : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    QList<RoomInfo> roomSet;
    QStringList markReadCalls;
    QStringList receiptCalls;

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
    QList<RoomInfo> rooms() const override { return roomSet; }
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
    void sendReadReceipt(const QString &roomId, const QString &eventId) override
    { receiptCalls.append(roomId + QLatin1Char('|') + eventId); }
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    bool supportsMarkRoomRead() const override { return true; }
    void markRoomRead(const QString &roomId) override
    { markReadCalls.append(roomId); }
};

class LegacyClient final : public CapableClient
{
    Q_OBJECT
public:
    using CapableClient::CapableClient;

    QHash<QString, QList<TimelineEvent>> timelines;

    QList<TimelineEvent> timeline(const QString &roomId) const override
    { return timelines.value(roomId); }

    bool supportsMarkRoomRead() const override { return false; }
};

const QString kRoom = QStringLiteral("!room:example.org");

}

class MarkRoomReadTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aFavouriteRanksAboveTheFeedUnderItsOwnHeader()
    {
        CapableClient client;
        RoomInfo busy = makeRoom(QStringLiteral("!busy:x"));
        busy.lastActivity =
            QDateTime(QDate(2026, 9, 5), QTime(12, 0), QTimeZone::UTC);
        RoomInfo quiet = makeRoom(QStringLiteral("!quiet:x"));
        quiet.lastActivity = busy.lastActivity.addSecs(-3600);
        quiet.isFavourite = true;
        RoomInfo pending = makeRoom(QStringLiteral("!invite:x"));
        pending.membership = RoomInfo::Invited;
        pending.isFavourite = true;
        client.roomSet = { busy, quiet, pending };
        RoomListModel model;
        model.setClient(&client);
        Q_EMIT client.roomsChanged();

        QCOMPARE(model.rowCount(), 3);
        auto idAt = [&](int row) {
            return model.data(model.index(row), RoomListModel::RoomIdRole).toString();
        };
        auto categoryAt = [&](int row) {
            return model.data(model.index(row), RoomListModel::CategoryRole).toString();
        };
        QCOMPARE(idAt(0), QStringLiteral("!invite:x"));
        QCOMPARE(categoryAt(0), QStringLiteral("invite"));
        QCOMPARE(idAt(1), QStringLiteral("!quiet:x"));
        QCOMPARE(categoryAt(1), QStringLiteral("favourite"));
        QCOMPARE(idAt(2), QStringLiteral("!busy:x"));
        QCOMPARE(categoryAt(2), QStringLiteral("conversation"));
        QCOMPARE(RoomListModel::orderRankOf(quiet), 1);
        QCOMPARE(RoomListModel::orderRankOf(busy), 2);
    }

    void aLowPriorityRoomSinksBelowTheFeedUnderItsOwnHeader()
    {
        CapableClient client;
        RoomInfo busy = makeRoom(QStringLiteral("!busy:x"));
        busy.lastActivity =
            QDateTime(QDate(2026, 9, 5), QTime(12, 0), QTimeZone::UTC);
        RoomInfo low = makeRoom(QStringLiteral("!low:x"));
        low.lastActivity = busy.lastActivity.addSecs(3600);
        low.lowPriority = true;
        RoomInfo both = makeRoom(QStringLiteral("!both:x"));
        both.lastActivity = busy.lastActivity.addSecs(-7200);
        both.lowPriority = true;
        both.isFavourite = true;
        client.roomSet = { low, busy, both };
        RoomListModel model;
        model.setClient(&client);
        Q_EMIT client.roomsChanged();

        QCOMPARE(model.rowCount(), 3);
        auto idAt = [&](int row) {
            return model.data(model.index(row), RoomListModel::RoomIdRole).toString();
        };
        auto categoryAt = [&](int row) {
            return model.data(model.index(row), RoomListModel::CategoryRole).toString();
        };
        QCOMPARE(idAt(0), QStringLiteral("!both:x"));
        QCOMPARE(categoryAt(0), QStringLiteral("favourite"));
        QCOMPARE(idAt(1), QStringLiteral("!busy:x"));
        QCOMPARE(categoryAt(1), QStringLiteral("conversation"));
        QCOMPARE(idAt(2), QStringLiteral("!low:x"));
        QCOMPARE(categoryAt(2), QStringLiteral("lowpriority"));
        QVERIFY(model.data(model.index(2), RoomListModel::IsLowPriorityRole).toBool());
        client.roomSet[0].lowPriority = false;
        Q_EMIT client.roomsChanged();
        QCOMPARE(idAt(1), QStringLiteral("!low:x"));
        QCOMPARE(categoryAt(1), QStringLiteral("conversation"));
        QVERIFY(!model.data(model.index(1), RoomListModel::IsLowPriorityRole).toBool());
    }

    void closedRoomIsMarkedReadThroughTheCapableBackend()
    {
        CapableClient client;
        client.roomSet = { makeRoom(kRoom) };
        RoomListModel model;
        model.setClient(&client);

        model.markRoomRead(kRoom);

        QCOMPARE(client.markReadCalls, QStringList{ kRoom });
        QVERIFY(client.receiptCalls.isEmpty());
    }

    void legacyBackendStillUsesTheLoadedTimeline()
    {
        LegacyClient client;
        client.roomSet = { makeRoom(kRoom) };
        client.timelines.insert(kRoom, { makeEvent(QStringLiteral("$a")),
                                         makeEvent(QStringLiteral("$b")) });
        RoomListModel model;
        model.setClient(&client);

        model.markRoomRead(kRoom);

        QVERIFY(client.markReadCalls.isEmpty());
        QCOMPARE(client.receiptCalls,
                 QStringList{ kRoom + QStringLiteral("|$b") });
    }

    void legacyBackendSkipsLocalEchoes()
    {
        LegacyClient client;
        client.roomSet = { makeRoom(kRoom) };
        client.timelines.insert(kRoom, { makeEvent(QStringLiteral("$real")),
                                         makeEvent(QStringLiteral("local:pending")) });
        RoomListModel model;
        model.setClient(&client);

        model.markRoomRead(kRoom);
        QCOMPARE(client.receiptCalls,
                 QStringList{ kRoom + QStringLiteral("|$real") });
    }

    void unknownTargetsAreInertNotFatal()
    {
        CapableClient client;
        client.roomSet = { makeRoom(kRoom) };
        RoomListModel model;
        model.setClient(&client);

        model.markRoomRead(QString());
        QVERIFY(client.markReadCalls.isEmpty());
        QVERIFY(client.receiptCalls.isEmpty());

        model.markRoomRead(QStringLiteral("!elsewhere:example.org"));
        QCOMPARE(client.markReadCalls,
                 QStringList{ QStringLiteral("!elsewhere:example.org") });

        RoomListModel detached;
        detached.markRoomRead(kRoom);
    }

    void markAllRoomsReadTouchesOnlyTheRoomsThatAreUnread()
    {
        CapableClient client;
        RoomInfo unreadCount = makeRoom(QStringLiteral("!a:example.org"));
        unreadCount.unreadCount = 3;
        RoomInfo highlighted = makeRoom(QStringLiteral("!b:example.org"));
        highlighted.highlightCount = 1;
        RoomInfo quietUnread = makeRoom(QStringLiteral("!c:example.org"));
        quietUnread.hasUnreadMessages = true;
        RoomInfo caughtUp = makeRoom(QStringLiteral("!d:example.org"));
        RoomInfo keptUnread = makeRoom(QStringLiteral("!e:example.org"));
        keptUnread.unreadCount = 2;
        keptUnread.markedUnread = true;
        RoomInfo invite = makeRoom(QStringLiteral("!f:example.org"));
        invite.membership = RoomInfo::Invited;
        invite.unreadCount = 1;
        RoomInfo space = makeRoom(QStringLiteral("!g:example.org"));
        space.isSpace = true;
        space.unreadCount = 5;

        client.roomSet = { unreadCount, highlighted, quietUnread, caughtUp,
                           keptUnread, invite, space };
        RoomListModel model;
        model.setClient(&client);

        QCOMPARE(model.markAllRoomsRead(), 3);
        QCOMPARE(client.markReadCalls,
                 (QStringList{ QStringLiteral("!a:example.org"),
                               QStringLiteral("!b:example.org"),
                               QStringLiteral("!c:example.org") }));

        client.markReadCalls.clear();
        QCOMPARE(model.markAllRoomsRead(), 3);
        QCOMPARE(client.markReadCalls.size(), 3);

        CapableClient quiet;
        quiet.roomSet = { caughtUp };
        RoomListModel quietModel;
        quietModel.setClient(&quiet);
        QCOMPARE(quietModel.markAllRoomsRead(), 0);
        QVERIFY(quiet.markReadCalls.isEmpty());

        RoomListModel detached;
        QCOMPARE(detached.markAllRoomsRead(), 0);
    }
};

QTEST_MAIN(MarkRoomReadTest)
#include "MarkRoomReadTest.moc"
