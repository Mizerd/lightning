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
    QString selfUserId = QStringLiteral("@me:example.org");
    QString accepted;
    QString rejected;
    QString marked;
    QString favouriteRoom;
    int favouriteWrites = 0;
    bool lastFavourite = false;
    bool favouritesSupported = true;
    quint64 profileOp = 0;
    QString profileUser;

    using MatrixClient::MatrixClient;
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return selfUserId; }
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
    quint64 fetchUserProfile(const QString &userId) override
    {
        profileUser = userId;
        return profileOp = profileOp + 1;
    }
    void acceptInvite(const QString &id) override { accepted = id; }
    void rejectInvite(const QString &id) override { rejected = id; }
    void setRoomMarkedUnread(const QString &id, bool unread) override
    {
        if (unread) marked = id;
    }
    bool supportsRoomFavourites() const override { return favouritesSupported; }
    // Deliberately does NOT touch `mirror`: a real backend only reflects the
    // tag back once the server accepted it, and these tests assert that the
    // model shows nothing until that happens.
    void setRoomFavourite(const QString &id, bool favourite) override
    {
        favouriteRoom = id;
        lastFavourite = favourite;
        ++favouriteWrites;
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
    void effectiveDirectAvatarPolicy();
    void effectiveDirectAvatarRefreshAndAccountIsolation();
    void missingDirectAvatarResolvesWithoutSearch();
    void missingDirectAvatarResolvesWithoutMemberSnapshot();
    void selfDirectMessageAdoptsOwnAvatar();
    void mismatchedProfileResultDoesNotWedgePending();
    void groupDirectMappingDoesNotResolveMemberAvatar();
    void inviteActionsRouteAndStaySeparate();
    void explicitDiffOperationsValidateIndexesAndIdentity();
    void replaceValidatesIdentityAndReset();
    void nestedSpacesAreCycleSafeAndSupportMultipleParents();
    void homeAggregatesSharedRoomWithoutDoubleCounting();
    void selectedSpaceDisappearingReturnsHome();
    void searchFiltersNameAndAliasAndFindsInvites();
    void filterModeSplitsPeopleRoomsUnreads();
    void identityColorKeyPolicyForDms();
    void favouritesFormTheirOwnSectionAboveEverythingButInvites();
    void favouriteToggleIsNeverAppliedLocally();
    void everyCategoryTheModelEmitsHasASectionLabel();
    void theFavouritesSectionIsClosedOffByADivider();
    void theFavouritesBoundaryIsOwnedByTheModel();
    void roomActivityOnlyEverMovesForward();
    void aDirectMessageIsNeverScopedByTheSelectedSpace();
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
    // v0.7 perf round: per-room updates coalesce onto a zero-timer
    // reconcile; settle it before asserting.
    QCoreApplication::processEvents();
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

void RoomStateModelTest::effectiveDirectAvatarPolicy()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), true);
    dm.directUserId = QStringLiteral("@bob:example.org");
    MemberInfo self{client.selfUserId, QStringLiteral("Me"),
                    QStringLiteral("mxc://example.org/self")};
    MemberInfo bob{dm.directUserId, QStringLiteral("Bob"),
                   QStringLiteral("mxc://example.org/bob")};
    dm.members.insert(self.userId, self);
    dm.members.insert(bob.userId, bob);
    client.mirror = {dm};
    model.setClient(&client);
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             bob.avatarMxcUrl);
    QCOMPARE(model.findRoom(dm.id).value(QStringLiteral("avatarUrl")).toString(),
             bob.avatarMxcUrl);

    client.mirror[0].avatarUrl = QStringLiteral("mxc://example.org/room");
    Q_EMIT client.roomsChanged();
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             client.mirror[0].avatarUrl);

    client.mirror[0].avatarUrl.clear();
    client.mirror[0].members[bob.userId].avatarMxcUrl.clear();
    Q_EMIT client.membersChanged(dm.id);
    QCoreApplication::processEvents();
    QVERIFY(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString().isEmpty());

    MemberInfo carol{QStringLiteral("@carol:example.org"), QStringLiteral("Carol"),
                     QStringLiteral("mxc://example.org/carol")};
    client.mirror[0].members.insert(carol.userId, carol);
    Q_EMIT client.membersChanged(dm.id);
    QCoreApplication::processEvents();
    QVERIFY(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString().isEmpty());

    client.mirror[0].members.remove(carol.userId);
    client.mirror[0].isDirect = false;
    client.mirror[0].members[bob.userId].avatarMxcUrl = bob.avatarMxcUrl;
    Q_EMIT client.roomsChanged();
    QVERIFY(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString().isEmpty());
}

void RoomStateModelTest::effectiveDirectAvatarRefreshAndAccountIsolation()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), true);
    dm.directUserId = QStringLiteral("@bob:example.org");
    dm.members.insert(client.selfUserId,
                      MemberInfo{client.selfUserId, {}, QStringLiteral("mxc://old/self")});
    dm.members.insert(dm.directUserId,
                      MemberInfo{dm.directUserId, {}, QStringLiteral("mxc://old/bob")});
    client.mirror = {dm};
    model.setClient(&client);
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             QStringLiteral("mxc://old/bob"));

    client.mirror[0].members[dm.directUserId].avatarMxcUrl =
        QStringLiteral("mxc://old/bob-new");
    Q_EMIT client.membersChanged(dm.id);
    QCoreApplication::processEvents();
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             QStringLiteral("mxc://old/bob-new"));

    client.selfUserId = QStringLiteral("@new:example.org");
    client.mirror.clear();
    Q_EMIT client.loggedOut();
    QCOMPARE(model.rowCount(), 0);
    auto next = room(QStringLiteral("!new:example.org"), true);
    next.directUserId = QStringLiteral("@dana:example.org");
    next.members.insert(client.selfUserId, MemberInfo{client.selfUserId, {}, {}});
    next.members.insert(next.directUserId,
                        MemberInfo{next.directUserId, {}, QStringLiteral("mxc://new/dana")});
    client.mirror = {next};
    Q_EMIT client.loginSucceeded(client.selfUserId);
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             QStringLiteral("mxc://new/dana"));
}

void RoomStateModelTest::missingDirectAvatarResolvesWithoutSearch()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), true);
    dm.directUserId = QStringLiteral("@bob:example.org");
    dm.members.insert(client.selfUserId, MemberInfo{client.selfUserId, {}, {}});
    dm.members.insert(dm.directUserId, MemberInfo{dm.directUserId, {}, {}});
    client.mirror = {dm};
    model.setClient(&client);
    QCOMPARE(client.profileUser, dm.directUserId);

    QSignalSpy changed(&model, &RoomListModel::dataChanged);
    Q_EMIT client.userProfileFinished(client.profileOp, true, dm.directUserId,
                                      QStringLiteral("Bob"),
                                      QStringLiteral("mxc://example.org/bob"), {});
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             QStringLiteral("mxc://example.org/bob"));
    QVERIFY(changed.count() > 0);
}

// 0.5.14 checkpoint 3: the real (Rust) backend never populates
// RoomInfo::members at all — it is fetched separately, on demand, only for
// the Room Information "People" tab, and that result never flows back into
// RoomListModel. The 0.5.13 test above (missingDirectAvatarResolvesWithoutSearch)
// artificially inserts member entries, which is exactly why it passed while
// the feature was still broken live: effectiveAvatarUrl() required a
// populated member snapshot to identify "the other participant" before it
// would ever consult the profile-fetch cache. This test reproduces the real
// backend's shape — isDirect + directUserId(s) set, members left entirely
// empty — end to end.
void RoomStateModelTest::missingDirectAvatarResolvesWithoutMemberSnapshot()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), true);
    // An explicit room name must not disable member-avatar derivation.
    dm.name = QStringLiteral("Mizerd");
    dm.directUserId = QStringLiteral("@bob:example.org");
    dm.directUserIds = { dm.directUserId };
    QVERIFY(dm.members.isEmpty());
    client.mirror = {dm};
    model.setClient(&client);
    QCOMPARE(client.profileUser, dm.directUserId);
    QVERIFY(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString().isEmpty());

    QSignalSpy changed(&model, &RoomListModel::dataChanged);
    Q_EMIT client.userProfileFinished(client.profileOp, true, dm.directUserId,
                                      QStringLiteral("Bob"),
                                      QStringLiteral("mxc://example.org/bob"), {});
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             QStringLiteral("mxc://example.org/bob"));
    QCOMPARE(model.findRoom(dm.id).value(QStringLiteral("avatarUrl")).toString(),
             QStringLiteral("mxc://example.org/bob"));
    QVERIFY(changed.count() > 0);
    QVERIFY(dm.members.isEmpty()); // never required
}

// A self-DM ("notes to self") has the direct target equal to our OWN user id,
// no room avatar and no member snapshot (the real backend shape). The room
// list never sees the per-event room-member avatar the timeline uses, so it
// must adopt the signed-in account's own avatar — even when that profile was
// fetched by another consumer (the account switcher) via an op the room list
// did not start. Otherwise the entry is stuck on an "M" initial forever.
void RoomStateModelTest::selfDirectMessageAdoptsOwnAvatar()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!self:example.org"), true);
    dm.directUserId = client.selfUserId;
    dm.directUserIds = { dm.directUserId };
    QVERIFY(dm.members.isEmpty());
    client.mirror = {dm};
    model.setClient(&client);
    QVERIFY(model.data(model.index(0), RoomListModel::AvatarUrlRole)
                .toString().isEmpty());

    // Own profile resolved for the account switcher: an op the model never
    // started (opId not in its map). It must still be adopted for the self-DM.
    QSignalSpy changed(&model, &RoomListModel::dataChanged);
    Q_EMIT client.userProfileFinished(/*opId=*/9999, true, client.selfUserId,
                                      QStringLiteral("Me"),
                                      QStringLiteral("mxc://example.org/me"), {});
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             QStringLiteral("mxc://example.org/me"));
    QVERIFY(changed.count() > 0);
}

// A profile result whose returned user id differs from the requested string
// (SDK id normalization) must still release the pending marker, so the next
// reconcile re-fetches. Previously the early return on the mismatch wedged the
// target permanently pending and the DM avatar could never resolve.
void RoomStateModelTest::mismatchedProfileResultDoesNotWedgePending()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), true);
    dm.directUserId = QStringLiteral("@bob:example.org");
    dm.directUserIds = { dm.directUserId };
    client.mirror = {dm};
    model.setClient(&client);
    QCOMPARE(client.profileUser, dm.directUserId);
    const quint64 firstOp = client.profileOp;

    // Mismatched id, no avatar: releases pending without caching.
    Q_EMIT client.userProfileFinished(firstOp, true,
                                      QStringLiteral("@BOB:example.org"),
                                      QStringLiteral("Bob"), {}, {});
    // The still-avatarless target must be re-fetched on the next reconcile.
    client.profileUser.clear();
    Q_EMIT client.roomUpdated(dm.id);
    QCoreApplication::processEvents();
    QCOMPARE(client.profileUser, dm.directUserId);
    QVERIFY(client.profileOp > firstOp);
}

// A room m.direct maps against more than one target user is a group DM (or
// an ambiguous mapping) and must never get an arbitrary member's avatar —
// verified via the authoritative directUserIds list (the Rust backend's
// signal), independent of any member snapshot.
void RoomStateModelTest::groupDirectMappingDoesNotResolveMemberAvatar()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!group-dm:example.org"), true);
    dm.directUserId = QStringLiteral("@bob:example.org");
    dm.directUserIds = { QStringLiteral("@bob:example.org"),
                        QStringLiteral("@carol:example.org") };
    client.mirror = {dm};
    model.setClient(&client);

    QVERIFY(client.profileUser.isEmpty()); // never even attempted
    QVERIFY(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString().isEmpty());
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

// A direct message is not a room in a Space: Matrix has no notion of one
// belonging to a Space unless somebody adds it as an m.space.child, which
// essentially nobody does. Scoping DMs by the selected Space therefore hid
// every one of them in every Space — reported as "the people tab in
// spaces/rooms isnt populated".
//
// Exempting only the People CHIP fixed that list and broke a bigger one: All
// then showed fewer rooms than People did, reported as "in all tab people are
// not shown". The exemption belongs to the DM, not to the chip.
void RoomStateModelTest::aDirectMessageIsNeverScopedByTheSelectedSpace()
{
    FakeClient client;
    SpaceManager spaces;
    auto space = room(QStringLiteral("!space:example.org"));
    space.isSpace = true;
    auto inSpace = room(QStringLiteral("!room:example.org"));
    auto dm = room(QStringLiteral("!dm:example.org"), /*direct=*/true);
    space.childRoomIds = { inSpace.id };
    inSpace.parentSpaceIds = { space.id };
    client.mirror = { space, inSpace, dm };

    RoomListModel model;
    spaces.setClient(&client);
    model.setSpaceManager(&spaces);
    model.setClient(&client);
    spaces.setActiveSpaceId(space.id);

    // All: the Space's rooms AND the DM. "All" showing fewer rows than
    // "People" is the bug this half exists to prevent.
    model.setFilterMode(0);
    QCOMPARE(model.rowCount(), 2);

    // People: the DM, even though it is not a child of this Space. Without
    // this the list is empty and there is no way to reach a DM from inside
    // a Space at all.
    model.setFilterMode(1);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), RoomListModel::RoomIdRole).toString(), dm.id);

    // Rooms stays scoped — a DM must not leak into it, and a room outside
    // the Space must not either.
    model.setFilterMode(2);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), RoomListModel::RoomIdRole).toString(),
             inSpace.id);

    // ...and All is exactly People plus Rooms, which is what the word means.
    model.setFilterMode(0);
    QCOMPARE(model.rowCount(), 2);
}

// 2026-08-14: Element-style filter chips. People/Rooms split on m.direct;
// Unreads keeps unread rooms, the pinned (open) room, and — like every
// mode — invites, which always need action.
void RoomStateModelTest::filterModeSplitsPeopleRoomsUnreads()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), /*direct=*/true);
    auto quiet = room(QStringLiteral("!quiet:example.org"));
    auto busy = room(QStringLiteral("!busy:example.org"));
    busy.hasUnreadMessages = true;
    auto invite = room(QStringLiteral("!inv:example.org"));
    invite.membership = RoomInfo::Invited;
    client.mirror = {dm, quiet, busy, invite};
    model.setClient(&client);
    QCOMPARE(model.rowCount(), 4);

    const auto ids = [&model]() {
        QStringList out;
        for (int i = 0; i < model.rowCount(); ++i)
            out.append(model.data(model.index(i),
                                  RoomListModel::RoomIdRole).toString());
        std::sort(out.begin(), out.end());
        return out;
    };

    model.setFilterMode(1); // People
    QCOMPARE(ids(), (QStringList{dm.id, invite.id}));
    model.setFilterMode(2); // Rooms
    QCOMPARE(ids(), (QStringList{busy.id, invite.id, quiet.id}));
    model.setFilterMode(3); // Unreads
    QCOMPARE(ids(), (QStringList{busy.id, invite.id}));
    // The pinned (open) room stays visible in Unreads even when read.
    model.setPinnedRoomId(quiet.id);
    QCOMPARE(ids(), (QStringList{busy.id, invite.id, quiet.id}));
    model.setPinnedRoomId(QString{});
    QCOMPARE(ids(), (QStringList{busy.id, invite.id}));
    // Out-of-range modes fall back to All (both directions — never
    // edge-snapped).
    model.setFilterMode(7);
    QCOMPARE(model.filterMode(), 0);
    QCOMPARE(model.rowCount(), 4);
    model.setFilterMode(2);
    model.setFilterMode(-2);
    QCOMPARE(model.filterMode(), 0);
    QCOMPARE(model.rowCount(), 4);

    // Home's recent strip is immune to the mode filter.
    model.setFilterMode(1);
    const QVariantList recents = model.recentRooms(6);
    QCOMPARE(recents.size(), 3); // dm + quiet + busy; the invite is not joined
}

// 2026-08-14: one fallback-colour policy — an unambiguous 1:1 DM is
// coloured as the person (their MXID); group DMs and plain rooms as the
// room (live report: the same user rendered in different colours across
// surfaces).
void RoomStateModelTest::identityColorKeyPolicyForDms()
{
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), /*direct=*/true);
    dm.directUserId = QStringLiteral("@ga:example.org");
    dm.directUserIds = {QStringLiteral("@ga:example.org")};
    auto groupDm = room(QStringLiteral("!group:example.org"), /*direct=*/true);
    groupDm.directUserId = QStringLiteral("@a:example.org");
    groupDm.directUserIds = {QStringLiteral("@a:example.org"),
                             QStringLiteral("@b:example.org")};
    auto plain = room(QStringLiteral("!room:example.org"));
    client.mirror = {dm, groupDm, plain};
    model.setClient(&client);

    const auto keyOf = [&model](const QString &roomId) {
        for (int i = 0; i < model.rowCount(); ++i) {
            if (model.data(model.index(i), RoomListModel::RoomIdRole)
                    .toString() == roomId)
                return model.data(model.index(i),
                                  RoomListModel::IdentityColorKeyRole)
                    .toString();
        }
        return QString();
    };
    QCOMPARE(keyOf(dm.id), QStringLiteral("@ga:example.org"));
    QCOMPARE(keyOf(groupDm.id), groupDm.id); // ambiguous — never a member's
    QCOMPARE(keyOf(plain.id), plain.id);
    QCOMPARE(model.findRoom(dm.id)
                 .value(QStringLiteral("identityColorKey")).toString(),
             QStringLiteral("@ga:example.org"));
}

// Element classic pins a Favourites section above People and Rooms, and a
// favourited DM lives THERE rather than in both places. RoomsPanel opens one
// header per contiguous run of the `category` role, so this also pins the
// property that makes the headers work at all: each category is exactly one
// run, which is only true while the sort and the role read the same
// classification (RoomListModel::groupIndexOf).
void RoomStateModelTest::favouritesFormTheirOwnSectionAboveEverythingButInvites()
{
    FakeClient client;
    RoomListModel model;
    auto invite = room(QStringLiteral("!invite:example.org"));
    invite.membership = RoomInfo::Invited;
    auto favouriteDm = room(QStringLiteral("!favdm:example.org"), true, 2);
    favouriteDm.isFavourite = true;
    auto favouriteRoom = room(QStringLiteral("!favroom:example.org"));
    favouriteRoom.isFavourite = true;
    const auto plainDm = room(QStringLiteral("!dm:example.org"), true, 2);
    const auto plainRoom = room(QStringLiteral("!room:example.org"));
    // Deliberately shuffled: the ordering must come from the sort, not from
    // the order the backend happened to hand rooms over in.
    client.mirror = { plainRoom, favouriteDm, invite, plainDm, favouriteRoom };
    model.setClient(&client);
    QCOMPARE(model.rowCount(), 5);

    QStringList categories;
    for (int i = 0; i < model.rowCount(); ++i) {
        categories << model.data(model.index(i), RoomListModel::CategoryRole)
                          .toString();
    }
    QCOMPARE(categories,
             (QStringList{ QStringLiteral("invite"), QStringLiteral("favourite"),
                           QStringLiteral("favourite"), QStringLiteral("dm"),
                           QStringLiteral("room") }));
    // Every category is ONE contiguous run — a second "People" header
    // halfway down the list is the failure this guards.
    QStringList runs;
    for (const QString &category : std::as_const(categories)) {
        if (runs.isEmpty() || runs.constLast() != category)
            runs << category;
    }
    QCOMPARE(runs.size(), 4);

    // The favourited DM reports the favourite section AND is still a DM, so
    // the People filter keeps it (Element does the same).
    const auto rowOf = [&model](const QString &roomId) {
        for (int i = 0; i < model.rowCount(); ++i) {
            if (model.data(model.index(i), RoomListModel::RoomIdRole)
                    .toString() == roomId)
                return i;
        }
        return -1;
    };
    QVERIFY(model.data(model.index(rowOf(favouriteDm.id)),
                       RoomListModel::IsDirectRole).toBool());
    QVERIFY(model.data(model.index(rowOf(favouriteDm.id)),
                       RoomListModel::IsFavouriteRole).toBool());
    QVERIFY(!model.data(model.index(rowOf(plainDm.id)),
                        RoomListModel::IsFavouriteRole).toBool());
    model.setFilterMode(1); // People
    QCOMPARE(model.rowCount(), 3); // invite always passes, + both DMs
    QCOMPARE(model.data(model.index(rowOf(favouriteDm.id)),
                        RoomListModel::CategoryRole).toString(),
             QStringLiteral("favourite"));
}

// The tag is ACCOUNT state. Flipping the row locally would show a favourite
// the account does not have whenever the server refuses the write — and
// worse, would park the row under a Favourites header it does not belong in.
void RoomStateModelTest::favouriteToggleIsNeverAppliedLocally()
{
    FakeClient client;
    RoomListModel model;
    client.mirror = { room(QStringLiteral("!room:example.org")) };
    model.setClient(&client);
    QVERIFY(model.roomFavouritesSupported());
    QVERIFY(!model.isRoomFavourite(QStringLiteral("!room:example.org")));

    model.setRoomFavourite(QStringLiteral("!room:example.org"), true);
    QCOMPARE(client.favouriteRoom, QStringLiteral("!room:example.org"));
    QCOMPARE(client.favouriteWrites, 1);
    QVERIFY(client.lastFavourite);
    // The backend has not confirmed anything, so nothing moved.
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("room"));
    QVERIFY(!model.data(model.index(0), RoomListModel::IsFavouriteRole).toBool());

    // Only the backend reflecting the tag back changes the row.
    client.mirror[0].isFavourite = true;
    Q_EMIT client.roomsChanged();
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("favourite"));
    QVERIFY(model.isRoomFavourite(QStringLiteral("!room:example.org")));

    client.favouritesSupported = false;
    QVERIFY(!model.roomFavouritesSupported());
}

// categoryOf() is read by RoomsPanel's `section.property`, and its section
// delegate ends in a bare `: qsTr("Rooms")` fallback — so a category added
// here without a label there does not fail, it renders "Rooms" over a
// section that is not rooms. That is exactly how the Favourites section
// would have shipped mislabelled.
void RoomStateModelTest::everyCategoryTheModelEmitsHasASectionLabel()
{
    QFile file(QStringLiteral(QML_DIR "/RoomsPanel.qml"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(file.fileName()));
    const QString source = QString::fromUtf8(file.readAll());

    RoomInfo probe;
    probe.membership = RoomInfo::Invited;
    QStringList emitted{ RoomListModel::categoryOf(probe) };
    probe.membership = RoomInfo::Joined;
    probe.isFavourite = true;
    emitted << RoomListModel::categoryOf(probe);
    probe.isFavourite = false;
    probe.isDirect = true;
    emitted << RoomListModel::categoryOf(probe);
    probe.isDirect = false;
    emitted << RoomListModel::categoryOf(probe);
    QCOMPARE(emitted,
             (QStringList{ QStringLiteral("invite"), QStringLiteral("favourite"),
                           QStringLiteral("dm"), QStringLiteral("room") }));

    // "room" is the delegate's fallback branch and carries no test, so it is
    // checked by its label instead.
    for (const QString &category : std::as_const(emitted)) {
        if (category == QStringLiteral("room"))
            continue;
        QVERIFY2(source.contains(QStringLiteral("section === \"%1\"")
                                     .arg(category)),
                 qPrintable(QStringLiteral("RoomsPanel has no section label "
                                           "for category '%1'")
                                .arg(category)));
    }
    QVERIFY(source.contains(QStringLiteral("qsTr(\"Favourites\")")));
    QVERIFY(source.contains(QStringLiteral("qsTr(\"Rooms\")")));
}


// The Favourites group is pinned above People and Rooms, but the section
// labels sit on the bare sidebar with no fill, so a short favourites block
// ran straight into the next group. The rule closing it off is scoped by
// THREE clauses and each one is load-bearing:
//   * it draws only on a favourite row;
//   * only on the LAST one (nextSection differs);
//   * and not when that row is the last in the whole view, where
//     ListView.nextSection is "" — without that clause a list whose only
//     group is favourites hangs a rule off its bottom edge.
// A source scan is the honest level here: this is a pure presentation
// binding with no C++ side, and the alternative (loading the panel
// offscreen with a seeded model) would assert Qt's section machinery
// rather than Lightning's use of it.
void RoomStateModelTest::theFavouritesSectionIsClosedOffByADivider()
{
    QFile panel(QStringLiteral(QML_DIR "/RoomsPanel.qml"));
    QVERIFY2(panel.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(panel.fileName()));
    const QString panelSource = QString::fromUtf8(panel.readAll());

    const int bindingAt = panelSource.indexOf(QStringLiteral("showGroupDivider:"));
    QVERIFY2(bindingAt >= 0, "RoomsPanel never binds showGroupDivider");
    const QString binding = panelSource.mid(bindingAt, 260);
    // The view must ASK THE MODEL. ListView.section / ListView.nextSection
    // go stale under reuseItems and row moves — selecting a room re-sorts
    // the list, and the rule then vanished until something else redrew it.
    QVERIFY2(binding.contains(QStringLiteral("favouritesBoundaryRoomId")),
             "the divider is not driven by the model's boundary property");
    QVERIFY2(!binding.contains(QStringLiteral("ListView.nextSection")),
             "the divider is back on the stale attached-property path");

    QFile row(QStringLiteral(QML_DIR "/RoomDelegate.qml"));
    QVERIFY2(row.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(row.fileName()));
    const QString rowSource = QString::fromUtf8(row.readAll());

    QVERIFY2(rowSource.contains(QStringLiteral("property bool showGroupDivider: false")),
             "RoomDelegate must default the divider OFF — RoomListPane has no "
             "sections at all and would otherwise rule every row");

    const int dividerAt = rowSource.indexOf(QStringLiteral("objectName: \"roomGroupDivider\""));
    QVERIFY2(dividerAt >= 0, "RoomDelegate has no divider item");
    const QString divider = rowSource.mid(dividerAt, 420);
    QVERIFY2(divider.contains(QStringLiteral("anchors.bottom: parent.bottom")),
             "the rule must anchor to the row's own bottom edge");
    // Anchored, never laid out: a rule that added height would move every
    // row below it the moment a room was favourited.
    QVERIFY2(!divider.contains(QStringLiteral("Layout.")),
             "the rule must not participate in the row's layout");
    // Drawn OFF the selection chip, not over it: `border` on a `selected`
    // fill is very nearly invisible, and that is what "the divider gets
    // hidden when the room is selected" looked like.
    QVERIFY2(rowSource.contains(
                 QStringLiteral("anchors.bottomMargin: root.showGroupDivider ? 1 : 0")),
             "the selection chip must yield the row's last pixel line to the rule");
}

void RoomStateModelTest::theFavouritesBoundaryIsOwnedByTheModel()
{
    FakeClient client;
    RoomListModel model;
    auto favA = room(QStringLiteral("!favA:example.org"));
    favA.isFavourite = true;
    auto favB = room(QStringLiteral("!favB:example.org"));
    favB.isFavourite = true;
    const auto plain = room(QStringLiteral("!plain:example.org"));
    client.mirror = { favA, favB, plain };
    model.setClient(&client);

    // Sorted invites -> favourites -> DMs -> rooms, so the boundary is the
    // second favourite, whichever order the two of them settle in.
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.data(model.index(2), RoomListModel::CategoryRole).toString(),
             QStringLiteral("room"));
    const QString expected =
        model.data(model.index(1), RoomListModel::RoomIdRole).toString();
    QCOMPARE(model.favouritesBoundaryRoomId(), expected);

    // Un-favouriting the boundary room moves the rule up a row.
    QSignalSpy moved(&model, &RoomListModel::favouritesBoundaryRoomIdChanged);
    for (auto &r : client.mirror) {
        if (r.id == expected)
            r.isFavourite = false;
    }
    Q_EMIT client.roomUpdated(expected);
    QCoreApplication::processEvents();
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("favourite"));
    QCOMPARE(model.favouritesBoundaryRoomId(),
             model.data(model.index(0), RoomListModel::RoomIdRole).toString());
    QVERIFY(moved.count() >= 1);

    // A list that is ENTIRELY favourites has nothing below the group, and a
    // trailing rule would divide it from the bottom of the window.
    for (auto &r : client.mirror)
        r.isFavourite = true;
    for (const auto &r : client.mirror)
        Q_EMIT client.roomUpdated(r.id);
    QCoreApplication::processEvents();
    QCOMPARE(model.data(model.index(2), RoomListModel::CategoryRole).toString(),
             QStringLiteral("favourite"));
    QVERIFY2(model.favouritesBoundaryRoomId().isEmpty(),
             "a favourites-only list must not hang a trailing rule");
}

void RoomStateModelTest::roomActivityOnlyEverMovesForward()
{
    // The room list sorts on RoomInfo::lastActivity, and four separate places
    // used to assign it. When any two disagreed a room jumped and fell back —
    // reported three times as "clicking an older room moves it upwards in the
    // room order, then it drops back down to where it was".
    RoomInfo room;
    const QDateTime older = QDateTime::fromMSecsSinceEpoch(1'700'000'000'000);
    const QDateTime newer = older.addSecs(600);

    // Nothing known yet: any valid stamp takes.
    QVERIFY(!room.lastActivity.isValid());
    QVERIFY(!room.raiseActivity(QDateTime()));   // invalid is never news
    QVERIFY(!room.lastActivity.isValid());
    QVERIFY(room.raiseActivity(newer));
    QCOMPARE(room.lastActivity, newer);

    // A stale answer — a summary that has not caught up, or a timeline that
    // was loaded and then unloaded — must not pull the room back down.
    QVERIFY(!room.raiseActivity(older));
    QCOMPARE(room.lastActivity, newer);
    // Nor may an identical one report a change and churn the list.
    QVERIFY(!room.raiseActivity(newer));
    QCOMPARE(room.lastActivity, newer);
    // Genuinely newer still moves.
    const QDateTime newest = newer.addSecs(1);
    QVERIFY(room.raiseActivity(newest));
    QCOMPARE(room.lastActivity, newest);
}

QTEST_GUILESS_MAIN(RoomStateModelTest)
#include "RoomStateModelTest.moc"
