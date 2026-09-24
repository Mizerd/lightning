// Space settings: the Space Home management card, the Space settings dialog
// (qml/SpaceSettingsDialog.qml), and the RoomInfoController policy behind its
// Permissions matrix and Members facets.
//
// A Space is a Matrix room: its name, topic and avatar go through the same
// permission-gated RoomInfoController backend as Room Information, and the
// permission matrix is the room's own `m.room.power_levels`. No Space-specific
// storage or permission model.
//
// Two kinds of test: source contracts for the QML (a control exists, is wired
// to the shared backend and carries the right gate), and real controller
// tests against a fake MatrixClient. No homeserver is contacted; a real
// power_levels round trip and Element interoperability are not tested.

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

    // The Space settings card inside Space Home, bounded so an assertion
    // cannot match elsewhere in a large file.
    static QString spaceSettingsBlock(const QString &pane)
    {
        const int start = pane.indexOf(QStringLiteral("id: settingsCol"));
        if (start < 0)
            return {};
        // The lobby after the card lives in SpaceLobby.qml; its instance is
        // the end marker.
        const int end = pane.indexOf(QStringLiteral("SpaceLobby {"), start);
        if (end < 0)
            return {};
        return pane.mid(start, end - start);
    }

    // Own level 100, an ordinary member, a member on a custom 42, and a banned
    // member: the four cases the Members page renders.
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
    // Space Home card.

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

    // The action row wraps instead of running off the pane.

    void theSpaceHomeActionRowWraps()
    {
        // Up to six buttons, present depending on permissions and state, so no
        // fixed width can be assumed: the row must wrap.
        const QString pane = readQml(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        const int createAt =
            pane.indexOf(QStringLiteral("objectName: \"spaceCreateRoomButton\""));
        QVERIFY2(createAt >= 0, "spaceCreateRoomButton is gone from Space Home");
        // Walk back to the container holding the button: its own line decides
        // whether the row wraps.
        const int flowAt = pane.lastIndexOf(QStringLiteral("Flow {"), createAt);
        const int rowAt = pane.lastIndexOf(QStringLiteral("RowLayout {"), createAt);
        QVERIFY2(flowAt > rowAt,
                 "the Space Home action row is back inside a RowLayout, which "
                 "does not wrap -- its buttons run off the right edge of a "
                 "narrow pane and some are unreachable");
        // Bounded to the Flow's own block (up to the first bare `}` at its
        // indent), not a fixed window that runs into siblings.
        const int lineStart = pane.lastIndexOf(QLatin1Char('\n'), flowAt) + 1;
        const QString indent = QString(flowAt - lineStart, QLatin1Char(' '));
        const int flowEnd = pane.indexOf(QLatin1Char('\n') + indent
                                             + QStringLiteral("}"),
                                         flowAt);
        QVERIFY2(flowEnd > flowAt, "could not find the end of the Flow block");
        const QString row = pane.mid(flowAt, flowEnd - flowAt);
        QVERIFY2(row.contains(QStringLiteral("objectName: \"spaceCreateRoomButton\"")),
                 "the Flow block located here is not the action row");
        // The Flow's own header only (lines before its first child), so a
        // child's fillWidth cannot satisfy it.
        const int firstChildAt = row.indexOf(QStringLiteral("AppButton {"));
        QVERIFY2(firstChildAt > 0, "the Flow block has no buttons in it");
        const QString flowHeader = row.left(firstChildAt);
        QVERIFY2(flowHeader.contains(QStringLiteral("Layout.fillWidth: true")),
                 "the Flow does not fill the pane's width, so it wraps "
                 "against its own implicit width instead of the space "
                 "available");
        // A fillWidth spacer is a RowLayout idiom; in a Flow it would take a
        // whole row.
        QVERIFY2(!row.contains(QStringLiteral("Item { Layout.fillWidth: true }")),
                 "a fillWidth spacer survived the move into the Flow");
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
        // The picker hands its result to the shared crop dialog, which uploads
        // through the shared room backend; both halves are asserted (the
        // wiring is covered in full by image-crop-contract).
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

    // The dialog's surfaces (source contracts).
    //
    // The Permissions page covers every key the backend accepts.
    void permissionMatrixCoversEveryKeyTheBackendAccepts()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        // Each row names a key the controller and the Rust edge accept; an
        // unknown key is a control that does nothing.
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

    // No row for `m.call.member`: it governs neither the identifier Lightning
    // sends nor the stable one.
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

    // Threshold combos mirror the value explicitly: `currentIndex:
    // indexOfValue(…)` is -1 at creation time, and clamping it to 0 makes the
    // control misreport the room.
    void thresholdCombosSnapBackRatherThanBind()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("levelCombo.syncToValue(")));
        // Scanned without comments: the dialog's own comment names the shape
        // this bans.
        QString code = dialog;
        code.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        code.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                       QRegularExpression::DotMatchesEverythingOption));
        QVERIFY2(!code.contains(QStringLiteral("currentIndex: indexOfValue")),
                 "a threshold combo bound to indexOfValue() shows row 0 while "
                 "the space holds something else");
        // The stripper must actually strip, or the ban is vacuous.
        QVERIFY2(code.contains(QStringLiteral("levelCombo.syncToValue(")),
                 "the comment stripper ate the code, so the ban is vacuous");
        QVERIFY2(dialog.contains(QStringLiteral("function onRosterTickChanged()")),
                 "nothing snaps the combo back after a rejected write");
    }

    // Every binding that calls a controller method reads the roster tick; a
    // method call creates no property dependency.
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

    // The Members page has a count, filters, and a notice when the roster is
    // capped.
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

    // The banner is a real state event with its own required level, not gated
    // on canEditAvatar.
    void bannerUsesTheBannerBackendAndItsOwnPermission()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("app.banners.setRoomBanner(")));
        QVERIFY(dialog.contains(QStringLiteral("app.banners.clearRoomBanner(")));
        QVERIFY2(dialog.contains(QStringLiteral("app.banners.canSetRoomBanner(")),
                 "the banner controls are gated on some other event's level");
        // ...and the dialog re-reads it on every open: sliding sync does not
        // deliver this custom state type (rust/src/banner.rs), and
        // `requestRoom` asks once per room per session.
        QVERIFY2(dialog.contains(QStringLiteral("app.banners.refreshRoom(")),
                 "the Space settings dialog no longer re-reads the banner");
        QVERIFY2(!dialog.contains(QStringLiteral("app.banners.requestRoom(")),
                 "a once-per-session read is back beside the refresh");
    }

    // The Space's identity can change under the open dialog: `info` calls
    // app.spaces.spaceInfo(), so the binding needs a change counter.
    // Rendered behaviour is covered by SpaceIdentityQmlTest.
    void theSpaceInfoBindingCarriesAChangeDependency()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY2(dialog.contains(QStringLiteral("property int spacesTick")),
                 "spaceInfo() is called from a binding with no dependency");
        QVERIFY2(dialog.contains(QStringLiteral("root.spacesTick++")),
                 "nothing bumps the counter the info binding reads");
    }

    // "No banner" is shown only when the Space has no banner, not whenever the
    // Image is invisible (it is also invisible while loading or after a
    // failed fetch).
    void theEmptyBannerStateIsClaimedOnlyWhenThereIsNoBanner()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        const qsizetype label =
            dialog.indexOf(QStringLiteral("objectName: \"spaceSettingsNoBanner\""));
        QVERIFY2(label >= 0, "the empty-banner label lost its objectName");
        const QString block = dialog.mid(label, 240);
        QVERIFY2(block.contains(
                     QStringLiteral("visible: bannerCard.bannerMxc.length === 0")),
                 "the empty state is claimed from the Image's readiness again");
    }

    // Banner recovery as on the profile card: wideImageSource() answers ""
    // while a transient failure mark stands, so cache completion and mark
    // expiry both bump the re-resolve counter; `source` is never assigned
    // imperatively.
    void theSpaceBannerRecoversFromATransientMediaFailure()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        const qsizetype at = dialog.indexOf(QStringLiteral("id: bannerPreview"));
        QVERIFY2(at > 0, "the Space banner preview is gone");
        const QString block = dialog.mid(at);
        QVERIFY2(block.contains(QStringLiteral("function onMediaCached(")),
                 "the banner no longer re-resolves when its bytes land");
        QVERIFY2(block.contains(QStringLiteral("function onMediaRetryable(")),
                 "a banner whose fetch failed once stays absent all session");
        QVERIFY2(!block.contains(QStringLiteral("bannerPreview.source =")),
                 "the banner binding is destroyed by an imperative assignment");
    }

    // Developer tools: every row copyable through the hidden TextEdit relay,
    // not a new C++ clipboard surface.
    void developerToolsCanCopyEveryRow()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("id: devCopyHelper")));
        QVERIFY(dialog.contains(QStringLiteral("devCopyHelper.copy()")));
        QVERIFY(dialog.contains(QStringLiteral("spaceSettingsDevCopy")));
    }

    // No local storage in this file and no upgrade button: an upgrade is
    // irreversible and orphans every m.space.child edge.
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

    // Controller policy.
    //
    // The snapshot carries the room's real thresholds, version and upgrade
    // capability.
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

    // An absent key is unknown, never 0 (0 is a real, permissive threshold),
    // and unknown fails closed for writes.
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

    // A threshold above your own level is never offered: requiring more than
    // you have for m.room.power_levels locks you out of undoing it.
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
        // ...which also stops users_default being raised above the person
        // raising it.
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
        // Outside the settable band. Display is unbounded; only the write is
        // bounded, which also keeps the MSC4289 creator sentinel out.
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"),
                                         RoomInfoController::kMaxSettableLevel + 1));
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"),
                                         RoomInfoController::kMinSettableLevel - 1));
        QVERIFY(ctl.canSetPowerLevelKey(QStringLiteral("ban"), 0));
    }

    // Nothing is applied optimistically: the roster is re-read on success and
    // on rejection.
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

    // A stale answer from a previous room does not clear the current pending
    // state.
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

    // Members facets.

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
        // An unknown facet matches nothing, rather than silently not
        // filtering.
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

    // A custom level (42) gets its own role group rather than being folded
    // into Moderator, as roleLabelForLevel does.
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

    // The member panel is a ListView over potentially thousands of members,
    // so it needs one flat model; flattening happens in C++ next to the
    // bucketing.
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

        // One header per group followed by that group's members, in order.
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
        // A header's id never starts with '@', so it cannot be mistaken for a
        // user id.
            QVERIFY(!header.value(QStringLiteral("userId")).toString()
                         .startsWith(QLatin1Char('@')));
            for (const QVariant &m : members) {
                const QVariantMap row = rows.at(at++).toMap();
                QCOMPARE(row.value(QStringLiteral("kind")).toString(),
                         QStringLiteral("member"));
                QCOMPARE(row.value(QStringLiteral("userId")).toString(),
                         m.toMap().value(QStringLiteral("userId")).toString());
                // Carried per row, so a recycled delegate cannot inherit the
                // previous role (roleLabelForLevel is Q_INVOKABLE).
                QCOMPARE(row.value(QStringLiteral("roleLabel")).toString(),
                         group.value(QStringLiteral("label")).toString());
                QCOMPARE(row.value(QStringLiteral("powerLevel")).toLongLong(),
                         group.value(QStringLiteral("level")).toLongLong());
            }
        }
        QCOMPARE(at, int(rows.size()));

        // The membership filter and A-to-Z sort reach it.
        QCOMPARE(ctl.memberRoleRows(QString(), QStringLiteral("banned"),
                                    false).size(),
                 2);   // one heading + one banned member
        QVERIFY(ctl.memberRoleRows(QStringLiteral("zzzz"), QString(),
                                   false).isEmpty());
    }

    // Sign-out and a Space switch clear the matrix; an empty map is the
    // unknown state.
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
