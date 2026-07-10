#include "matrix/MatrixClient.h"
#include "models/RoomListModel.h"
#include "spaces/SpaceManager.h"

#include <QtTest/QtTest>

namespace {

RoomInfo room(const QString &id, bool direct = false, int members = 0)
{
    RoomInfo value;
    value.id = id;
    value.name = id;
    value.isDirect = direct;
    value.lastActivity = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < members; ++i) {
        MemberInfo member;
        member.userId = QStringLiteral("@u%1:example.org").arg(i);
        value.members.insert(member.userId, member);
    }
    return value;
}

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    QList<RoomInfo> mirror;
    QString accepted;
    QString rejected;
    QString marked;

    using MatrixClient::MatrixClient;
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return mirror; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
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
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
    void acceptInvite(const QString &id) override { accepted = id; }
    void rejectInvite(const QString &id) override { rejected = id; }
    void setRoomMarkedUnread(const QString &id, bool unread) override
    {
        if (unread) marked = id;
    }
};

} // namespace

class RoomStateModelTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void directClassificationUsesMDirectOnly();
    void liveDirectUpdateChangesCategory();
    void directMappingRemovalReturnsToRoom();
    void inviteActionsRouteAndStaySeparate();
    void explicitDiffOperationsValidateIndexesAndIdentity();
    void replaceValidatesIdentityAndReset();
    void nestedSpacesAreCycleSafeAndSupportMultipleParents();
    void homeAggregatesSharedRoomWithoutDoubleCounting();
    void selectedSpaceDisappearingReturnsHome();
    void searchFiltersNameAndAliasAndFindsInvites();
};

void RoomStateModelTest::directClassificationUsesMDirectOnly()
{
    FakeClient client;
    RoomListModel model;
    client.mirror = { room(QStringLiteral("!two:example.org"), false, 2),
                      room(QStringLiteral("!large:example.org"), true, 8) };
    model.setClient(&client);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.data(model.index(0), RoomListModel::RoomIdRole).toString(),
             QStringLiteral("!large:example.org"));
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("dm"));
    QCOMPARE(model.data(model.index(1), RoomListModel::CategoryRole).toString(),
             QStringLiteral("room"));
}

void RoomStateModelTest::liveDirectUpdateChangesCategory()
{
    FakeClient client;
    RoomListModel model;
    client.mirror = { room(QStringLiteral("!room:example.org"), false, 2) };
    model.setClient(&client);
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("room"));
    client.mirror[0].isDirect = true;
    Q_EMIT client.roomUpdated(client.mirror[0].id);
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("dm"));
}

void RoomStateModelTest::inviteActionsRouteAndStaySeparate()
{
    FakeClient client;
    RoomListModel model;
    auto invite = room(QStringLiteral("!invite:example.org"));
    invite.membership = RoomInfo::Invited;
    client.mirror = { invite };
    model.setClient(&client);
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("invite"));
    model.acceptInvite(invite.id);
    QCOMPARE(client.accepted, invite.id);
    model.rejectInvite(invite.id);
    QCOMPARE(client.rejected, invite.id);
    model.markRoomUnread(invite.id);
    QCOMPARE(client.marked, invite.id);
}

void RoomStateModelTest::explicitDiffOperationsValidateIndexesAndIdentity()
{
    RoomListModel model;
    const auto a = room(QStringLiteral("!a:example.org"));
    const auto b = room(QStringLiteral("!b:example.org"));
    QVERIFY(model.appendRooms({a}));
    QVERIFY(model.insertRoom(1, b));
    QVERIFY(!model.insertRoom(2, b));
    QVERIFY(!model.removeRoom(3));
    QVERIFY(!model.truncate(3));
    QVERIFY(model.removeRoom(0));
    QCOMPARE(model.rowCount(), 1);
    QVERIFY(model.truncate(0));
    QCOMPARE(model.rowCount(), 0);
}

void RoomStateModelTest::directMappingRemovalReturnsToRoom()
{
    // A room dropped from m.direct returns to the ROOMS group; member count
    // is never consulted for classification.
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), true, 5);
    dm.directUserId = QStringLiteral("@bob:example.org");
    client.mirror = { dm };
    model.setClient(&client);
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("dm"));
    client.mirror[0].isDirect = false;
    client.mirror[0].directUserId.clear();
    Q_EMIT client.roomsChanged();
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("room"));
}

void RoomStateModelTest::replaceValidatesIdentityAndReset()
{
    RoomListModel model;
    const auto a = room(QStringLiteral("!a:example.org"));
    const auto b = room(QStringLiteral("!b:example.org"));
    model.resetRooms({a, b});
    QCOMPARE(model.rowCount(), 2);
    // In-place replace requires the same id at that index.
    auto a2 = a; a2.name = QStringLiteral("renamed");
    QVERIFY(model.replaceRoom(0, a2));
    QCOMPARE(model.data(model.index(0), RoomListModel::NameRole).toString(),
             QStringLiteral("renamed"));
    // Replacing with a different id at that index is rejected (no silent
    // identity swap that could duplicate another row).
    QVERIFY(!model.replaceRoom(0, b));
    QVERIFY(!model.replaceRoom(5, a2));
    // removeRange / truncate bounds.
    QVERIFY(!model.removeRange(1, 5));
    QVERIFY(model.truncate(1));
    QCOMPARE(model.rowCount(), 1);
}

void RoomStateModelTest::nestedSpacesAreCycleSafeAndSupportMultipleParents()
{
    FakeClient client;
    SpaceManager spaces;
    auto a = room(QStringLiteral("!a:example.org")); a.isSpace = true;
    auto b = room(QStringLiteral("!b:example.org")); b.isSpace = true;
    auto c = room(QStringLiteral("!c:example.org")); c.isSpace = true;
    auto leaf = room(QStringLiteral("!leaf:example.org")); leaf.unreadCount = 4;
    a.childRoomIds = {b.id, leaf.id};
    b.childRoomIds = {c.id, leaf.id};
    c.childRoomIds = {a.id}; // malformed cycle
    leaf.parentSpaceIds = {a.id, b.id};
    client.mirror = {a, b, c, leaf};
    spaces.setClient(&client);
    QVERIFY(spaces.includesRoom(a.id, leaf.id));
    QVERIFY(spaces.includesRoom(b.id, leaf.id));
    QCOMPARE(spaces.roomsInSpace(a.id).count(leaf.id), 1);
}

void RoomStateModelTest::homeAggregatesSharedRoomWithoutDoubleCounting()
{
    // A room reachable from two Spaces contributes to Home exactly once.
    FakeClient client;
    SpaceManager spaces;
    auto a = room(QStringLiteral("!a:example.org")); a.isSpace = true;
    auto b = room(QStringLiteral("!b:example.org")); b.isSpace = true;
    auto leaf = room(QStringLiteral("!leaf:example.org"));
    leaf.unreadCount = 4;
    leaf.highlightCount = 1;
    a.childRoomIds = {leaf.id};
    b.childRoomIds = {leaf.id};
    leaf.parentSpaceIds = {a.id, b.id};
    client.mirror = {a, b, leaf};
    spaces.setClient(&client);
    // Home (row 0) counts the shared leaf once, not once per parent Space.
    QCOMPARE(spaces.data(spaces.index(0), SpaceManager::UnreadTotalRole).toInt(), 4);
    QCOMPARE(spaces.data(spaces.index(0), SpaceManager::HighlightTotalRole).toInt(), 1);
}

void RoomStateModelTest::selectedSpaceDisappearingReturnsHome()
{
    FakeClient client;
    SpaceManager spaces;
    auto a = room(QStringLiteral("!a:example.org")); a.isSpace = true;
    auto leaf = room(QStringLiteral("!leaf:example.org"));
    a.childRoomIds = {leaf.id};
    client.mirror = {a, leaf};
    spaces.setClient(&client);
    spaces.setActiveSpaceId(a.id);
    QCOMPARE(spaces.activeSpaceId(), a.id);
    // The Space leaves; rebuild must fall back to Home, not a dangling id.
    client.mirror = {leaf};
    Q_EMIT client.roomsChanged();
    QVERIFY(spaces.activeSpaceId().isEmpty());
}

void RoomStateModelTest::searchFiltersNameAndAliasAndFindsInvites()
{
    FakeClient client;
    RoomListModel model;
    auto invite = room(QStringLiteral("!inv:example.org"));
    invite.name = QStringLiteral("Alpha Invite");
    invite.membership = RoomInfo::Invited;
    auto r1 = room(QStringLiteral("!r1:example.org"));
    r1.name = QStringLiteral("Alpha Room");
    auto r2 = room(QStringLiteral("!r2:example.org"));
    r2.name = QStringLiteral("Beta Room");
    r2.canonicalAlias = QStringLiteral("#alpha-alias:example.org");
    client.mirror = {invite, r1, r2};
    model.setClient(&client);
    QCOMPARE(model.rowCount(), 3);

    // Debounced search over name AND canonical alias. Synchronize on the
    // debounced searchQuery property before asserting the filtered rows so
    // the check never races a stale previous result. "invite" narrows to
    // the invitation alone — invites are findable, not hidden.
    model.setSearchQuery(QStringLiteral("invite"));
    QTRY_COMPARE(model.searchQuery(), QStringLiteral("invite"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), RoomListModel::RoomIdRole).toString(),
             invite.id);
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("invite"));
    // Match on canonical alias only (r2's name is "Beta Room").
    model.setSearchQuery(QStringLiteral("alpha-alias"));
    QTRY_COMPARE(model.searchQuery(), QStringLiteral("alpha-alias"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), RoomListModel::RoomIdRole).toString(),
             r2.id);
    // "alpha" matches both names and the alias.
    model.setSearchQuery(QStringLiteral("alpha"));
    QTRY_COMPARE(model.searchQuery(), QStringLiteral("alpha"));
    QCOMPARE(model.rowCount(), 3);
    // Clearing restores every row.
    model.setSearchQuery(QString{});
    QTRY_COMPARE(model.searchQuery(), QString{});
    QCOMPARE(model.rowCount(), 3);
}

QTEST_GUILESS_MAIN(RoomStateModelTest)
#include "RoomStateModelTest.moc"
