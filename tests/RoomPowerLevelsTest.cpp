// v0.7.x room administration through RoomInfoController — member power
// levels, join rule and canonical alias. Pins:
//   * the extended snapshot fields (canChangePowerLevels / canPinMessages /
//     canChangeJoinRule / canChangeAlias / usersDefaultPowerLevel / joinRule
//     / canonicalAlias) land on the controller;
//   * canSetPowerLevel enforces the Matrix rules the server will apply —
//     never grant above your own level, never act on a peer at or above it,
//     self-demotion only — and fails closed for an unknown target;
//   * arbitrary CUSTOM numeric levels survive: they are neither rounded into
//     a preset nor relabelled as one;
//   * the write is not optimistic — the authoritative roster is re-read
//     afterwards, on success and on rejection alike;
//   * the join rule accepts only the three rules that carry no allow-rule
//     list, and a no-op is not sent;
//   * a bare alias localpart is completed with the account's own server, and
//     an empty alias (clearing it) is a legitimate value.
//
// HONEST SCOPE: policy and wiring only. Real m.room.power_levels /
// m.room.join_rules / m.room.canonical_alias round trips against a homeserver
// and Element interoperability are NOT exercised here and are NOT TESTED.

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
    int memberCalls = 0;
    int powerCalls = 0;
    int joinRuleCalls = 0;
    int aliasCalls = 0;
    bool refuseWrites = false;
    QString lastPowerUser;
    qlonglong lastPowerLevel = 0;
    QString lastJoinRule;
    QString lastAlias;
    quint64 lastOpId = 0;
    quint64 lastPowerOpId = 0;

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

    bool supportsRoomManagement() const override { return true; }
    quint64 requestRoomMembers(const QString &) override
    {
        ++memberCalls;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 setMemberPowerLevel(const QString &, const QString &userId,
                                qlonglong level) override
    {
        if (refuseWrites)
            return 0;
        ++powerCalls;
        lastPowerUser = userId;
        lastPowerLevel = level;
        lastPowerOpId = nextOp++;
        lastOpId = lastPowerOpId;
        return lastPowerOpId;
    }
    quint64 setRoomJoinRule(const QString &, const QString &rule) override
    {
        if (refuseWrites)
            return 0;
        ++joinRuleCalls;
        lastJoinRule = rule;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 setRoomCanonicalAlias(const QString &, const QString &alias) override
    {
        if (refuseWrites)
            return 0;
        ++aliasCalls;
        lastAlias = alias;
        lastOpId = nextOp++;
        return lastOpId;
    }
};

QVariantMap memberRow(const QString &userId, qlonglong powerLevel,
                      bool isOwn = false)
{
    QVariantMap member;
    member.insert(QStringLiteral("userId"), userId);
    member.insert(QStringLiteral("displayName"), userId);
    member.insert(QStringLiteral("membership"), QStringLiteral("joined"));
    member.insert(QStringLiteral("role"), QStringLiteral("user"));
    member.insert(QStringLiteral("powerLevel"), powerLevel);
    member.insert(QStringLiteral("isOwn"), isOwn);
    return member;
}

QVariantMap adminSnapshot(qlonglong ownPl, const QVariantList &members,
                          bool canChangePl = true,
                          qlonglong usersDefault = 0,
                          const QString &joinRule = QStringLiteral("invite"),
                          const QString &alias = QString())
{
    QVariantMap s;
    s.insert(QStringLiteral("ok"), true);
    s.insert(QStringLiteral("joinedCount"), int(members.size()));
    s.insert(QStringLiteral("invitedCount"), 0);
    s.insert(QStringLiteral("truncated"), false);
    s.insert(QStringLiteral("canInvite"), true);
    s.insert(QStringLiteral("canEditName"), true);
    s.insert(QStringLiteral("canEditTopic"), true);
    s.insert(QStringLiteral("canEditAvatar"), true);
    s.insert(QStringLiteral("canKick"), true);
    s.insert(QStringLiteral("canBan"), true);
    s.insert(QStringLiteral("canUnban"), true);
    s.insert(QStringLiteral("ownPowerLevel"), ownPl);
    s.insert(QStringLiteral("canChangePowerLevels"), canChangePl);
    s.insert(QStringLiteral("canPinMessages"), true);
    s.insert(QStringLiteral("canChangeJoinRule"), true);
    s.insert(QStringLiteral("canChangeAlias"), true);
    s.insert(QStringLiteral("canManageSpaceChildren"), true);
    s.insert(QStringLiteral("usersDefaultPowerLevel"), usersDefault);
    s.insert(QStringLiteral("joinRule"), joinRule);
    s.insert(QStringLiteral("canonicalAlias"), alias);
    s.insert(QStringLiteral("members"), members);
    return s;
}

const QString kRoom = QStringLiteral("!room:example.org");
const QString kMe = QStringLiteral("@me:example.org");
const QString kBob = QStringLiteral("@bob:example.org");
const QString kMod = QStringLiteral("@mod:example.org");
const QString kPeer = QStringLiteral("@peer:example.org");

} // namespace

class RoomPowerLevelsTest : public QObject
{
    Q_OBJECT

private:
    // Own level 100 (admin), Bob 0, a moderator at 50, a peer at 100.
    static void seed(RoomInfoController &ctl, FakeClient &client,
                     qlonglong ownPl = 100, bool canChangePl = true,
                     qlonglong usersDefault = 0)
    {
        ctl.setRoomId(kRoom);
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, kRoom,
            adminSnapshot(ownPl,
                          { memberRow(kMe, ownPl, /*isOwn=*/true),
                            memberRow(kBob, usersDefault),
                            memberRow(kMod, 50),
                            memberRow(kPeer, 100) },
                          canChangePl, usersDefault));
    }

private Q_SLOTS:
    void snapshotCarriesTheAdministrationFields()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, kRoom,
            adminSnapshot(100, { memberRow(kMe, 100, true) }, true,
                          /*usersDefault=*/5, QStringLiteral("knock"),
                          QStringLiteral("#room:example.org")));

        QVERIFY(ctl.canChangePowerLevels());
        QVERIFY(ctl.canPinMessages());
        QVERIFY(ctl.canChangeJoinRule());
        QVERIFY(ctl.canChangeAlias());
        QVERIFY(ctl.canManageSpaceChildren());
        QCOMPARE(ctl.usersDefaultPowerLevel(), 5);
        QCOMPARE(ctl.joinRule(), QStringLiteral("knock"));
        QCOMPARE(ctl.canonicalAlias(), QStringLiteral("#room:example.org"));
    }

    void canSetPowerLevelAppliesTheMatrixRules()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client, /*ownPl=*/100);

        // Promote a member below you, up to your own level: allowed.
        QVERIFY(ctl.canSetPowerLevel(kBob, 50));
        QVERIFY(ctl.canSetPowerLevel(kBob, 100));
        // Above your own level: you cannot grant authority you lack.
        QVERIFY(!ctl.canSetPowerLevel(kBob, 101));
        // A peer AT your level is not yours to change.
        QVERIFY(!ctl.canSetPowerLevel(kPeer, 0));
        // A no-op is not an action worth offering.
        QVERIFY(!ctl.canSetPowerLevel(kMod, 50));
        // Unknown target fails closed — levels may legitimately be
        // negative, so no sentinel can stand in for "unknown".
        QVERIFY(!ctl.canSetPowerLevel(QStringLiteral("@ghost:example.org"), 50));
        QVERIFY(!ctl.canSetPowerLevel(QString(), 50));
    }

    void selfChangeIsDemotionOnly()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client, /*ownPl=*/100);

        QVERIFY(ctl.canSetPowerLevel(kMe, 50));
        QVERIFY(ctl.canSetPowerLevel(kMe, 0));
        // Nothing in Matrix lets a user raise their own level.
        QVERIFY(!ctl.canSetPowerLevel(kMe, 100));
    }

    void withoutStatePermissionNothingIsOfferedOrSent()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client, /*ownPl=*/100, /*canChangePl=*/false);

        QVERIFY(!ctl.canChangePowerLevels());
        QVERIFY(!ctl.canSetPowerLevel(kBob, 50));
        ctl.setMemberPowerLevel(kBob, 50);
        QCOMPARE(client.powerCalls, 0);
    }

    void customNumericLevelsAreNeverFlattened()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, kRoom,
            adminSnapshot(100,
                          { memberRow(kMe, 100, true),
                            memberRow(kBob, 42),
                            // Element's "Restricted" is a real, negative,
                            // legitimate level.
                            memberRow(kMod, -1) }));

        QCOMPARE(ctl.powerLevelFor(kBob), 42);
        QCOMPARE(ctl.powerLevelFor(kMod), -1);
        // The label must show the number, never round 42 into "Moderator".
        const QString label = ctl.roleLabelForLevel(42);
        QVERIFY2(label.contains(QStringLiteral("42")),
                 "an unconventional level must render as its number");
        QVERIFY(ctl.roleLabelForLevel(-1).contains(QStringLiteral("-1")));
        QCOMPARE(ctl.roleLabelForLevel(100), QStringLiteral("Administrator"));
        QCOMPARE(ctl.roleLabelForLevel(50), QStringLiteral("Moderator"));
        QCOMPARE(ctl.roleLabelForLevel(0), QStringLiteral("Member"));
    }

    void memberRoleLabelFollowsTheRoomsOwnDefault()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        // A room whose users_default is 10: a member sitting at 10 IS an
        // ordinary member there, not a custom level.
        seed(ctl, client, /*ownPl=*/100, /*canChangePl=*/true,
             /*usersDefault=*/10);
        QCOMPARE(ctl.usersDefaultPowerLevel(), 10);
        QCOMPARE(ctl.roleLabelForLevel(10), QStringLiteral("Member"));
        QCOMPARE(ctl.powerLevelFor(kBob), 10);
    }

    void writeIsNotOptimisticAndRefetchesEitherWay()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        const int membersBefore = client.memberCalls;

        QSignalSpy finished(&ctl,
                            &RoomInfoController::powerLevelActionFinished);
        ctl.setMemberPowerLevel(kBob, 50);
        QCOMPARE(client.powerCalls, 1);
        QCOMPARE(client.lastPowerUser, kBob);
        QCOMPARE(client.lastPowerLevel, 50);
        QVERIFY(ctl.powerLevelPending());
        // The snapshot has NOT changed: nothing is applied locally.
        QCOMPARE(ctl.powerLevelFor(kBob), 0);
        // One in flight at a time.
        QVERIFY(!ctl.canSetPowerLevel(kMod, 0));

        Q_EMIT client.powerLevelChangeFinished(client.lastPowerOpId, kRoom,
                                               kBob, 50, true, QString());
        QVERIFY(!ctl.powerLevelPending());
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(3).toBool());          // ok
        // Authoritative roster re-read rather than a local write.
        QCOMPARE(client.memberCalls, membersBefore + 1);
    }

    void rejectionReportsAndStillRefetches()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        const int membersBefore = client.memberCalls;

        QSignalSpy finished(&ctl,
                            &RoomInfoController::powerLevelActionFinished);
        ctl.setMemberPowerLevel(kBob, 50);
        Q_EMIT client.powerLevelChangeFinished(client.lastPowerOpId, kRoom,
                                               kBob, 50, false,
                                               QStringLiteral("forbidden"));

        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.at(0).at(3).toBool());         // ok
        QVERIFY(!finished.at(0).at(4).toString().isEmpty()); // message
        // The UI must not be left showing an optimistic value.
        QCOMPARE(ctl.powerLevelFor(kBob), 0);
        QCOMPARE(client.memberCalls, membersBefore + 1);
    }

    void staleAnswerAfterRoomSwitchIsIgnored()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        ctl.setMemberPowerLevel(kBob, 50);
        const quint64 staleOp = client.lastPowerOpId;

        ctl.setRoomId(QStringLiteral("!other:example.org"));
        QSignalSpy finished(&ctl,
                            &RoomInfoController::powerLevelActionFinished);
        Q_EMIT client.powerLevelChangeFinished(staleOp, kRoom, kBob, 50, true,
                                               QString());
        QCOMPARE(finished.size(), 0);
    }

    void joinRuleAcceptsOnlyTheConfigurationFreeRules()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QCOMPARE(ctl.joinRule(), QStringLiteral("invite"));

        // Restricted carries an allow-rule list this surface cannot build;
        // sending it with an empty list would silently lock the room to
        // invite-only while claiming otherwise.
        ctl.setJoinRule(QStringLiteral("restricted"));
        ctl.setJoinRule(QStringLiteral("nonsense"));
        QCOMPARE(client.joinRuleCalls, 0);

        // A no-op is not sent either.
        ctl.setJoinRule(QStringLiteral("invite"));
        QCOMPARE(client.joinRuleCalls, 0);

        ctl.setJoinRule(QStringLiteral("public"));
        QCOMPARE(client.joinRuleCalls, 1);
        QCOMPARE(client.lastJoinRule, QStringLiteral("public"));
    }

    void joinRuleRefreshesTheRosterOnlyOnSuccess()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        // Join rule rides the MEMBER snapshot, and nothing refetches that
        // for a state change which is not a membership change — so the
        // successful write must ask for it explicitly.
        ctl.setJoinRule(QStringLiteral("public"));
        int before = client.memberCalls;
        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("join_rule"), true,
                                       QString());
        QCOMPARE(client.memberCalls, before + 1);

        // A failed write changed nothing, so there is nothing to re-read.
        ctl.setJoinRule(QStringLiteral("knock"));
        before = client.memberCalls;
        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("join_rule"), false,
                                       QStringLiteral("forbidden"));
        QCOMPARE(client.memberCalls, before);
        QVERIFY(!ctl.editError().isEmpty());
    }

    void aliasCompletesABareLocalpartAndAcceptsClearing()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        // Typing "#name:server" by hand is not something to require.
        ctl.setCanonicalAlias(QStringLiteral("lounge"));
        QCOMPARE(client.aliasCalls, 1);
        QCOMPARE(client.lastAlias, QStringLiteral("#lounge:example.org"));

        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("canonical_alias"), true,
                                       QString());
        // A fully-qualified alias is passed through untouched.
        ctl.setCanonicalAlias(QStringLiteral("#other:elsewhere.example"));
        QCOMPARE(client.aliasCalls, 2);
        QCOMPARE(client.lastAlias, QStringLiteral("#other:elsewhere.example"));

        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("canonical_alias"), true,
                                       QString());
        // Clearing is a real value, not an empty-input no-op — but only when
        // there IS an alias to clear.
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, kRoom,
            adminSnapshot(100, { memberRow(kMe, 100, true) }, true, 0,
                          QStringLiteral("invite"),
                          QStringLiteral("#lounge:example.org")));
        ctl.setCanonicalAlias(QString());
        QCOMPARE(client.aliasCalls, 3);
        QCOMPARE(client.lastAlias, QString());
    }
};

QTEST_MAIN(RoomPowerLevelsTest)
#include "RoomPowerLevelsTest.moc"
