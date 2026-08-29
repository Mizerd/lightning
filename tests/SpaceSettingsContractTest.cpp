// Space settings — the Space Home management card AND, since 2026-08-26, the
// full Space settings dialog (qml/SpaceSettingsDialog.qml) plus the
// RoomInfoController policy behind its new Permissions matrix and Members
// facets.
//
// A SPACE IS A MATRIX ROOM. Editing its name, topic and avatar is the SAME
// permission-gated room-edit backend (`RoomInfoController::setRoomName` /
// `setRoomTopic` / `setRoomAvatar` / `removeRoomAvatar`) that Room Information
// uses, and the new permission matrix is nothing but the room's own
// `m.room.power_levels`. Lightning invents no Space-specific storage and no
// Space-specific permission model.
//
// TWO KINDS OF TEST LIVE HERE, deliberately.
//   * Source contracts (the QML halves) — like ContextMenuContractTest. They
//     prove a control exists, is wired to the shared backend, and carries the
//     right permission gate. They instantiate nothing.
//   * Real controller tests (the policy halves) — they drive
//     RoomInfoController against a fake MatrixClient and prove what it will
//     and will not dispatch.
//
// HONEST SCOPE: no homeserver is contacted. A real m.room.power_levels round
// trip for a Space, and Element interoperability of anything written here, is
// NOT TESTED.

#include "app/RoomInfoController.h"
#include "matrix/MatrixClient.h"

#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <QFile>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    quint64 lastOpId = 0;
    quint64 lastMatrixOpId = 0;
    int matrixCalls = 0;
    bool refuseWrites = false;
    QString lastMatrixKey;
    qlonglong lastMatrixLevel = 0;

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
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 setRoomPowerLevelKey(const QString &, const QString &key,
                                 qlonglong level) override
    {
        if (refuseWrites)
            return 0;
        ++matrixCalls;
        lastMatrixKey = key;
        lastMatrixLevel = level;
        lastMatrixOpId = nextOp++;
        lastOpId = lastMatrixOpId;
        return lastMatrixOpId;
    }
};

QString readQml(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR "/") + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                          : QString{};
}

QVariantMap memberRow(const QString &userId, const QString &displayName,
                      qlonglong powerLevel, const QString &membership,
                      bool isOwn = false)
{
    QVariantMap member;
    member.insert(QStringLiteral("userId"), userId);
    member.insert(QStringLiteral("displayName"), displayName);
    member.insert(QStringLiteral("membership"), membership);
    member.insert(QStringLiteral("powerLevel"), powerLevel);
    member.insert(QStringLiteral("isOwn"), isOwn);
    return member;
}

// The seven scalar thresholds plus the four state-event rows, as the Rust
// snapshot emits them.
QVariantMap levels(qlonglong usersDefault = 0, qlonglong powerLevelsKey = 100)
{
    QVariantMap m;
    m.insert(QStringLiteral("ban"), qlonglong(50));
    m.insert(QStringLiteral("invite"), qlonglong(0));
    m.insert(QStringLiteral("kick"), qlonglong(50));
    m.insert(QStringLiteral("redact"), qlonglong(50));
    m.insert(QStringLiteral("events_default"), qlonglong(0));
    m.insert(QStringLiteral("state_default"), qlonglong(50));
    m.insert(QStringLiteral("users_default"), usersDefault);
    m.insert(QStringLiteral("m.space.child"), qlonglong(50));
    m.insert(QStringLiteral("m.room.name"), qlonglong(50));
    m.insert(QStringLiteral("m.room.avatar"), qlonglong(50));
    m.insert(QStringLiteral("m.room.topic"), qlonglong(50));
    m.insert(QStringLiteral("m.room.join_rules"), qlonglong(50));
    m.insert(QStringLiteral("m.room.canonical_alias"), qlonglong(50));
    m.insert(QStringLiteral("m.room.power_levels"), powerLevelsKey);
    m.insert(QStringLiteral("m.room.tombstone"), qlonglong(100));
    return m;
}

QVariantMap snapshot(qlonglong ownPl, const QVariantList &members,
                     bool canChangePl = true, qlonglong usersDefault = 0,
                     const QVariantMap &powerLevels = levels(),
                     const QString &version = QStringLiteral("10"))
{
    QVariantMap s;
    s.insert(QStringLiteral("ok"), true);
    s.insert(QStringLiteral("joinedCount"), int(members.size()));
    s.insert(QStringLiteral("invitedCount"), 0);
    s.insert(QStringLiteral("truncated"), false);
    s.insert(QStringLiteral("ownPowerLevel"), ownPl);
    s.insert(QStringLiteral("canChangePowerLevels"), canChangePl);
    s.insert(QStringLiteral("usersDefaultPowerLevel"), usersDefault);
    s.insert(QStringLiteral("joinRule"), QStringLiteral("invite"));
    s.insert(QStringLiteral("powerLevels"), powerLevels);
    s.insert(QStringLiteral("roomVersion"), version);
    s.insert(QStringLiteral("canUpgradeRoom"), true);
    s.insert(QStringLiteral("members"), members);
    return s;
}

const QString kSpace = QStringLiteral("!space:example.org");
const QString kMe = QStringLiteral("@me:example.org");
const QString kZoe = QStringLiteral("@zoe:example.org");
const QString kAmy = QStringLiteral("@amy:example.org");
const QString kBan = QStringLiteral("@ban:example.org");

} // namespace

class SpaceSettingsContractTest : public QObject
{
    Q_OBJECT

    // The Space settings card inside Space Home, bounded so an assertion meant
    // for it cannot match some other part of a 4000-line file.
    static QString spaceSettingsBlock(const QString &pane)
    {
        const int start = pane.indexOf(QStringLiteral("id: settingsCol"));
        if (start < 0)
            return {};
        const int end = pane.indexOf(QStringLiteral("ROOMS AND SPACES"), start);
        if (end < 0)
            return {};
        return pane.mid(start, end - start);
    }

    // Own level 100, an ordinary member, a member sitting on a CUSTOM 42, and
    // a banned member — the four cases the Members page has to render.
    static void seed(RoomInfoController &ctl, FakeClient &client,
                     qlonglong ownPl = 100, bool canChangePl = true,
                     qlonglong usersDefault = 0,
                     const QVariantMap &powerLevels = levels())
    {
        ctl.setRoomId(kSpace);
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, kSpace,
            snapshot(ownPl,
                     { memberRow(kMe, QStringLiteral("Me"), ownPl,
                                 QStringLiteral("joined"), /*isOwn=*/true),
                       memberRow(kZoe, QStringLiteral("Zoe"), usersDefault,
                                 QStringLiteral("joined")),
                       memberRow(kAmy, QStringLiteral("Amy"), 42,
                                 QStringLiteral("invited")),
                       memberRow(kBan, QStringLiteral("Banned Bob"), usersDefault,
                                 QStringLiteral("banned")) },
                     canChangePl, usersDefault, powerLevels));
    }

private Q_SLOTS:
    // ---- Space Home card (pre-existing contract, unchanged) ------------

    void spaceHomeExposesAllThreeEdits()
    {
        const QString block =
            spaceSettingsBlock(readQml(QStringLiteral("TimelinePane.qml")));
        QVERIFY2(!block.isEmpty(), "the Space settings card is missing");
        QVERIFY(block.contains(QStringLiteral("objectName: \"spaceNameEditField\"")));
        QVERIFY(block.contains(QStringLiteral("objectName: \"spaceTopicEditField\"")));
        QVERIFY2(block.contains(QStringLiteral("objectName: \"spaceChangeAvatarButton\"")),
                 "a Space could be renamed but never given an avatar");
        QVERIFY(block.contains(QStringLiteral("objectName: \"spaceRemoveAvatarButton\"")));
    }

    void everyEditUsesTheSharedRoomBackend()
    {
        const QString pane = readQml(QStringLiteral("TimelinePane.qml"));
        const QString block = spaceSettingsBlock(pane);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.setRoomName(")));
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.setRoomTopic(")));
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.removeRoomAvatar()")));
        QVERIFY(pane.contains(QStringLiteral("id: spaceAvatarDialog")));
        // The picker now hands its result to the shared crop dialog and the
        // upload happens on its way OUT, so the chosen file is no longer
        // uploaded verbatim. Both halves are asserted: the picker must reach
        // the cropper, and the cropper must still reach the shared room
        // backend — which is what this case is about. The wiring itself is
        // pinned in full by `image-crop-contract`.
        QVERIFY(pane.contains(QStringLiteral("onAccepted: spaceAvatarCrop.openFor(selectedFile)")));
        QVERIFY(pane.contains(QStringLiteral("id: spaceAvatarCrop")));
        QVERIFY(pane.contains(QStringLiteral("app.roomInfo.setRoomAvatar(file)")));
    }

    void editsAreGatedOnTheRealPerEventPermission()
    {
        const QString block =
            spaceSettingsBlock(readQml(QStringLiteral("TimelinePane.qml")));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.canEditName")));
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.canEditTopic")));
        QVERIFY2(block.contains(QStringLiteral("app.roomInfo.canEditAvatar")),
                 "the avatar controls must be gated on the avatar permission, "
                 "not on the name or topic one");
        QVERIFY(block.contains(QStringLiteral("!app.roomInfo.editPending")));
    }

    void editFailuresAreDisclosed()
    {
        const QString block =
            spaceSettingsBlock(readQml(QStringLiteral("TimelinePane.qml")));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("app.roomInfo.editError")));
    }

    void avatarControlsCarryAccessibleNames()
    {
        const QString block =
            spaceSettingsBlock(readQml(QStringLiteral("TimelinePane.qml")));
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("Change the Space avatar")));
        QVERIFY(block.contains(QStringLiteral("Remove the Space avatar")));
    }

    // ---- 2026-08-26: the dialog's new surfaces (source contract) --------

    // On the unfixed tree the dialog had no permission matrix at all, so every
    // needle below is absent and this reports "the Permissions page lost …".
    void permissionMatrixCoversEveryKeyTheBackendAccepts()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        // Each row must name a key the controller and the Rust edge accept —
        // a key neither knows produces a control that silently does nothing.
        const QStringList keys = RoomInfoController::powerLevelKeys();
        for (const QString &key : keys) {
            QVERIFY2(dialog.contains(QStringLiteral("key: \"%1\"").arg(key)),
                     qPrintable(QStringLiteral("the Permissions page lost %1")
                                    .arg(key)));
        }
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.setPowerLevelKey(")));
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.canSetPowerLevelKey(")));
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.powerLevelForKey(")));
    }

    // `m.call.member` would govern neither the identifier Lightning sends nor
    // the stable one; a row for it is worse than no row. This fails the moment
    // somebody adds it back without resolving that.
    void theMatrixOffersNoCallMemberRow()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY2(!dialog.contains(QStringLiteral("key: \"m.call.member\"")),
                 "a Start & Join Calls row governs an event type Lightning "
                 "does not send under that name");
        QVERIFY(!RoomInfoController::powerLevelKeys().contains(
            QStringLiteral("m.call.member")));
    }

    // The combo is an explicit MIRROR. `currentIndex: indexOfValue(…)` is the
    // shape that made the GIF settings look reset every launch: indexOfValue()
    // is -1 at creation time and clamping it to 0 makes the control lie about
    // the room.
    void thresholdCombosSnapBackRatherThanBind()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("levelCombo.syncToValue(")));
        // Scanned WITHOUT comments. This ban matched the dialog's own
        // explanatory comment warning against the very shape it forbids —
        // a ban assertion that fires on prose measures nothing about the
        // code, and it fired here on the first run.
        QString code = dialog;
        code.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        code.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                       QRegularExpression::DotMatchesEverythingOption));
        QVERIFY2(!code.contains(QStringLiteral("currentIndex: indexOfValue")),
                 "a threshold combo bound to indexOfValue() shows row 0 while "
                 "the space holds something else");
        // The stripper must actually strip: a scan that silently removes
        // everything would make the ban above vacuously true.
        QVERIFY2(code.contains(QStringLiteral("levelCombo.syncToValue(")),
                 "the comment stripper ate the code, so the ban is vacuous");
        QVERIFY2(dialog.contains(QStringLiteral("function onRosterTickChanged()")),
                 "nothing snaps the combo back after a rejected write");
    }

    // Every binding that CALLS a controller method must read the tick, or it
    // never re-evaluates: a method call creates no property dependency. The
    // member list was bound to the search text alone before this round and so
    // did not refresh when the roster changed.
    void invokableBackedBindingsReadTheRosterTick()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("property int rosterTick: 0")));
        QVERIFY(dialog.contains(QStringLiteral("root.rosterTick++")));
        QVERIFY2(dialog.contains(QStringLiteral("var _t = root.rosterTick")),
                 "no binding reads the tick, so none of them re-evaluate");
    }

    // The Members page gained the four facets Sable shows, and — the one that
    // matters for honesty — a notice when the roster is capped. A 34k-member
    // space used to show an honest 34156 above 500 rows and say nothing.
    void membersPageCarriesCountFiltersAndTheTruncationNotice()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("spaceSettingsMemberFilter")));
        QVERIFY(dialog.contains(QStringLiteral("spaceSettingsMembershipCombo")));
        QVERIFY(dialog.contains(QStringLiteral("spaceSettingsSortCombo")));
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.memberRoleGroups(")));
        QVERIFY2(dialog.contains(QStringLiteral("spaceSettingsMemberTruncationNotice")),
                 "the member count describes a population the list does not "
                 "contain, and nothing says so");
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.truncated")));
    }

    // The banner is a REAL state event with its OWN required level. Gating it
    // on canEditAvatar would be a guess dressed as a permission.
    void bannerUsesTheBannerBackendAndItsOwnPermission()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("app.banners.setRoomBanner(")));
        QVERIFY(dialog.contains(QStringLiteral("app.banners.clearRoomBanner(")));
        QVERIFY2(dialog.contains(QStringLiteral("app.banners.canSetRoomBanner(")),
                 "the banner controls are gated on some other event's level");
    }

    // Developer tools: every row copyable, through the established hidden
    // TextEdit relay rather than a new C++ clipboard surface.
    void developerToolsCanCopyEveryRow()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("id: devCopyHelper")));
        QVERIFY(dialog.contains(QStringLiteral("devCopyHelper.copy()")));
        QVERIFY(dialog.contains(QStringLiteral("spaceSettingsDevCopy")));
    }

    // Still no local storage anywhere in this file, and no upgrade button:
    // an upgrade is irreversible and orphans every m.space.child edge.
    void thePageStillWritesMatrixStateAndNothingElse()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(!dialog.contains(QStringLiteral("app.settings")));
        QVERIFY(!dialog.contains(QStringLiteral("app.railLayout")));
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.roomVersion")));
        QVERIFY2(!dialog.contains(QStringLiteral("upgradeRoom(")),
                 "an upgrade is irreversible and orphans every m.space.child "
                 "edge; it must not be a button until it is built properly");
    }

    // ---- 2026-08-26: the controller policy (real behaviour) ------------

    // On the unfixed tree powerLevels/roomVersion/canUpgradeRoom do not exist,
    // so this does not compile — which is the point: the matrix had NO source
    // of truth on the C++ side, read or write.
    void snapshotCarriesTheRoomsRealThresholds()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QCOMPARE(ctl.powerLevelForKey(QStringLiteral("ban")), qlonglong(50));
        QCOMPARE(ctl.powerLevelForKey(QStringLiteral("invite")), qlonglong(0));
        QCOMPARE(ctl.powerLevelForKey(QStringLiteral("m.space.child")), qlonglong(50));
        QCOMPARE(ctl.roomVersion(), QStringLiteral("10"));
        QVERIFY(ctl.canUpgradeRoom());
    }

    // An ABSENT key is UNKNOWN, never 0 — a threshold of 0 is a real and very
    // permissive configuration, so a defaulted answer would claim the space
    // requires nothing. Unknown also FAILS CLOSED for the write.
    void anAbsentThresholdIsUnknownAndFailsClosed()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        QVariantMap partial = levels();
        partial.remove(QStringLiteral("ban"));
        seed(ctl, client, 100, true, 0, partial);
        QVERIFY(!ctl.powerLevelKnown(QStringLiteral("ban")));
        QCOMPARE(ctl.powerLevelForKey(QStringLiteral("ban")), qlonglong(-1));
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"), 0));
        ctl.setPowerLevelKey(QStringLiteral("ban"), 0);
        QCOMPARE(client.matrixCalls, 0);
    }

    // THE ONE-WAY DOOR. Requiring more than you have for m.room.power_levels
    // locks you out of the only key that would undo it, and no server will
    // help. On the unfixed tree there is no gate at all.
    void aThresholdAboveYourOwnLevelIsNeverOffered()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client, /*ownPl=*/50);
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("m.room.power_levels"),
                                         100));
        QVERIFY(ctl.canSetPowerLevelKey(QStringLiteral("m.room.power_levels"),
                                        50));
        ctl.setPowerLevelKey(QStringLiteral("m.room.power_levels"), 100);
        QCOMPARE(client.matrixCalls, 0);
        // …and the same clause is what stops users_default being raised above
        // the person raising it, which is how a space accidentally hands
        // everyone moderator rights.
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("users_default"), 100));
    }

    void aKeyOutsideTheAllowlistIsNeverDispatched()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("m.room.encryption"),
                                         50));
        ctl.setPowerLevelKey(QStringLiteral("m.room.encryption"), 50);
        QCOMPARE(client.matrixCalls, 0);
    }

    void writesAreBoundedAndNoOpsAreNotOffered()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        // Already 50.
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"), 50));
        // Outside the settable band. DISPLAY is unbounded; only the write is
        // bounded, which also keeps the MSC4289 creator sentinel out.
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"),
                                         RoomInfoController::kMaxSettableLevel + 1));
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"),
                                         RoomInfoController::kMinSettableLevel - 1));
        QVERIFY(ctl.canSetPowerLevelKey(QStringLiteral("ban"), 0));
    }

    // Nothing is applied optimistically: the roster is re-read on success AND
    // on rejection, so a refused write cannot leave a value the space does not
    // have.
    void aRefusedThresholdWriteRereadsTheRoster()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QSignalSpy done(&ctl, &RoomInfoController::powerMatrixActionFinished);
        ctl.setPowerLevelKey(QStringLiteral("ban"), 0);
        QCOMPARE(client.matrixCalls, 1);
        QCOMPARE(client.lastMatrixKey, QStringLiteral("ban"));
        QVERIFY(ctl.powerMatrixPending());
        // A second write is refused while one is in flight.
        ctl.setPowerLevelKey(QStringLiteral("kick"), 0);
        QCOMPARE(client.matrixCalls, 1);

        const quint64 before = client.lastOpId;
        Q_EMIT client.roomPowerMatrixFinished(client.lastMatrixOpId, kSpace,
                                              QStringLiteral("ban"), 0, false,
                                              QStringLiteral("forbidden"));
        QCOMPARE(done.size(), 1);
        // powerMatrixActionFinished(roomId, key, level, ok, message)
        QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("ban"));
        QCOMPARE(done.at(0).at(3).toBool(), false);
        QVERIFY(!done.at(0).at(4).toString().isEmpty());
        QVERIFY(!ctl.powerMatrixPending());
        QVERIFY2(client.lastOpId > before,
                 "a rejected threshold write did not re-read the roster, so "
                 "the UI keeps whatever it optimistically showed");
        // The controller never wrote the requested value into the snapshot.
        QCOMPARE(ctl.powerLevelForKey(QStringLiteral("ban")), qlonglong(50));
    }

    // A stale answer — from the room the dialog was on before — must not clear
    // the current pending state.
    void aStaleThresholdAnswerIsIgnored()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        ctl.setPowerLevelKey(QStringLiteral("ban"), 0);
        QVERIFY(ctl.powerMatrixPending());
        QSignalSpy done(&ctl, &RoomInfoController::powerMatrixActionFinished);
        Q_EMIT client.roomPowerMatrixFinished(client.lastMatrixOpId + 999,
                                              kSpace, QStringLiteral("ban"), 0,
                                              true, QString());
        QCOMPARE(done.size(), 0);
        QVERIFY(ctl.powerMatrixPending());
    }

    // ---- Members facets --------------------------------------------------

    void theMembershipFacetFiltersAndAnUnknownOneMatchesNothing()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QCOMPARE(ctl.filterMembers(QString()).size(), 4);
        QCOMPARE(ctl.filterMembers(QString(), QStringLiteral("joined"),
                                   false).size(), 2);
        QCOMPARE(ctl.filterMembers(QString(), QStringLiteral("banned"),
                                   false).size(), 1);
        // A filter that silently stops filtering looks exactly like a filter
        // that found everything.
        QCOMPARE(ctl.filterMembers(QString(), QStringLiteral("left"),
                                   false).size(), 0);
    }

    void aToZSortsByTheNameAPersonReads()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        const QVariantList rows =
            ctl.filterMembers(QString(), QString(), /*alphabetical=*/true);
        QCOMPARE(rows.size(), 4);
        QStringList names;
        for (const QVariant &row : rows)
            names << row.toMap().value(QStringLiteral("displayName")).toString();
        QCOMPARE(names, (QStringList{ QStringLiteral("Amy"),
                                      QStringLiteral("Banned Bob"),
                                      QStringLiteral("Me"),
                                      QStringLiteral("Zoe") }));
    }

    // A room using 42 gets its OWN group. Folding it into Moderator would
    // misdescribe the room's configuration in the one place a person consults
    // to understand it — the same rule roleLabelForLevel already follows.
    void roleGroupsGiveACustomLevelItsOwnBucket()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        const QVariantList groups =
            ctl.memberRoleGroups(QString(), QString(), false);
        QCOMPARE(groups.size(), 3); // 100, 42, 0
        QCOMPARE(groups.at(0).toMap().value(QStringLiteral("level")).toLongLong(),
                 qlonglong(100));
        QCOMPARE(groups.at(1).toMap().value(QStringLiteral("level")).toLongLong(),
                 qlonglong(42));
        QVERIFY(groups.at(1).toMap().value(QStringLiteral("label")).toString()
                    .contains(QStringLiteral("42")));
        QCOMPARE(groups.at(2).toMap().value(QStringLiteral("members")).toList()
                     .size(), 2);
    }

    // The room member panel is a ListView over a room that may have thousands
    // of members, so it needs ONE FLAT model — a Repeater over the nested
    // groups instantiates every row of every group at once. The flattening is
    // in C++ with the bucketing, or the grouping rule lives in two places.
    void theFlattenedRoleRowsCarryTheirHeadingsInOrder()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);

        const QVariantList groups =
            ctl.memberRoleGroups(QString(), QString(), false);
        const QVariantList rows =
            ctl.memberRoleRows(QString(), QString(), false);

        // Same content, same order: one header per group followed by that
        // group's members.
        int expected = groups.size();
        for (const QVariant &g : groups)
            expected += g.toMap().value(QStringLiteral("members")).toList().size();
        QCOMPARE(rows.size(), expected);

        int at = 0;
        for (const QVariant &g : groups) {
            const QVariantMap group = g.toMap();
            const QVariantMap header = rows.at(at++).toMap();
            QCOMPARE(header.value(QStringLiteral("kind")).toString(),
                     QStringLiteral("header"));
            QCOMPARE(header.value(QStringLiteral("label")).toString(),
                     group.value(QStringLiteral("label")).toString());
            QCOMPARE(header.value(QStringLiteral("level")).toLongLong(),
                     group.value(QStringLiteral("level")).toLongLong());
            const QVariantList members =
                group.value(QStringLiteral("members")).toList();
            QCOMPARE(header.value(QStringLiteral("count")).toInt(),
                     int(members.size()));
            // A header's id must never be mistakable for a user id — every
            // Matrix user id starts with '@', so a delegate keying on it
            // would otherwise open a profile for a heading.
            QVERIFY(!header.value(QStringLiteral("userId")).toString()
                         .startsWith(QLatin1Char('@')));
            for (const QVariant &m : members) {
                const QVariantMap row = rows.at(at++).toMap();
                QCOMPARE(row.value(QStringLiteral("kind")).toString(),
                         QStringLiteral("member"));
                QCOMPARE(row.value(QStringLiteral("userId")).toString(),
                         m.toMap().value(QStringLiteral("userId")).toString());
                // Carried per row so a recycled delegate cannot inherit the
                // previous member's role: roleLabelForLevel is Q_INVOKABLE,
                // so a call from a binding creates no dependency.
                QCOMPARE(row.value(QStringLiteral("roleLabel")).toString(),
                         group.value(QStringLiteral("label")).toString());
                QCOMPARE(row.value(QStringLiteral("powerLevel")).toLongLong(),
                         group.value(QStringLiteral("level")).toLongLong());
            }
        }
        QCOMPARE(at, int(rows.size()));

        // The membership filter and the A-to-Z sort reach it, or the panel's
        // two controls are inert.
        QCOMPARE(ctl.memberRoleRows(QString(), QStringLiteral("banned"),
                                    false).size(),
                 2);   // one heading + one banned member
        QVERIFY(ctl.memberRoleRows(QStringLiteral("zzzz"), QString(),
                                   false).isEmpty());
    }

    // Sign-out and a Space switch both clear the matrix. An empty map is the
    // unknown state, so a stale threshold cannot survive into the next room.
    void switchingSpaceClearsTheMatrix()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        QVERIFY(ctl.powerLevelKnown(QStringLiteral("ban")));
        ctl.setRoomId(QStringLiteral("!other:example.org"));
        QVERIFY(!ctl.powerLevelKnown(QStringLiteral("ban")));
        QVERIFY(ctl.powerLevels().isEmpty());
        QVERIFY(ctl.roomVersion().isEmpty());
        QVERIFY(!ctl.canUpgradeRoom());
    }
};

QTEST_MAIN(SpaceSettingsContractTest)
#include "SpaceSettingsContractTest.moc"
