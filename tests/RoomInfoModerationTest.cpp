// 2026-08-14: moderation (kick / ban) through RoomInfoController — snapshot
// permission ingestion (canKick/canBan/ownPowerLevel + per-member power
// levels), gating on the SDK-derived flags, one-in-flight discipline,
// sanitized result mapping, and stale-result rejection after a room switch.

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

    // MatrixClient pure virtuals (inert).
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
    // Default: the common configuration where unban is granted alongside
    // ban (kick level <= ban level, so max(ban, kick) == ban).
    return snapshotWithMembers(canKick, canBan, ownPl, members,
                               canKick && canBan);
}

QVariantMap snapshotWith(bool canKick, bool canBan, qlonglong ownPl)
{
    return snapshotWithMembers(
        canKick, canBan, ownPl,
        QVariantList{ memberRow(QStringLiteral("@bob:example.org"), 0) });
}

} // namespace

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

    // Review M3: the offer/dispatch policy lives in the controller, not
    // QML — equal power, higher power, self, an unknown target and a
    // NEGATIVE-but-known power level must all resolve here.
    void canModerateEnforcesPowerLevelPolicy()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        const QVariantList members = {
            memberRow(QStringLiteral("@me:example.org"), 100, /*isOwn=*/true),
            memberRow(QStringLiteral("@below:example.org"), 50),
            memberRow(QStringLiteral("@equal:example.org"), 100),
            memberRow(QStringLiteral("@above:example.org"), 200),
            // Element's "Restricted" role: a legal NEGATIVE level, which
            // must read as known-and-below, never as "unknown".
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
        // Unknown target (no snapshot row) fails closed.
        QVERIFY(!controller.canModerate(
            QStringLiteral("@stranger:example.org"), QStringLiteral("kick")));
        // Unknown op fails closed.
        QVERIFY(!controller.canModerate(QStringLiteral("@below:example.org"),
                                        QStringLiteral("smite")));

        // The dispatch path re-checks the same policy: an equal-power kick
        // never reaches the client even though canKick is true.
        controller.kickMember(QStringLiteral("@equal:example.org"), {});
        QCOMPARE(client.kickCalls, 0);
        QVERIFY(!controller.moderationPending());
    }

    // Unban is membership-aware: only banned members can be unbanned, and
    // banned members can be neither kicked nor re-banned. Unban rides the
    // ban power flag.
    void unbanIsOfferedOnlyForBannedMembers()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        const QVariantList members = {
            memberRow(QStringLiteral("@me:example.org"), 100, /*isOwn=*/true),
            memberRow(QStringLiteral("@joined:example.org"), 0),
            memberRow(QStringLiteral("@banned:example.org"), 0,
                      /*isOwn=*/false, QStringLiteral("banned")),
            memberRow(QStringLiteral("@bannedadmin:example.org"), 100,
                      /*isOwn=*/false, QStringLiteral("banned")),
        };
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, QStringLiteral("!room:example.org"),
            snapshotWithMembers(true, true, 100, members));

        QVERIFY(controller.canModerate(QStringLiteral("@banned:example.org"),
                                       QStringLiteral("unban")));
        // A banned member is already out: no kick, no second ban.
        QVERIFY(!controller.canModerate(QStringLiteral("@banned:example.org"),
                                        QStringLiteral("kick")));
        QVERIFY(!controller.canModerate(QStringLiteral("@banned:example.org"),
                                        QStringLiteral("ban")));
        // A joined member cannot be "unbanned".
        QVERIFY(!controller.canModerate(QStringLiteral("@joined:example.org"),
                                        QStringLiteral("unban")));
        // The strictly-below rule applies to unban too.
        QVERIFY(!controller.canModerate(
            QStringLiteral("@bannedadmin:example.org"),
            QStringLiteral("unban")));

        // Dispatch reaches the client's unban call.
        controller.unbanMember(QStringLiteral("@banned:example.org"),
                               QStringLiteral("appeal accepted"));
        QCOMPARE(client.unbanCalls, 1);
        QCOMPARE(client.kickCalls, 0);
        QCOMPARE(client.banCalls, 0);
        QCOMPARE(client.lastModUser, QStringLiteral("@banned:example.org"));
        QCOMPARE(client.lastModReason, QStringLiteral("appeal accepted"));
        QVERIFY(controller.moderationPending());
    }

    // Review MU1: unban's required level is max(ban, kick) — a room with
    // kick above ban grants can_ban WITHOUT can_unban, and the client
    // must not offer an unban the server will reject. The flag comes
    // from the SDK's PowerLevelAction::Unban helper, never derived from
    // the ban flag.
    void unbanHasItsOwnPermissionFlag()
    {
        FakeClient client;
        RoomInfoController controller;
        controller.setClient(&client);
        controller.setRoomId(QStringLiteral("!room:example.org"));
        const QVariantList members = {
            memberRow(QStringLiteral("@banned:example.org"), 0,
                      /*isOwn=*/false, QStringLiteral("banned")),
        };
        // canBan true, canUnban false — the kick-above-ban configuration.
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, QStringLiteral("!room:example.org"),
            snapshotWithMembers(false, true, 100, members,
                                /*canUnban=*/false));
        QVERIFY(!controller.canModerate(QStringLiteral("@banned:example.org"),
                                        QStringLiteral("unban")));
        controller.unbanMember(QStringLiteral("@banned:example.org"), {});
        QCOMPARE(client.unbanCalls, 0);
        QVERIFY(!controller.moderationPending());
    }

    // Review M2: the roster refresh after a successful action is
    // CLIENT-initiated — sync never emits a members snapshot by itself.
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

        // A FAILED action does not refetch.
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

    // Review L2: a synchronous dispatch rejection (backend returns op 0)
    // reports honestly instead of arming the confirm surface forever.
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

        // A second action while one is pending is refused.
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
};

QTEST_GUILESS_MAIN(RoomInfoModerationTest)
#include "RoomInfoModerationTest.moc"
