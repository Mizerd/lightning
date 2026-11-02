
#include "app/RoomInfoController.h"
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
    bool moderationSupported = true;
    int memberCalls = 0;
    int kickCalls = 0;
    int banCalls = 0;
    int unbanCalls = 0;
    QString lastModRoom;
    QString lastModUser;
    QString lastModReason;
    quint64 lastOpId = 0;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
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

    bool supportsRoomManagement() const override { return true; }
    quint64 requestRoomMembers(const QString &) override
    {
        ++memberCalls;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 kickUser(const QString &roomId, const QString &userId,
                     const QString &reason) override
    {
        if (!moderationSupported)
            return 0;
        ++kickCalls;
        lastModRoom = roomId;
        lastModUser = userId;
        lastModReason = reason;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 banUser(const QString &roomId, const QString &userId,
                    const QString &reason) override
    {
        if (!moderationSupported)
            return 0;
        ++banCalls;
        lastModRoom = roomId;
        lastModUser = userId;
        lastModReason = reason;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 unbanUser(const QString &roomId, const QString &userId,
                      const QString &reason) override
    {
        if (!moderationSupported)
            return 0;
        ++unbanCalls;
        lastModRoom = roomId;
        lastModUser = userId;
        lastModReason = reason;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 inviteUsers(const QString &roomId,
                        const QStringList &userIds) override
    {
        ++inviteCalls;
        lastInviteRoom = roomId;
        lastInvitees = userIds;
        lastInviteOpId = nextOp++;
        lastOpId = lastInviteOpId;
        return lastInviteOpId;
    }
    int inviteCalls = 0;
    QString lastInviteRoom;
    QStringList lastInvitees;
    quint64 lastInviteOpId = 0;

    quint64 leaveRoom(const QString &roomId) override
    {
        ++leaveCalls;
        lastLeaveRoom = roomId;
        return leaveRefused ? 0 : nextOp++;
    }
    bool leaveRefused = false;
    int leaveCalls = 0;
    QString lastLeaveRoom;
};

QVariantMap memberRow(const QString &userId, qlonglong powerLevel,
                      bool isOwn = false,
                      const QString &membership = QStringLiteral("joined"))
{
    QVariantMap member;
    member.insert(QStringLiteral("userId"), userId);
    member.insert(QStringLiteral("displayName"),
                  userId.mid(1, userId.indexOf(QLatin1Char(':')) - 1));
    member.insert(QStringLiteral("membership"), membership);
    member.insert(QStringLiteral("role"), QStringLiteral("user"));
    member.insert(QStringLiteral("powerLevel"), powerLevel);
    member.insert(QStringLiteral("isOwn"), isOwn);
    return member;
}

QVariantMap snapshotWithMembers(bool canKick, bool canBan, qlonglong ownPl,
                                const QVariantList &members,
                                bool canUnban)
{
    QVariantMap snapshot;
    snapshot.insert(QStringLiteral("ok"), true);
    snapshot.insert(QStringLiteral("joinedCount"), members.size());
    snapshot.insert(QStringLiteral("invitedCount"), 0);
    snapshot.insert(QStringLiteral("truncated"), false);
    snapshot.insert(QStringLiteral("canInvite"), false);
    snapshot.insert(QStringLiteral("canEditName"), false);
    snapshot.insert(QStringLiteral("canEditTopic"), false);
    snapshot.insert(QStringLiteral("canEditAvatar"), false);
    snapshot.insert(QStringLiteral("canKick"), canKick);
    snapshot.insert(QStringLiteral("canBan"), canBan);
    snapshot.insert(QStringLiteral("canUnban"), canUnban);
    snapshot.insert(QStringLiteral("ownPowerLevel"), ownPl);
    snapshot.insert(QStringLiteral("members"), members);
    return snapshot;
}

QVariantMap snapshotWithMembers(bool canKick, bool canBan, qlonglong ownPl,
                                const QVariantList &members)
{
    return snapshotWithMembers(canKick, canBan, ownPl, members,
                               canKick && canBan);
}

QVariantMap snapshotWith(bool canKick, bool canBan, qlonglong ownPl)
{
    return snapshotWithMembers(
        canKick, canBan, ownPl,
        QVariantList{ memberRow(QStringLiteral("@bob:example.org"), 0) });
}

}

class RoomInfoModerationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    void snapshotCarriesModerationPermissions()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        QCOMPARE(client.memberCalls, 1);
        QVERIFY(!controller.canKick());
        QVERIFY(!controller.canBan());

        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWith(true, true, 100));
        QVERIFY(controller.canKick());
        QVERIFY(controller.canBan());
        QCOMPARE(controller.ownPowerLevel(), qlonglong(100));
        const QVariantMap row = controller.members().value(0).toMap();
        QCOMPARE(row.value(QStringLiteral("powerLevel")).toLongLong(),
                 qlonglong(0));
    }

    void canModerateEnforcesPowerLevelPolicy()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        const QVariantList members = {
            memberRow(QStringLiteral("@me:example.org"), 100,true),
            memberRow(QStringLiteral("@below:example.org"), 50),
            memberRow(QStringLiteral("@equal:example.org"), 100),
            memberRow(QStringLiteral("@above:example.org"), 200),
            memberRow(QStringLiteral("@restricted:example.org"), -1),
        };
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, QStringLiteral("!room:example.org"),
            snapshotWithMembers(true, true, 100, members));

        QVERIFY(controller.canModerate(QStringLiteral("@below:example.org"),
                                       QStringLiteral("kick")));
        QVERIFY(controller.canModerate(
            QStringLiteral("@restricted:example.org"), QStringLiteral("ban")));
        QVERIFY(!controller.canModerate(QStringLiteral("@equal:example.org"),
                                        QStringLiteral("kick")));
        QVERIFY(!controller.canModerate(QStringLiteral("@above:example.org"),
                                        QStringLiteral("ban")));
        QVERIFY(!controller.canModerate(QStringLiteral("@me:example.org"),
                                        QStringLiteral("kick")));
        QVERIFY(!controller.canModerate(
            QStringLiteral("@stranger:example.org"), QStringLiteral("kick")));
        QVERIFY(!controller.canModerate(QStringLiteral("@below:example.org"),
                                        QStringLiteral("smite")));

        controller.kickMember(QStringLiteral("@equal:example.org"), {});
        QCOMPARE(client.kickCalls, 0);
        QVERIFY(!controller.moderationPending());
    }

    void unbanIsOfferedOnlyForBannedMembers()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        const QVariantList members = {
            memberRow(QStringLiteral("@me:example.org"), 100,true),
            memberRow(QStringLiteral("@joined:example.org"), 0),
            memberRow(QStringLiteral("@banned:example.org"), 0,
false, QStringLiteral("banned")),
            memberRow(QStringLiteral("@bannedadmin:example.org"), 100,
false, QStringLiteral("banned")),
        };
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, QStringLiteral("!room:example.org"),
            snapshotWithMembers(true, true, 100, members));

        QVERIFY(controller.canModerate(QStringLiteral("@banned:example.org"),
                                       QStringLiteral("unban")));
        QVERIFY(!controller.canModerate(QStringLiteral("@banned:example.org"),
                                        QStringLiteral("kick")));
        QVERIFY(!controller.canModerate(QStringLiteral("@banned:example.org"),
                                        QStringLiteral("ban")));
        QVERIFY(!controller.canModerate(QStringLiteral("@joined:example.org"),
                                        QStringLiteral("unban")));
        QVERIFY(!controller.canModerate(
            QStringLiteral("@bannedadmin:example.org"),
            QStringLiteral("unban")));

        controller.unbanMember(QStringLiteral("@banned:example.org"),
                               QStringLiteral("appeal accepted"));
        QCOMPARE(client.unbanCalls, 1);
        QCOMPARE(client.kickCalls, 0);
        QCOMPARE(client.banCalls, 0);
        QCOMPARE(client.lastModUser, QStringLiteral("@banned:example.org"));
        QCOMPARE(client.lastModReason, QStringLiteral("appeal accepted"));
        QVERIFY(controller.moderationPending());
    }

    void unbanHasItsOwnPermissionFlag()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        const QVariantList members = {
            memberRow(QStringLiteral("@banned:example.org"), 0,
false, QStringLiteral("banned")),
        };
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, QStringLiteral("!room:example.org"),
            snapshotWithMembers(false, true, 100, members,
false));
        QVERIFY(!controller.canModerate(QStringLiteral("@banned:example.org"),
                                        QStringLiteral("unban")));
        controller.unbanMember(QStringLiteral("@banned:example.org"), {});
        QCOMPARE(client.unbanCalls, 0);
        QVERIFY(!controller.moderationPending());
    }

    void unbanInviteBackFollowsSuccessOnly()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        const QVariantList members = {
            memberRow(QStringLiteral("@banned:example.org"), 0,
false, QStringLiteral("banned")),
        };
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, QStringLiteral("!room:example.org"),
            snapshotWithMembers(true, true, 100, members));

        controller.unbanMember(QStringLiteral("@banned:example.org"), {},
true);
        QCOMPARE(client.unbanCalls, 1);
        Q_EMIT client.moderationFinished(client.lastOpId,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@banned:example.org"),
                                         QStringLiteral("unban"), false,
                                         QStringLiteral("forbidden"));
        QCOMPARE(client.inviteCalls, 0);

        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWithMembers(true, true, 100,
                                                              members));
        controller.unbanMember(QStringLiteral("@banned:example.org"), {},
true);
        const quint64 unbanOp = client.lastOpId;
        QSignalSpy done(&controller,
                        &RoomInfoController::moderationActionFinished);
        Q_EMIT client.moderationFinished(unbanOp,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@banned:example.org"),
                                         QStringLiteral("unban"), true, {});
        QCOMPARE(client.inviteCalls, 1);
        QCOMPARE(client.lastInvitees,
                 QStringList{ QStringLiteral("@banned:example.org") });
        Q_EMIT client.inviteUserFinished(client.lastInviteOpId,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@banned:example.org"),
                                         true, {});
        bool sawInviteBack = false;
        for (const auto &args : done) {
            if (args.at(2).toString() == QLatin1String("invite_back")) {
                sawInviteBack = true;
                QCOMPARE(args.at(3).toBool(), true);
            }
        }
        QVERIFY(sawInviteBack);

        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWithMembers(true, true, 100,
                                                              members));
        controller.unbanMember(QStringLiteral("@banned:example.org"), {},
false);
        Q_EMIT client.moderationFinished(client.lastOpId,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@banned:example.org"),
                                         QStringLiteral("unban"), true, {});
        QCOMPARE(client.inviteCalls, 1);
    }

    void successfulActionRefreshesRoster()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWith(true, true, 100));
        QCOMPARE(client.memberCalls, 1);

        controller.kickMember(QStringLiteral("@bob:example.org"), {});
        const quint64 kickOp = client.lastOpId;
        Q_EMIT client.moderationFinished(kickOp,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@bob:example.org"),
                                         QStringLiteral("kick"), true, {});
        QCOMPARE(client.memberCalls, 2);

        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWith(true, true, 100));
        controller.banMember(QStringLiteral("@bob:example.org"), {});
        Q_EMIT client.moderationFinished(client.lastOpId,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@bob:example.org"),
                                         QStringLiteral("ban"), false,
                                         QStringLiteral("forbidden"));
        QCOMPARE(client.memberCalls, 2);
    }

    void synchronousRejectionReportsFailure()
    {
        FakeClient client;
        client.moderationSupported = false;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWith(true, true, 100));
        QSignalSpy done(&controller,
                        &RoomInfoController::moderationActionFinished);
        controller.kickMember(QStringLiteral("@bob:example.org"), {});
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.first().at(3).toBool(), false);
        QVERIFY(!done.first().at(4).toString().isEmpty());
        QVERIFY(!controller.moderationPending());
    }

    void kickIsGatedOnSnapshotFlag()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWith(false, false, 0));
        controller.kickMember(QStringLiteral("@bob:example.org"), {});
        controller.banMember(QStringLiteral("@bob:example.org"), {});
        QCOMPARE(client.kickCalls, 0);
        QCOMPARE(client.banCalls, 0);
        QVERIFY(!controller.moderationPending());
    }

    void kickSendsAndReportsSuccess()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWith(true, true, 100));

        QSignalSpy done(&controller,
                        &RoomInfoController::moderationActionFinished);
        controller.kickMember(QStringLiteral("@bob:example.org"),
                              QStringLiteral("spam"));
        QCOMPARE(client.kickCalls, 1);
        QCOMPARE(client.lastModRoom, QStringLiteral("!room:example.org"));
        QCOMPARE(client.lastModUser, QStringLiteral("@bob:example.org"));
        QCOMPARE(client.lastModReason, QStringLiteral("spam"));
        QVERIFY(controller.moderationPending());

        controller.banMember(QStringLiteral("@bob:example.org"), {});
        QCOMPARE(client.banCalls, 0);

        Q_EMIT client.moderationFinished(client.lastOpId,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@bob:example.org"),
                                         QStringLiteral("kick"), true, {});
        QVERIFY(!controller.moderationPending());
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.first().at(3).toBool(), true);
        QVERIFY(done.first().at(4).toString().isEmpty());
    }

    void forbiddenFailureMapsToPermissionMessage()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWith(true, true, 100));

        QSignalSpy done(&controller,
                        &RoomInfoController::moderationActionFinished);
        controller.banMember(QStringLiteral("@bob:example.org"), {});
        QCOMPARE(client.banCalls, 1);
        Q_EMIT client.moderationFinished(client.lastOpId,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@bob:example.org"),
                                         QStringLiteral("ban"), false,
                                         QStringLiteral("forbidden"));
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.first().at(3).toBool(), false);
        QVERIFY(done.first().at(4).toString().contains(
            QStringLiteral("permission")));
    }

    void aRefusedLeaveIsReportedNotSwallowed()
    {
        FakeClient client;
        client.leaveRefused = true;
        RoomInfoController controller;
        controller.setClient(&client);

        QSignalSpy failed(&controller, &RoomInfoController::roomLeaveFailed);
        QSignalSpy left(&controller, &RoomInfoController::roomLeft);
        controller.leaveRoom(QStringLiteral("!invite:example.org"));
        QCOMPARE(client.leaveCalls, 1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(),
                 QStringLiteral("!invite:example.org"));
        QVERIFY(!failed.at(0).at(1).toString().isEmpty());
        QCOMPARE(left.count(), 0);

        controller.setRoomId(QStringLiteral("!room:example.org"));
        controller.leaveRoom();
        QCOMPARE(client.leaveCalls, 2);
        QVERIFY(!controller.leavePending());
        QVERIFY(!controller.leaveError().isEmpty());
    }

    void anAcceptedLeaveWaitsForItsResult()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);

        QSignalSpy failed(&controller, &RoomInfoController::roomLeaveFailed);
        QSignalSpy left(&controller, &RoomInfoController::roomLeft);
        const quint64 op = client.nextOp;
        controller.leaveRoom(QStringLiteral("!invite:example.org"));
        QCOMPARE(client.leaveCalls, 1);
        QCOMPARE(failed.count(), 0);
        Q_EMIT client.roomLeaveFinished(op, QStringLiteral("!invite:example.org"),
                                        true, QString());
        QCOMPARE(left.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    void roomSwitchDropsLateResult()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!room:example.org"),
                                          snapshotWith(true, true, 100));
        controller.kickMember(QStringLiteral("@bob:example.org"), {});
        const quint64 modOp = client.lastOpId;
        QVERIFY(controller.moderationPending());

        controller.setRoomId(QStringLiteral("!other:example.org"));
        QVERIFY(!controller.moderationPending());

        QSignalSpy done(&controller,
                        &RoomInfoController::moderationActionFinished);
        Q_EMIT client.moderationFinished(modOp,
                                         QStringLiteral("!room:example.org"),
                                         QStringLiteral("@bob:example.org"),
                                         QStringLiteral("kick"), true, {});
        QCOMPARE(done.count(), 0);
    }

    void aMembershipPokeDuringAFetchIsFetchedAfterIt()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        controller.setRoomId(room);
        QCOMPARE(client.memberCalls, 1);
        const quint64 inFlight = client.lastOpId;

        Q_EMIT client.roomMemberEventSeen(room);
        QCOMPARE(client.memberCalls, 1);

        QVariantMap partial = snapshotWith(true, true, 100);
        partial.insert(QStringLiteral("partial"), true);
        Q_EMIT client.roomMembersReceived(inFlight, room, partial);
        QCoreApplication::processEvents();
        QCOMPARE(client.memberCalls, 1);

        Q_EMIT client.roomMembersReceived(inFlight, room,
                                          snapshotWith(true, true, 100));
        QTRY_COMPARE(client.memberCalls, 2);
        QVERIFY(controller.loading());

        Q_EMIT client.roomMembersReceived(client.lastOpId, room,
                                          snapshotWith(true, true, 100));
        QCoreApplication::processEvents();
        QCOMPARE(client.memberCalls, 2);
        QVERIFY(!controller.loading());
    }

    void anOwedFetchDoesNotFollowARoomSwitch()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!a:example.org"));
        Q_EMIT client.roomMemberEventSeen(QStringLiteral("!a:example.org"));
        controller.setRoomId(QStringLiteral("!b:example.org"));
        QCOMPARE(client.memberCalls, 2);
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!b:example.org"),
                                          snapshotWith(true, true, 100));
        QCoreApplication::processEvents();
        QCOMPARE(client.memberCalls, 2);
    }

    void sendAndRedactPermissionsComeFromTheSnapshot()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        controller.setRoomId(room);
        QVERIFY(controller.canSendMessages());
        QVERIFY(controller.sendBlockedReason().isEmpty());
        QVERIFY(!controller.canRedactOthers());
        QVERIFY(controller.ownMembership().isEmpty());

        QVariantMap muted = snapshotWith(false, false, 0);
        muted.insert(QStringLiteral("canSendMessages"), false);
        muted.insert(QStringLiteral("canRedactOthers"), false);
        muted.insert(QStringLiteral("ownMembership"), QStringLiteral("joined"));
        Q_EMIT client.roomMembersReceived(client.lastOpId, room, muted);
        QVERIFY(!controller.canSendMessages());
        QCOMPARE(controller.sendBlockedReason(),
                 RoomInfoController::tr(
                     "You do not have permission to post in this room."));
        QCOMPARE(controller.ownMembership(), QStringLiteral("joined"));

        controller.refreshMembers();
        QVariantMap moderator = snapshotWith(true, true, 50);
        moderator.insert(QStringLiteral("canSendMessages"), true);
        moderator.insert(QStringLiteral("canRedactOthers"), true);
        moderator.insert(QStringLiteral("ownMembership"),
                         QStringLiteral("joined"));
        Q_EMIT client.roomMembersReceived(client.lastOpId, room, moderator);
        QVERIFY(controller.canSendMessages());
        QVERIFY(controller.sendBlockedReason().isEmpty());
        QVERIFY(controller.canRedactOthers());

        controller.refreshMembers();
        Q_EMIT client.roomMembersReceived(client.lastOpId, room,
                                          snapshotWith(true, true, 50));
        QVERIFY(controller.canSendMessages());
        QVERIFY(!controller.canRedactOthers());
        QVERIFY(controller.ownMembership().isEmpty());
    }

    void aNotJoinedAnswerBlocksSendingAndWithdrawsModeration()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        controller.setRoomId(room);
        QVariantMap joined = snapshotWith(true, true, 100);
        joined.insert(QStringLiteral("canRedactOthers"), true);
        joined.insert(QStringLiteral("ownMembership"), QStringLiteral("joined"));
        joined.insert(QStringLiteral("canChangeHistoryVisibility"), true);
        joined.insert(QStringLiteral("canChangeGuestAccess"), true);
        joined.insert(QStringLiteral("canPublishCallMembership"), true);
        Q_EMIT client.roomMembersReceived(client.lastOpId, room, joined);
        QVERIFY(controller.canKick());
        QVERIFY(controller.canRedactOthers());
        QVERIFY(controller.canChangeHistoryVisibility());
        QVERIFY(controller.canChangeGuestAccess());
        QVERIFY(controller.canPublishCallMembership());

        Q_EMIT client.roomMemberEventSeen(room);
        QCOMPARE(client.memberCalls, 2);
        QVariantMap removed;
        removed.insert(QStringLiteral("ok"), false);
        removed.insert(QStringLiteral("category"), QStringLiteral("not_joined"));
        removed.insert(QStringLiteral("ownMembership"), QStringLiteral("left"));
        Q_EMIT client.roomMembersReceived(client.lastOpId, room, removed);

        QCOMPARE(controller.ownMembership(), QStringLiteral("left"));
        QVERIFY(!controller.canSendMessages());
        QCOMPARE(controller.sendBlockedReason(),
                 RoomInfoController::tr("You are not in this room."));
        QVERIFY(!controller.canKick());
        QVERIFY(!controller.canBan());
        QVERIFY(!controller.canRedactOthers());
        QVERIFY(!controller.canModerate(QStringLiteral("@bob:example.org"),
                                        QStringLiteral("kick")));
        QVERIFY(!controller.canChangeHistoryVisibility());
        QVERIFY(!controller.canChangeGuestAccess());
        QVERIFY(!controller.canPublishCallMembership());

        controller.setRoomId(QStringLiteral("!other:example.org"));
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!other:example.org"),
                                          joined);
        controller.refreshMembers();
        QVariantMap offline;
        offline.insert(QStringLiteral("ok"), false);
        offline.insert(QStringLiteral("category"), QStringLiteral("network"));
        Q_EMIT client.roomMembersReceived(client.lastOpId,
                                          QStringLiteral("!other:example.org"),
                                          offline);
        QVERIFY(controller.canSendMessages());
        QVERIFY(controller.canKick());
        QCOMPARE(controller.ownMembership(), QStringLiteral("joined"));
    }
};

QTEST_GUILESS_MAIN(RoomInfoModerationTest)
#include "RoomInfoModerationTest.moc"
