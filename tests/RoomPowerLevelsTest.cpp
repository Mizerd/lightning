
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
    int restrictedCalls = 0;
    QStringList lastAllowed;
    QString lastHistoryVisibility;
    QString lastGuestAccess;
    QStringList lastAltAliases;
    int directoryRequests = 0;
    int lastDirectoryPublished = -1;
    quint64 setRoomJoinRule(const QString &, const QString &rule,
                            const QStringList &allowedRoomIds) override
    {
        if (refuseWrites)
            return 0;
        ++restrictedCalls;
        lastJoinRule = rule;
        lastAllowed = allowedRoomIds;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 setRoomHistoryVisibility(const QString &,
                                     const QString &visibility) override
    {
        if (refuseWrites)
            return 0;
        lastHistoryVisibility = visibility;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 setRoomGuestAccess(const QString &, const QString &access) override
    {
        if (refuseWrites)
            return 0;
        lastGuestAccess = access;
        lastOpId = nextOp++;
        return lastOpId;
    }
    void requestRoomDirectoryVisibility(const QString &) override
    {
        ++directoryRequests;
    }
    quint64 setRoomDirectoryVisibility(const QString &, bool published) override
    {
        if (refuseWrites)
            return 0;
        lastDirectoryPublished = published ? 1 : 0;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 setRoomAltAliases(const QString &, const QStringList &aliases) override
    {
        if (refuseWrites)
            return 0;
        lastAltAliases = aliases;
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
    s.insert(QStringLiteral("canChangeHistoryVisibility"), true);
    s.insert(QStringLiteral("canChangeGuestAccess"), true);
    s.insert(QStringLiteral("historyVisibility"), QStringLiteral("shared"));
    s.insert(QStringLiteral("guestAccess"), QStringLiteral("forbidden"));
    s.insert(QStringLiteral("altAliases"), QStringList());
    s.insert(QStringLiteral("restrictedAllowedRooms"), QStringList());
    s.insert(QStringLiteral("restrictedHasUnknownRules"), false);
    s.insert(QStringLiteral("members"), members);
    return s;
}

const QString kRoom = QStringLiteral("!room:example.org");
const QString kMe = QStringLiteral("@me:example.org");
const QString kBob = QStringLiteral("@bob:example.org");
const QString kMod = QStringLiteral("@mod:example.org");
const QString kPeer = QStringLiteral("@peer:example.org");

}

class RoomPowerLevelsTest : public QObject
{
    Q_OBJECT

private:
    static void seed(RoomInfoController &ctl, FakeClient &client,
                     qlonglong ownPl = 100, bool canChangePl = true,
                     qlonglong usersDefault = 0)
    {
        ctl.setRoomId(kRoom);
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, kRoom,
            adminSnapshot(ownPl,
                          { memberRow(kMe, ownPl,true),
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
5, QStringLiteral("knock"),
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
        seed(ctl, client,100);

        QVERIFY(ctl.canSetPowerLevel(kBob, 50));
        QVERIFY(ctl.canSetPowerLevel(kBob, 100));
        QVERIFY(!ctl.canSetPowerLevel(kBob, 101));
        QVERIFY(!ctl.canSetPowerLevel(kPeer, 0));
        QVERIFY(!ctl.canSetPowerLevel(kMod, 50));
        QVERIFY(!ctl.canSetPowerLevel(QStringLiteral("@ghost:example.org"), 50));
        QVERIFY(!ctl.canSetPowerLevel(QString(), 50));
    }

    void selfChangeIsDemotionOnly()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client,100);

        QVERIFY(ctl.canSetPowerLevel(kMe, 50));
        QVERIFY(ctl.canSetPowerLevel(kMe, 0));
        QVERIFY(!ctl.canSetPowerLevel(kMe, 100));
    }

    void withoutStatePermissionNothingIsOfferedOrSent()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client,100,false);

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
                            memberRow(kMod, -1) }));

        QCOMPARE(ctl.powerLevelFor(kBob), 42);
        QCOMPARE(ctl.powerLevelFor(kMod), -1);
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
        seed(ctl, client,100,true,
10);
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
        QCOMPARE(ctl.powerLevelFor(kBob), 0);
        QVERIFY(!ctl.canSetPowerLevel(kMod, 0));

        Q_EMIT client.powerLevelChangeFinished(client.lastPowerOpId, kRoom,
                                               kBob, 50, true, QString());
        QVERIFY(!ctl.powerLevelPending());
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.at(0).at(3).toBool());
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
        QVERIFY(!finished.at(0).at(3).toBool());
        QVERIFY(!finished.at(0).at(4).toString().isEmpty());
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

        ctl.setJoinRule(QStringLiteral("restricted"));
        ctl.setJoinRule(QStringLiteral("nonsense"));
        QCOMPARE(client.joinRuleCalls, 0);

        ctl.setJoinRule(QStringLiteral("invite"));
        QCOMPARE(client.joinRuleCalls, 0);

        ctl.setJoinRule(QStringLiteral("public"));
        QCOMPARE(client.joinRuleCalls, 1);
        QCOMPARE(client.lastJoinRule, QStringLiteral("public"));
    }

    void restrictedJoinRuleCarriesTheAllowListAndRefusesAnEmptyOne()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        ctl.setRestrictedJoinRule(QStringLiteral("restricted"), {});
        QCOMPARE(client.restrictedCalls, 0);
        QVERIFY(!ctl.editError().isEmpty());
        QVERIFY(!ctl.editPending());

        ctl.setRestrictedJoinRule(
            QStringLiteral("knock_restricted"),
            { QStringLiteral("!space:example.org"), QStringLiteral("junk"),
              QStringLiteral("!space:example.org") });
        QCOMPARE(client.restrictedCalls, 1);
        QCOMPARE(client.lastJoinRule, QStringLiteral("knock_restricted"));
        QCOMPARE(client.lastAllowed,
                 QStringList{ QStringLiteral("!space:example.org") });
        QVERIFY(ctl.editPending());
        const int before = client.memberCalls;
        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("join_rule"), true,
                                       QString());
        QCOMPARE(client.memberCalls, before + 1);
    }

    void historyVisibilityAndGuestAccessAreGatedAndValidated()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QCOMPARE(ctl.historyVisibility(), QStringLiteral("shared"));
        QCOMPARE(ctl.guestAccess(), QStringLiteral("forbidden"));

        ctl.setHistoryVisibility(QStringLiteral("everyone"));
        QVERIFY(client.lastHistoryVisibility.isEmpty());
        ctl.setHistoryVisibility(QStringLiteral("shared"));
        QVERIFY(client.lastHistoryVisibility.isEmpty());
        ctl.setHistoryVisibility(QStringLiteral("world_readable"));
        QCOMPARE(client.lastHistoryVisibility, QStringLiteral("world_readable"));
        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("history_visibility"),
                                       true, QString());
        QVERIFY(!ctl.editPending());

        ctl.setGuestAccess(QStringLiteral("can_join"));
        QCOMPARE(client.lastGuestAccess, QStringLiteral("can_join"));

        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("guest_access"), true,
                                       QString());
        QVariantMap gated = adminSnapshot(100, { memberRow(kMe, 100, true) },
                                          true);
        gated.insert(QStringLiteral("canChangeGuestAccess"), false);
        Q_EMIT client.roomMembersReceived(client.lastOpId, kRoom, gated);
        QVERIFY(!ctl.canChangeGuestAccess());
        client.lastGuestAccess.clear();
        ctl.setGuestAccess(QStringLiteral("can_join"));
        QVERIFY(client.lastGuestAccess.isEmpty());
    }

    void directoryVisibilityIsATriStateReadFromTheServer()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QCOMPARE(ctl.directoryPublished(), -1);

        ctl.requestDirectoryVisibility();
        QCOMPARE(client.directoryRequests, 1);
        Q_EMIT client.roomDirectoryVisibilityReceived(kRoom, true, true);
        QCOMPARE(ctl.directoryPublished(), 1);
        Q_EMIT client.roomDirectoryVisibilityReceived(
            QStringLiteral("!other:example.org"), true, false);
        QCOMPARE(ctl.directoryPublished(), 1);
        Q_EMIT client.roomDirectoryVisibilityReceived(kRoom, false, false);
        QCOMPARE(ctl.directoryPublished(), -1);

        Q_EMIT client.roomDirectoryVisibilityReceived(kRoom, true, false);
        ctl.setDirectoryPublished(true);
        QCOMPARE(client.lastDirectoryPublished, 1);
        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("directory_visibility"),
                                       true, QString());
        QCOMPARE(client.directoryRequests, 2);

        ctl.setRoomId(QStringLiteral("!other:example.org"));
        QCOMPARE(ctl.directoryPublished(), -1);
    }

    void altAliasesAreCompletedDedupedAndSentAsOneList()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        ctl.setAltAliases({ QStringLiteral("lounge"),
                            QStringLiteral("#lounge:example.org"),
                            QStringLiteral("#other:example.org") });
        QCOMPARE(client.lastAltAliases,
                 (QStringList{ QStringLiteral("#lounge:example.org"),
                               QStringLiteral("#other:example.org") }));
        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("alt_aliases"), true,
                                       QString());
        QVERIFY(!ctl.editPending());
    }

    void joinRuleRefreshesTheRosterOnlyOnSuccess()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        ctl.setJoinRule(QStringLiteral("public"));
        int before = client.memberCalls;
        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("join_rule"), true,
                                       QString());
        QCOMPARE(client.memberCalls, before + 1);

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

        ctl.setCanonicalAlias(QStringLiteral("lounge"));
        QCOMPARE(client.aliasCalls, 1);
        QCOMPARE(client.lastAlias, QStringLiteral("#lounge:example.org"));

        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("canonical_alias"), true,
                                       QString());
        ctl.setCanonicalAlias(QStringLiteral("#other:elsewhere.example"));
        QCOMPARE(client.aliasCalls, 2);
        QCOMPARE(client.lastAlias, QStringLiteral("#other:elsewhere.example"));

        Q_EMIT client.roomEditFinished(client.lastOpId, kRoom,
                                       QStringLiteral("canonical_alias"), true,
                                       QString());
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, kRoom,
            adminSnapshot(100, { memberRow(kMe, 100, true) }, true, 0,
                          QStringLiteral("invite"),
                          QStringLiteral("#lounge:example.org")));
        ctl.setCanonicalAlias(QString());
        QCOMPARE(client.aliasCalls, 3);
        QCOMPARE(client.lastAlias, QString());
    }

    void anAddressThisClientWouldNotAcceptIsNeverSent()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        ctl.setCanonicalAlias(QStringLiteral("#bad name:localhs.test"));
        QCOMPARE(client.aliasCalls, 0);
        QVERIFY(!ctl.editPending());
        QVERIFY2(ctl.editError().contains(QStringLiteral("spaces")),
                 qPrintable(ctl.editError()));
        QCOMPARE(ctl.aliasProblem(QStringLiteral("#bad name:localhs.test")),
                 ctl.editError());

        const QStringList refused{
            QStringLiteral("bad name"),
            QStringLiteral("a#b"),
            QStringLiteral("a,b"),
            QStringLiteral("tab\there"),
            QStringLiteral("zero") + QChar(0x200B) + QStringLiteral("width"),
            QStringLiteral("rlo") + QChar(0x202E) + QStringLiteral("x"),
            QStringLiteral("#:example.org"),
            QStringLiteral("#lounge:"),
            QStringLiteral("#") + QString(260, QLatin1Char('a'))
                + QStringLiteral(":example.org"),
        };
        for (const QString &bad : refused) {
            ctl.setCanonicalAlias(bad);
            QVERIFY2(client.aliasCalls == 0, qPrintable(bad));
            QVERIFY2(!ctl.aliasProblem(bad).isEmpty(), qPrintable(bad));
        }
        ctl.setAltAliases({ QStringLiteral("#bad name:example.org") });
        QVERIFY(client.lastAltAliases.isEmpty());
        QVERIFY(!ctl.editPending());

        for (const QString &good :
             { QStringLiteral("lounge"), QStringLiteral("#lounge:example.org"),
               QStringLiteral("#lounge:example.org:8448"),
               QStringLiteral("\u0161altibar\u0161\u010diai"),
               QString() })
            QVERIFY2(ctl.aliasProblem(good).isEmpty(), qPrintable(good));
        ctl.setCanonicalAlias(QStringLiteral("lounge"));
        QCOMPARE(client.aliasCalls, 1);
        QCOMPARE(client.lastAlias, QStringLiteral("#lounge:example.org"));
    }

    void anExistingLooseAlternativeAddressDoesNotBlockTheList()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kRoom);
        QVariantMap snapshot = adminSnapshot(100, { memberRow(kMe, 100, true) });
        const QString loose = QStringLiteral("#old one:example.org");
        snapshot.insert(QStringLiteral("altAliases"), QStringList{ loose });
        Q_EMIT client.roomMembersReceived(client.lastOpId, kRoom, snapshot);
        QCOMPARE(ctl.altAliases(), QStringList{ loose });

        ctl.setAltAliases({ loose, QStringLiteral("extra") });
        QCOMPARE(client.lastAltAliases,
                 (QStringList{ loose, QStringLiteral("#extra:example.org") }));
    }
};

QTEST_MAIN(RoomPowerLevelsTest)
#include "RoomPowerLevelsTest.moc"
