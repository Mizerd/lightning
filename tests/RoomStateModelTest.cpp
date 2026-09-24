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

/// `n` seconds before a fixed instant, so "newer" is unambiguous and nothing
/// depends on wall-clock time.
QDateTime ago(int seconds)
{
    static const QDateTime base =
        QDateTime(QDate(2026, 8, 31), QTime(13, 0), QTimeZone::UTC);
    return base.addSecs(-seconds);
}

RoomInfo at(const QString &id, bool direct, int secondsAgo)
{
    RoomInfo value = room(id, direct);
    value.lastActivity = ago(secondsAgo);
    return value;
}

/// Row order as room ids, top to bottom.
QStringList orderOf(const RoomListModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i)
        out << model.data(model.index(i), RoomListModel::RoomIdRole).toString();
    return out;
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
    // Does not touch `mirror`: a real backend reflects the tag only once the
    // server accepts it, and the tests assert the model shows nothing before.
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
    void invitesStayOnTopAndEverythingElseIsOneActivityFeed();
    void aNewerRoomOutranksAnOlderDirectMessage();
    void aNewerDirectMessageOutranksAnOlderRoom();
    void aFavouriteOutranksRecencyButNotEachOther();
    void peopleAndRoomsFiltersAreRecentFirst();
    void unreadsFilterMixesDirectMessagesAndRoomsByRecency();
    void incomingActivityMovesOneRowAndKeepsTheSelection();
    void aSpaceScopedListUsesTheSameRecencyRule();
    void roomsWithNoActivityYetSortLastAndDeterministically();
    void favouriteToggleIsNeverAppliedLocally();
    void everyCategoryTheModelEmitsHasASectionLabel();
    void theFavouritesSectionIsClosedOffByADivider();
    void theFavouritesBoundaryIsRetiredWithTheGroup();
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
    // Direct-ness is IsDirectRole. The category role only reports the section
    // (invite or conversation), since DMs and rooms share one feed.
    QVERIFY(model.data(model.index(0), RoomListModel::IsDirectRole).toBool());
    QVERIFY(!model.data(model.index(1), RoomListModel::IsDirectRole).toBool());
}

void RoomStateModelTest::liveDirectUpdateChangesCategory()
{
    FakeClient client;
    RoomListModel model;
    client.mirror = { room(QStringLiteral("!room:example.org"), false, 2) };
    model.setClient(&client);
    QVERIFY(!model.data(model.index(0), RoomListModel::IsDirectRole).toBool());
    client.mirror[0].isDirect = true;
    Q_EMIT client.roomUpdated(client.mirror[0].id);
    // Per-room updates coalesce onto a zero-timer reconcile; settle it first.
    QCoreApplication::processEvents();
    QVERIFY(model.data(model.index(0), RoomListModel::IsDirectRole).toBool());
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
    // A room dropped from m.direct returns to rooms; member count is never
    // consulted for classification.
    FakeClient client;
    RoomListModel model;
    auto dm = room(QStringLiteral("!dm:example.org"), true, 5);
    dm.directUserId = QStringLiteral("@bob:example.org");
    client.mirror = { dm };
    model.setClient(&client);
    QVERIFY(model.data(model.index(0), RoomListModel::IsDirectRole).toBool());
    client.mirror[0].isDirect = false;
    client.mirror[0].directUserId.clear();
    Q_EMIT client.roomsChanged();
    QVERIFY(!model.data(model.index(0), RoomListModel::IsDirectRole).toBool());
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

// The real backend never populates RoomInfo::members (it is fetched on demand
// for the Room Information People tab only), so a DM's avatar must resolve
// from isDirect + directUserId(s) with an empty member snapshot, end to end.
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

// A self-DM (direct target is our own user id, no room avatar, no member
// snapshot) adopts the signed-in account's own avatar, even when that profile
// was fetched by another consumer via an op the room list did not start.
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

    // Own profile resolved via an op the model never started (e.g. the account
    // switcher); still adopted for the self-DM.
    QSignalSpy changed(&model, &RoomListModel::dataChanged);
    Q_EMIT client.userProfileFinished(/*opId=*/9999, true, client.selfUserId,
                                      QStringLiteral("Me"),
                                      QStringLiteral("mxc://example.org/me"), {});
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             QStringLiteral("mxc://example.org/me"));
    QVERIFY(changed.count() > 0);
}

// A profile result whose user id differs from the requested string (SDK
// normalization) releases the pending marker for both ids and records the
// answer, so the target is neither wedged pending nor re-asked on every
// rebuild. A face arriving later on a member event still wins, and sign-out
// clears the cache.
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

    // Mismatched id, no avatar: both ids are released from pending and recorded
    // as "asked, no picture".
    Q_EMIT client.userProfileFinished(firstOp, true,
                                      QStringLiteral("@BOB:example.org"),
                                      QStringLiteral("Bob"), {}, {});
    // Not re-asked.
    client.profileUser.clear();
    Q_EMIT client.roomUpdated(dm.id);
    QCoreApplication::processEvents();
    QVERIFY2(client.profileUser.isEmpty(),
             "an avatarless answer was re-asked on the next reconcile — that "
             "is the unbounded loop, not a recovery");

    // Not wedged: a face that turns up later still lands.
    Q_EMIT client.userProfileFinished(firstOp + 1, true, dm.directUserId,
                                      QStringLiteral("Bob"),
                                      QStringLiteral("mxc://example.org/bob"),
                                      {});
    QCOMPARE(model.data(model.index(0), RoomListModel::AvatarUrlRole).toString(),
             QStringLiteral("mxc://example.org/bob"));
}

// A room m.direct maps to more than one user (group DM or ambiguous mapping)
// never gets an arbitrary member's avatar; decided from directUserIds, not
// any member snapshot.
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
    // identity swap that could duplicate a row).
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

    // Debounced search over name and canonical alias; wait for the debounced
    // searchQuery before asserting. Invites are findable, not hidden.
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

// A DM is never scoped by the selected Space (Matrix DMs are not Space
// children), so it shows under All and People inside a Space. The exemption
// belongs to the DM, not to the People chip, or All would show fewer rows
// than People.
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

    // All: the Space's rooms and the DM.
    model.setFilterMode(0);
    QCOMPARE(model.rowCount(), 2);

    // People: the DM, although it is not a child of this Space.
    model.setFilterMode(1);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), RoomListModel::RoomIdRole).toString(), dm.id);

    // Rooms stays scoped: no DM and no room outside the Space.
    model.setFilterMode(2);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), RoomListModel::RoomIdRole).toString(),
             inSpace.id);

    // All is exactly People plus Rooms.
    model.setFilterMode(0);
    QCOMPARE(model.rowCount(), 2);
}

// Filter chips: People/Rooms split on m.direct; Unreads keeps unread rooms,
// the pinned (open) room and, like every mode, invites.
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
    // Out-of-range modes fall back to All in both directions, never
    // edge-snapped.
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

// One fallback-colour policy: an unambiguous 1:1 DM is coloured as the person
// (their MXID); group DMs and plain rooms as the room.
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

// Invitations need action and stay on top; everything joined below them is one
// feed ordered by last activity, DMs and rooms interleaved.

void RoomStateModelTest::invitesStayOnTopAndEverythingElseIsOneActivityFeed()
{
    FakeClient client;
    RoomListModel model;
    auto invite = at(QStringLiteral("!invite:example.org"), false, 9999);
    invite.membership = RoomInfo::Invited;
    const auto newRoom = at(QStringLiteral("!newroom:example.org"), false, 60);
    const auto oldDm   = at(QStringLiteral("!olddm:example.org"), true, 600);
    const auto newDm   = at(QStringLiteral("!newdm:example.org"), true, 30);
    const auto oldRoom = at(QStringLiteral("!oldroom:example.org"), false, 900);
    // Shuffled: the order must come from the sort, not the backend.
    client.mirror = { oldRoom, newDm, invite, oldDm, newRoom };
    model.setClient(&client);

    // The invite is the stalest and still leads. Everything below is by
    // recency, alternating kinds.
    QCOMPARE(orderOf(model),
             (QStringList{ invite.id, newDm.id, newRoom.id, oldDm.id,
                           oldRoom.id }));

    // The section role has two values here, so the presenter cannot draw a
    // second header halfway down an alternating list.
    QStringList categories;
    for (int i = 0; i < model.rowCount(); ++i) {
        categories << model.data(model.index(i), RoomListModel::CategoryRole)
                          .toString();
    }
    QCOMPARE(categories,
             (QStringList{ QStringLiteral("invite"),
                           QStringLiteral("conversation"),
                           QStringLiteral("conversation"),
                           QStringLiteral("conversation"),
                           QStringLiteral("conversation") }));
    QStringList runs;
    for (const QString &category : std::as_const(categories)) {
        if (runs.isEmpty() || runs.constLast() != category)
            runs << category;
    }
    QCOMPARE(runs.size(), 2);
}

void RoomStateModelTest::aNewerRoomOutranksAnOlderDirectMessage()
{
    // A room with a message from 13:05 sorts above a DM whose latest was
    // 12:40.
    FakeClient client;
    RoomListModel model;
    const auto dm = at(QStringLiteral("!dm:example.org"), true, 25 * 60);
    const auto room1305 = at(QStringLiteral("!room:example.org"), false, 0);
    client.mirror = { dm, room1305 };
    model.setClient(&client);
    QCOMPARE(orderOf(model), (QStringList{ room1305.id, dm.id }));
}

void RoomStateModelTest::aNewerDirectMessageOutranksAnOlderRoom()
{
    // The converse, so the fix cannot be "rooms always win".
    FakeClient client;
    RoomListModel model;
    const auto room1305 = at(QStringLiteral("!room:example.org"), false, 60);
    const auto dm1306 = at(QStringLiteral("!dm:example.org"), true, 0);
    client.mirror = { room1305, dm1306 };
    model.setClient(&client);
    QCOMPARE(orderOf(model), (QStringList{ dm1306.id, room1305.id }));
}

// A favourite outranks recency; among favourites, recency still decides.
void RoomStateModelTest::aFavouriteOutranksRecencyButNotEachOther()
{
    FakeClient client;
    RoomListModel model;
    auto staleFavourite = at(QStringLiteral("!fav:example.org"), false, 86400);
    staleFavourite.isFavourite = true;
    const auto fresh = at(QStringLiteral("!fresh:example.org"), false, 10);
    client.mirror = { staleFavourite, fresh };
    model.setClient(&client);

    // The star wins rank even though the other room is a day fresher.
    QCOMPARE(orderOf(model), (QStringList{ staleFavourite.id, fresh.id }));
    QVERIFY(model.data(model.index(0), RoomListModel::IsFavouriteRole).toBool());
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("favourite"));
    QCOMPARE(model.data(model.index(1), RoomListModel::CategoryRole).toString(),
             QStringLiteral("conversation"));

    // Among favourites the more recent comes first, so the list never freezes
    // into the order rooms were starred in.
    auto secondFavourite = at(QStringLiteral("!fav2:example.org"), false, 60);
    secondFavourite.isFavourite = true;
    client.mirror = { staleFavourite, fresh, secondFavourite };
    Q_EMIT client.roomsChanged();
    QCOMPARE(orderOf(model),
             (QStringList{ secondFavourite.id, staleFavourite.id, fresh.id }));
}

void RoomStateModelTest::peopleAndRoomsFiltersAreRecentFirst()
{
    FakeClient client;
    RoomListModel model;
    const auto dmOld = at(QStringLiteral("!dmold:example.org"), true, 900);
    const auto dmNew = at(QStringLiteral("!dmnew:example.org"), true, 30);
    const auto roomOld = at(QStringLiteral("!roomold:example.org"), false, 800);
    const auto roomNew = at(QStringLiteral("!roomnew:example.org"), false, 20);
    client.mirror = { dmOld, roomOld, dmNew, roomNew };
    model.setClient(&client);

    model.setFilterMode(1); // People
    QCOMPARE(orderOf(model), (QStringList{ dmNew.id, dmOld.id }));
    model.setFilterMode(2); // Rooms
    QCOMPARE(orderOf(model), (QStringList{ roomNew.id, roomOld.id }));
}

void RoomStateModelTest::unreadsFilterMixesDirectMessagesAndRoomsByRecency()
{
    FakeClient client;
    RoomListModel model;
    auto dm = at(QStringLiteral("!dm:example.org"), true, 500);
    dm.hasUnreadMessages = true;
    auto roomRow = at(QStringLiteral("!room:example.org"), false, 40);
    roomRow.hasUnreadMessages = true;
    client.mirror = { dm, roomRow };
    model.setClient(&client);
    model.setFilterMode(3); // Unreads
    QCOMPARE(orderOf(model), (QStringList{ roomRow.id, dm.id }));
}

void RoomStateModelTest::incomingActivityMovesOneRowAndKeepsTheSelection()
{
    FakeClient client;
    RoomListModel model;
    const auto a = at(QStringLiteral("!a:example.org"), false, 100);
    const auto b = at(QStringLiteral("!b:example.org"), true, 200);
    const auto c = at(QStringLiteral("!c:example.org"), false, 300);
    client.mirror = { a, b, c };
    model.setClient(&client);
    QCOMPARE(orderOf(model), (QStringList{ a.id, b.id, c.id }));

    // A message in the oldest room moves it rather than resetting the model,
    // which would destroy every delegate and lose the reader's place.
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    client.mirror[2].lastActivity = ago(0);
    Q_EMIT client.roomsChanged();

    QCOMPARE(orderOf(model), (QStringList{ c.id, a.id, b.id }));
    QCOMPARE(reset.count(), 0);
}

void RoomStateModelTest::aSpaceScopedListUsesTheSameRecencyRule()
{
    // Space scoping narrows which rooms are listed, not how they are ordered.
    FakeClient client;
    RoomListModel model;
    SpaceManager spaces;
    auto space = room(QStringLiteral("!space:example.org"));
    space.isSpace = true;
    space.childRoomIds = { QStringLiteral("!inroom:example.org"),
                           QStringLiteral("!inolder:example.org") };
    const auto inOlder = at(QStringLiteral("!inolder:example.org"), false, 900);
    const auto inRoom = at(QStringLiteral("!inroom:example.org"), false, 10);
    client.mirror = { space, inOlder, inRoom };
    spaces.setClient(&client);
    model.setClient(&client);
    model.setSpaceManager(&spaces);
    // Scope comes from the manager's selection, which the rail sets.
    spaces.setActiveSpaceId(space.id);

    QCOMPARE(orderOf(model), (QStringList{ inRoom.id, inOlder.id }));
}

void RoomStateModelTest::roomsWithNoActivityYetSortLastAndDeterministically()
{
    // A room that has never had a message has an invalid timestamp; an
    // explicit rule keeps such rooms from reshuffling between syncs.
    FakeClient client;
    RoomListModel model;
    auto quietB = room(QStringLiteral("!q2:example.org"));
    quietB.name = QStringLiteral("Beta");
    quietB.lastActivity = QDateTime();
    auto quietA = room(QStringLiteral("!q1:example.org"));
    quietA.name = QStringLiteral("Alpha");
    quietA.lastActivity = QDateTime();
    const auto active = at(QStringLiteral("!live:example.org"), false, 3600);
    client.mirror = { quietB, active, quietA };
    model.setClient(&client);

    // The active room leads however stale, then the silent ones by name.
    QCOMPARE(orderOf(model),
             (QStringList{ active.id, quietA.id, quietB.id }));
}

// The favourite tag is account state and is never flipped locally: if the
// server refuses the write, the row would show a favourite the account does
// not have.
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
    // The backend has not confirmed anything, so nothing changed.
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("conversation"));
    QVERIFY(!model.data(model.index(0), RoomListModel::IsFavouriteRole).toBool());

    // Only the backend reflecting the tag back changes the row, moving it into
    // the favourites section.
    client.mirror[0].isFavourite = true;
    Q_EMIT client.roomsChanged();
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("favourite"));
    QVERIFY(model.isRoomFavourite(QStringLiteral("!room:example.org")));

    client.favouritesSupported = false;
    QVERIFY(!model.roomFavouritesSupported());
}

// Every category categoryOf() emits has a section label in the Classic
// presenter; its section delegate falls back to "Rooms", so a missing label
// would mislabel a section rather than fail.
void RoomStateModelTest::everyCategoryTheModelEmitsHasASectionLabel()
{
    QFile file(QStringLiteral(QML_DIR "/RoomListClassicPresenter.qml"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(file.fileName()));
    const QString source = QString::fromUtf8(file.readAll());

    // Three categories: invite, favourite and conversation. DMs and rooms share
    // one section (a split would repeat headers as kinds alternate), while
    // favourites sort above everything and form their own.
    RoomInfo probe;
    probe.membership = RoomInfo::Invited;
    QStringList emitted{ RoomListModel::categoryOf(probe) };
    probe.membership = RoomInfo::Joined;
    // A favourite (DM or room) is "favourite"; a plain DM or room is
    // "conversation".
    probe.isFavourite = true;
    emitted << RoomListModel::categoryOf(probe);
    probe.isDirect = true;
    emitted << RoomListModel::categoryOf(probe);
    probe.isFavourite = false;
    emitted << RoomListModel::categoryOf(probe);
    probe.isDirect = false;
    emitted << RoomListModel::categoryOf(probe);
    QCOMPARE(emitted,
             (QStringList{ QStringLiteral("invite"),
                           QStringLiteral("favourite"),
                           QStringLiteral("favourite"),
                           QStringLiteral("conversation"),
                           QStringLiteral("conversation") }));

    // The invite section is matched by name; the conversation section takes
    // its label from the active filter, so a fallback property is required
    // rather than another `section ===` branch.
    QVERIFY2(source.contains(QStringLiteral("section === \"invite\"")),
             "the Classic presenter has no section label for invites");
    QVERIFY2(source.contains(QStringLiteral("conversationSectionLabel")),
             "the Classic presenter has no label for the conversation "
             "section");
    // Every emitted category needs a label.
    QVERIFY2(source.contains(QStringLiteral("section === \"favourite\"")),
             "the Classic presenter has no section label for favourites");
    // One label per filter mode, so no mode falls through to a wrong name.
    for (const char *label : { "Conversations", "People", "Rooms", "Unread" }) {
        QVERIFY2(source.contains(
                     QStringLiteral("qsTr(\"%1\")").arg(QLatin1String(label))),
                 qPrintable(QStringLiteral("no section label '%1'")
                                .arg(QLatin1String(label))));
    }
}


// The favourites divider binding asks the model (favouritesBoundaryRoomId)
// rather than ListView.section/nextSection, is anchored rather than laid out,
// and is drawn off the selection chip. A source scan: a pure presentation
// binding with no C++ side.
void RoomStateModelTest::theFavouritesSectionIsClosedOffByADivider()
{
    QFile panel(
        QStringLiteral(QML_DIR "/RoomListClassicPresenter.qml"));
    QVERIFY2(panel.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(panel.fileName()));
    const QString panelSource = QString::fromUtf8(panel.readAll());

    const int bindingAt = panelSource.indexOf(QStringLiteral("showGroupDivider:"));
    QVERIFY2(bindingAt >= 0,
             "the Classic presenter never binds showGroupDivider");
    const QString binding = panelSource.mid(bindingAt, 260);
        // The view asks the model: ListView.section / nextSection go stale
        // under reuseItems and row moves.
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
        // Anchored, never laid out: a rule adding height would move every row
        // below it.
    QVERIFY2(!divider.contains(QStringLiteral("Layout.")),
             "the rule must not participate in the row's layout");
        // Drawn off the selection chip: a border on a `selected` fill is nearly
        // invisible.
    QVERIFY2(rowSource.contains(
                 QStringLiteral("anchors.bottomMargin: root.showGroupDivider ? 1 : 0")),
             "the selection chip must yield the row's last pixel line to the rule");
}

void RoomStateModelTest::theFavouritesBoundaryIsRetiredWithTheGroup()
{
    // Favourites form their own group, newest first, followed by everything
    // else; the model reports no divider boundary, because the section header
    // now separates the groups.
    FakeClient client;
    RoomListModel model;
    auto favA = at(QStringLiteral("!favA:example.org"), false, 500);
    favA.isFavourite = true;
    auto favB = at(QStringLiteral("!favB:example.org"), false, 400);
    favB.isFavourite = true;
    const auto plain = at(QStringLiteral("!plain:example.org"), false, 10);
    client.mirror = { favA, favB, plain };
    model.setClient(&client);

    QCOMPARE(model.rowCount(), 3);
    // Favourites lead, newest first among themselves; the plain room follows
    // however fresh.
    QCOMPARE(orderOf(model), (QStringList{ favB.id, favA.id, plain.id }));
    QVERIFY(model.data(model.index(0), RoomListModel::IsFavouriteRole).toBool());
    QVERIFY(model.data(model.index(1), RoomListModel::IsFavouriteRole).toBool());
    QCOMPARE(model.data(model.index(0), RoomListModel::CategoryRole).toString(),
             QStringLiteral("favourite"));
    QCOMPARE(model.data(model.index(1), RoomListModel::CategoryRole).toString(),
             QStringLiteral("favourite"));
    QCOMPARE(model.data(model.index(2), RoomListModel::CategoryRole).toString(),
             QStringLiteral("conversation"));
    // No boundary rule: the section header divides the groups. The property is
    // still bound in QML, so a non-empty value would start ruling a random row.
    QVERIFY2(model.favouritesBoundaryRoomId().isEmpty(),
             "the section header divides the groups, not a boundary rule");

    // Un-starring drops the room out of the group, and it lands by recency.
    for (auto &r : client.mirror) {
        if (r.id == favB.id)
            r.isFavourite = false;
    }
    Q_EMIT client.roomUpdated(favB.id);
    QCoreApplication::processEvents();
    QCOMPARE(orderOf(model), (QStringList{ favA.id, plain.id, favB.id }));
    QCOMPARE(model.data(model.index(2), RoomListModel::CategoryRole).toString(),
             QStringLiteral("conversation"));
    QVERIFY(model.favouritesBoundaryRoomId().isEmpty());
}

void RoomStateModelTest::roomActivityOnlyEverMovesForward()
{
    // Every writer of RoomInfo::lastActivity goes through raiseActivity(), so
    // disagreeing sources cannot make a room jump and fall back.
    RoomInfo room;
    const QDateTime older = QDateTime::fromMSecsSinceEpoch(1'700'000'000'000);
    const QDateTime newer = older.addSecs(600);

    // Nothing known yet: any valid stamp takes.
    QVERIFY(!room.lastActivity.isValid());
    QVERIFY(!room.raiseActivity(QDateTime()));   // invalid is never news
    QVERIFY(!room.lastActivity.isValid());
    QVERIFY(room.raiseActivity(newer));
    QCOMPARE(room.lastActivity, newer);

    // A stale answer (a lagging summary, an unloaded timeline) must not pull
    // the room back down.
    QVERIFY(!room.raiseActivity(older));
    QCOMPARE(room.lastActivity, newer);
    // An identical stamp reports no change.
    QVERIFY(!room.raiseActivity(newer));
    QCOMPARE(room.lastActivity, newer);
    // Genuinely newer still moves.
    const QDateTime newest = newer.addSecs(1);
    QVERIFY(room.raiseActivity(newest));
    QCOMPARE(room.lastActivity, newest);
}

QTEST_GUILESS_MAIN(RoomStateModelTest)
#include "RoomStateModelTest.moc"
