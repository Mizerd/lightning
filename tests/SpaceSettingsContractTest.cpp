
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

}

class SpaceSettingsContractTest : public QObject
{
    Q_OBJECT

    static QString spaceSettingsBlock(const QString &pane)
    {
        const int start = pane.indexOf(QStringLiteral("id: settingsCol"));
        if (start < 0)
            return {};
        const int end = pane.indexOf(QStringLiteral("SpaceLobby {"), start);
        if (end < 0)
            return {};
        return pane.mid(start, end - start);
    }

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
                                 QStringLiteral("joined"),true),
                       memberRow(kZoe, QStringLiteral("Zoe"), usersDefault,
                                 QStringLiteral("joined")),
                       memberRow(kAmy, QStringLiteral("Amy"), 42,
                                 QStringLiteral("invited")),
                       memberRow(kBan, QStringLiteral("Banned Bob"), usersDefault,
                                 QStringLiteral("banned")) },
                     canChangePl, usersDefault, powerLevels));
    }

private Q_SLOTS:

    void spaceHomeExposesAllThreeEdits()
    {
        const QString block =
            spaceSettingsBlock(readQml(QStringLiteral("TimelinePane.qml")));
        QVERIFY2(!block.isEmpty(), "the Space settings card is missing");
        QVERIFY(block.contains(QStringLiteral("fieldObjectName: \"spaceNameEditField\"")));
        QVERIFY(block.contains(QStringLiteral("fieldObjectName: \"spaceTopicEditField\"")));
        QVERIFY2(block.contains(QStringLiteral("objectName: \"spaceChangeAvatarButton\"")),
                 "a Space could be renamed but never given an avatar");
        QVERIFY(block.contains(QStringLiteral("objectName: \"spaceRemoveAvatarButton\"")));
    }

    void spacePeopleListsOnlyTheJoined()
    {
        const QString pane = readQml(QStringLiteral("TimelinePane.qml"));
        const int button = pane.indexOf(QStringLiteral("objectName: \"spacePeopleButton\""));
        const int settings = pane.indexOf(QStringLiteral("id: settingsCol"), button);
        QVERIFY2(button >= 0 && settings > button, "the Space Home people button moved");
        const QString buttonBlock = pane.mid(button, settings - button);
        QVERIFY(buttonBlock.contains(QStringLiteral("spaceHome.spacePeople.length")));
        QVERIFY2(!buttonBlock.contains(QStringLiteral("app.roomInfo.members")),
                 "the people button reads the whole roster, bans included");

        const int lobby = pane.indexOf(QStringLiteral("SpaceLobby {"), settings);
        const int card = pane.indexOf(QStringLiteral("objectName: \"spacePeopleCard\""));
        const int end = pane.indexOf(QStringLiteral("id: removeChildConfirm"), card);
        QVERIFY2(lobby > 0 && card > lobby && end > card,
                 "the people card is no longer after the Space's channel list");
        const QString cardBlock = pane.mid(card, end - card);
        QVERIFY2(!cardBlock.contains(QStringLiteral("app.roomInfo.members")),
                 "the people card reads the whole roster, bans included");
        QVERIFY(cardBlock.contains(QStringLiteral("model: spaceHome.spacePeople.slice(0, 60)")));
        QVERIFY(cardBlock.contains(QStringLiteral(".arg(spaceHome.spacePeople.length)")));
        QVERIFY(pane.contains(QStringLiteral(
            "app.roomInfo ? (app.roomInfo.joinedMembers || []) : []")));
    }

    void joinedMembersAreTheJoinedRowsInRosterOrder()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        ctl.setRoomId(kSpace);
        QSignalSpy changed(&ctl, &RoomInfoController::membersChanged);
        Q_EMIT client.roomMembersReceived(
            client.lastOpId, kSpace,
            snapshot(100,
                     { memberRow(kZoe, QStringLiteral("Zoe"), 0, QStringLiteral("joined")),
                       memberRow(kBan, QStringLiteral("Banned Bob"), 0,
                                 QStringLiteral("banned")),
                       memberRow(kAmy, QStringLiteral("Amy"), 0, QStringLiteral("invited")),
                       memberRow(QStringLiteral("@gone:example.org"), QStringLiteral("Gone"),
                                 0, QStringLiteral("other")),
                       memberRow(kMe, QStringLiteral("Me"), 100, QStringLiteral("joined"),
true) }));
        QVERIFY(changed.count() > 0);
        QCOMPARE(ctl.members().size(), 5);
        QStringList ids;
        for (const QVariant &row : ctl.joinedMembers())
            ids << row.toMap().value(QStringLiteral("userId")).toString();
        QCOMPARE(ids, (QStringList{ kZoe, kMe }));
        const QMetaObject *meta = ctl.metaObject();
        const QMetaProperty property =
            meta->property(meta->indexOfProperty("joinedMembers"));
        QVERIFY(property.isValid());
        QCOMPARE(property.notifySignal().name(), QByteArray("membersChanged"));
    }

    void theSpaceHomeTitleFollowsARename()
    {
        const QString pane = readQml(QStringLiteral("TimelinePane.qml"));
        const int title = pane.indexOf(QStringLiteral("objectName: \"roomHeaderTitle\""));
        QVERIFY(title > 0);
        const int call = pane.indexOf(QStringLiteral("app.spaces.spaceName("), title);
        QVERIFY(call > title);
        QVERIFY2(pane.mid(title, call - title).contains(QStringLiteral("root.spacesTick")),
                 "the Space Home title has no dependency on Space changes");
        QVERIFY(pane.contains(QStringLiteral("function onSpacesChanged() { root.spacesTick++ }")));
    }

    void theSpaceHomeActionRowWraps()
    {
        const QString pane = readQml(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        const int createAt =
            pane.indexOf(QStringLiteral("objectName: \"spaceCreateRoomButton\""));
        QVERIFY2(createAt >= 0, "spaceCreateRoomButton is gone from Space Home");
        const int flowAt = pane.lastIndexOf(QStringLiteral("Flow {"), createAt);
        const int rowAt = pane.lastIndexOf(QStringLiteral("RowLayout {"), createAt);
        QVERIFY2(flowAt > rowAt,
                 "the Space Home action row is back inside a RowLayout, which "
                 "does not wrap -- its buttons run off the right edge of a "
                 "narrow pane and some are unreachable");
        const int lineStart = pane.lastIndexOf(QLatin1Char('\n'), flowAt) + 1;
        const QString indent = QString(flowAt - lineStart, QLatin1Char(' '));
        const int flowEnd = pane.indexOf(QLatin1Char('\n') + indent
                                             + QStringLiteral("}"),
                                         flowAt);
        QVERIFY2(flowEnd > flowAt, "could not find the end of the Flow block");
        const QString row = pane.mid(flowAt, flowEnd - flowAt);
        QVERIFY2(row.contains(QStringLiteral("objectName: \"spaceCreateRoomButton\"")),
                 "the Flow block located here is not the action row");
        const int firstChildAt = row.indexOf(QStringLiteral("AppButton {"));
        QVERIFY2(firstChildAt > 0, "the Flow block has no buttons in it");
        const QString flowHeader = row.left(firstChildAt);
        QVERIFY2(flowHeader.contains(QStringLiteral("Layout.fillWidth: true")),
                 "the Flow does not fill the pane's width, so it wraps "
                 "against its own implicit width instead of the space "
                 "available");
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

    void permissionMatrixCoversEveryKeyTheBackendAccepts()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
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

    void roomInformationReusesThePermissionMatrix()
    {
        const QString panel = readQml(QStringLiteral("RoomInfoPanel.qml"));
        const QString dialog = readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(panel.contains(QStringLiteral("root.roomPermissionsDialog.openPermissionsForRoom(")));
        QVERIFY(dialog.contains(QStringLiteral("function openPermissionsForRoom(")));
        QVERIFY(dialog.contains(QStringLiteral("section = 2")));
        QVERIFY(dialog.contains(QStringLiteral("roomMode ? [")));
        for (const QString &key : {QStringLiteral("events_default"),
                                   QStringLiteral("notifications.room"),
                                   QStringLiteral("redact"),
                                   QStringLiteral("m.room.power_levels"),
                                   QStringLiteral("m.room.history_visibility")}) {
            QVERIFY2(dialog.contains(QStringLiteral("key: \"%1\"").arg(key)),
                     qPrintable(key));
            QVERIFY(RoomInfoController::powerLevelKeys().contains(key));
        }
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.canSetPowerLevel(")));
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.canSetPowerLevelKey(")));
        QVERIFY(dialog.contains(QStringLiteral("app.roomInfo.powerMatrixError")));
    }

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

    void thresholdCombosSnapBackRatherThanBind()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("levelCombo.syncToValue(")));
        QString code = dialog;
        code.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        code.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                       QRegularExpression::DotMatchesEverythingOption));
        QVERIFY2(!code.contains(QStringLiteral("currentIndex: indexOfValue")),
                 "a threshold combo bound to indexOfValue() shows row 0 while "
                 "the space holds something else");
        QVERIFY2(code.contains(QStringLiteral("levelCombo.syncToValue(")),
                 "the comment stripper ate the code, so the ban is vacuous");
        QVERIFY2(dialog.contains(QStringLiteral("function onRosterTickChanged()")),
                 "nothing snaps the combo back after a rejected write");
    }

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

    void bannerUsesTheBannerBackendAndItsOwnPermission()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("app.banners.setRoomBanner(")));
        QVERIFY(dialog.contains(QStringLiteral("app.banners.clearRoomBanner(")));
        QVERIFY2(dialog.contains(QStringLiteral("app.banners.canSetRoomBanner(")),
                 "the banner controls are gated on some other event's level");
        QVERIFY2(dialog.contains(QStringLiteral("app.banners.refreshRoom(")),
                 "the Space settings dialog no longer re-reads the banner");
        QVERIFY2(!dialog.contains(QStringLiteral("app.banners.requestRoom(")),
                 "a once-per-session read is back beside the refresh");
    }

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

    void developerToolsCanCopyEveryRow()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("id: devCopyHelper")));
        QVERIFY(dialog.contains(QStringLiteral("devCopyHelper.copy()")));
        QVERIFY(dialog.contains(QStringLiteral("spaceSettingsDevCopy")));
    }

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

    void aThresholdAboveYourOwnLevelIsNeverOffered()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client,50);
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("m.room.power_levels"),
                                         100));
        QVERIFY(ctl.canSetPowerLevelKey(QStringLiteral("m.room.power_levels"),
                                        50));
        ctl.setPowerLevelKey(QStringLiteral("m.room.power_levels"), 100);
        QCOMPARE(client.matrixCalls, 0);
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
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"), 50));
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"),
                                         RoomInfoController::kMaxSettableLevel + 1));
        QVERIFY(!ctl.canSetPowerLevelKey(QStringLiteral("ban"),
                                         RoomInfoController::kMinSettableLevel - 1));
        QVERIFY(ctl.canSetPowerLevelKey(QStringLiteral("ban"), 0));
    }

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
        ctl.setPowerLevelKey(QStringLiteral("kick"), 0);
        QCOMPARE(client.matrixCalls, 1);

        const quint64 before = client.lastOpId;
        Q_EMIT client.roomPowerMatrixFinished(client.lastMatrixOpId, kSpace,
                                              QStringLiteral("ban"), 0, false,
                                              QStringLiteral("forbidden"));
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.at(0).at(1).toString(), QStringLiteral("ban"));
        QCOMPARE(done.at(0).at(3).toBool(), false);
        QVERIFY(!done.at(0).at(4).toString().isEmpty());
        QVERIFY(!ctl.powerMatrixPending());
        QVERIFY2(client.lastOpId > before,
                 "a rejected threshold write did not re-read the roster, so "
                 "the UI keeps whatever it optimistically showed");
        QCOMPARE(ctl.powerLevelForKey(QStringLiteral("ban")), qlonglong(50));
    }

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
            ctl.filterMembers(QString(), QString(),true);
        QCOMPARE(rows.size(), 4);
        QStringList names;
        for (const QVariant &row : rows)
            names << row.toMap().value(QStringLiteral("displayName")).toString();
        QCOMPARE(names, (QStringList{ QStringLiteral("Amy"),
                                      QStringLiteral("Banned Bob"),
                                      QStringLiteral("Me"),
                                      QStringLiteral("Zoe") }));
    }

    void roleGroupsGiveACustomLevelItsOwnBucket()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        seed(ctl, client);
        const QVariantList groups =
            ctl.memberRoleGroups(QString(), QString(), false);
        QCOMPARE(groups.size(), 3);
        QCOMPARE(groups.at(0).toMap().value(QStringLiteral("level")).toLongLong(),
                 qlonglong(100));
        QCOMPARE(groups.at(1).toMap().value(QStringLiteral("level")).toLongLong(),
                 qlonglong(42));
        QVERIFY(groups.at(1).toMap().value(QStringLiteral("label")).toString()
                    .contains(QStringLiteral("42")));
        QCOMPARE(groups.at(2).toMap().value(QStringLiteral("members")).toList()
                     .size(), 2);
    }

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
            QVERIFY(!header.value(QStringLiteral("userId")).toString()
                         .startsWith(QLatin1Char('@')));
            for (const QVariant &m : members) {
                const QVariantMap row = rows.at(at++).toMap();
                QCOMPARE(row.value(QStringLiteral("kind")).toString(),
                         QStringLiteral("member"));
                QCOMPARE(row.value(QStringLiteral("userId")).toString(),
                         m.toMap().value(QStringLiteral("userId")).toString());
                QCOMPARE(row.value(QStringLiteral("roleLabel")).toString(),
                         group.value(QStringLiteral("label")).toString());
                QCOMPARE(row.value(QStringLiteral("powerLevel")).toLongLong(),
                         group.value(QStringLiteral("level")).toLongLong());
            }
        }
        QCOMPARE(at, int(rows.size()));

        QCOMPARE(ctl.memberRoleRows(QString(), QStringLiteral("banned"),
                                    false).size(),
                 2);
        QVERIFY(ctl.memberRoleRows(QStringLiteral("zzzz"), QString(),
                                   false).isEmpty());
    }

    void memberActionsAreGatedAndConfirmed()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        const int at = dialog.indexOf(QStringLiteral("function openMemberMenu("));
        QVERIFY2(at > 0, "the member menu opener was not found");
        const int end = dialog.indexOf(QStringLiteral("memberMenu.popup("), at);
        QVERIFY2(end > at, "the opener's end anchor was not found");
        const QString opener = dialog.mid(at, end - at);
        QVERIFY2(opener.contains(QStringLiteral("!root.infoIsOurs")),
                 "the menu can open against another room's roster");
        for (const QString &op : { QStringLiteral("kick"), QStringLiteral("ban"),
                                   QStringLiteral("unban") }) {
            QVERIFY2(opener.contains(
                         QStringLiteral("canModerate(uid, \"%1\")").arg(op)),
                     qPrintable(op));
        }
        QVERIFY(opener.contains(QStringLiteral("canSetPowerLevel(uid, level)")));

        for (const QString &op : { QStringLiteral("Kick"), QStringLiteral("Ban"),
                                   QStringLiteral("Unban") }) {
            const int item = dialog.indexOf(
                QStringLiteral("objectName: \"spaceMember%1\"").arg(op));
            QVERIFY2(item > 0, qPrintable(op));
            const QString block = dialog.mid(item, 520);
            QVERIFY2(block.contains(
                         QStringLiteral("visible: memberMenu.can%1").arg(op)),
                     qPrintable(op));
            QVERIFY2(block.contains(QStringLiteral("memberAction.openFor(")),
                     qPrintable(op));
            QVERIFY2(!block.contains(QStringLiteral("kickMember("))
                         && !block.contains(QStringLiteral("banMember(")),
                     "a menu item acts without confirmation");
        }
        QVERIFY(dialog.contains(QStringLiteral("SpaceMemberActionDialog {")));
        QVERIFY(dialog.contains(QStringLiteral("acceptedButtons: Qt.RightButton")));
    }

    void spaceHomeMemberChipOffersConfirmedModeration()
    {
        const QString chip = readQml(QStringLiteral("SpaceMemberChip.qml"));
        const QString home = readQml(QStringLiteral("TimelinePane.qml"));
        QVERIFY(chip.contains(QStringLiteral("app.roomInfo.roomId !== spaceId")));
        QVERIFY(chip.contains(QStringLiteral("canModerate(modelData.userId, \"kick\")")));
        QVERIFY(chip.contains(QStringLiteral("canModerate(modelData.userId, \"ban\")")));
        QVERIFY(chip.contains(QStringLiteral("acceptedButtons: Qt.RightButton")));
        QVERIFY(home.contains(QStringLiteral("onModerationRequested: (member, action)")));
        QVERIFY(home.contains(QStringLiteral("spaceChipMemberAction.openFor(")));
    }

    void theActionDialogRendersTheControllersText()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceMemberActionDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("app.spaceModeration")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.consequenceText")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.cascadeLabel")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.statusText")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.confirm(")));
        QVERIFY(dialog.contains(QStringLiteral("enabled: root.ctl && root.ctl.canConfirm")));
        QVERIFY(dialog.contains(QStringLiteral("Popup.NoAutoClose")));
        QVERIFY(!dialog.contains(QStringLiteral("Text.StyledText")));
        QVERIFY(!dialog.contains(QStringLiteral("Text.RichText")));
    }

    void openingDuringARunningFlowIsNotSilent()
    {
        const QString dialog =
            readQml(QStringLiteral("SpaceMemberActionDialog.qml"));
        const int at = dialog.indexOf(QStringLiteral("function openFor("));
        QVERIFY(at > 0);
        const int end = dialog.indexOf(QStringLiteral("\n    }"), at);
        QVERIFY(end > at);
        const QString body = dialog.mid(at, end - at);
        QVERIFY2(!body.contains(QStringLiteral("running")),
                 "openFor returns early while a flow runs, so the click "
                 "does nothing visible");
        QVERIFY(body.contains(QStringLiteral("open()")));
        QVERIFY(body.contains(QStringLiteral("ctl.begin(")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.notice")));
    }

    void aCreatorIsNotOfferedToLowerTheirOwnLevel()
    {
        FakeClient client;
        RoomInfoController ctl;
        ctl.setClient(&client);
        const qlonglong creator = qlonglong(1) << 53;
        seed(ctl, client,creator);
        QVERIFY(!ctl.canSetPowerLevel(kMe, 100));
        QVERIFY(!ctl.canSetPowerLevel(kMe, 0));
        QVERIFY(ctl.canSetPowerLevel(kZoe, 50));

        FakeClient client2;
        RoomInfoController admin;
        admin.setClient(&client2);
        seed(admin, client2,100);
        QVERIFY(admin.canSetPowerLevel(kMe, 50));

        const QString dialog = readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        const int at = dialog.indexOf(QStringLiteral("readonly property bool selfDemotion:"));
        QVERIFY(at > 0);
        QVERIFY(dialog.mid(at, 300).contains(
            QStringLiteral("app.roomInfo.ownPowerLevel < 9007199254740992")));
        QCOMPARE(ctl.roleLabelForLevel(creator), QStringLiteral("Creator"));
        QCOMPARE(ctl.roleLabelForLevel(creator - 1),
                 QStringLiteral("Custom (%1)").arg(creator - 1));
        const int loses = dialog.indexOf(QStringLiteral("readonly property bool losesRoleControl:"));
        QVERIFY(loses > 0);
        QVERIFY(dialog.mid(loses, 200).contains(QStringLiteral("root.rosterTick")));
    }

    void loweringYourOwnRoleIsConfirmedWithAWarning()
    {
        const QString dialog = readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        const int at = dialog.indexOf(QStringLiteral("id: roleConfirm"));
        QVERIFY(at > 0);
        const QString confirm = dialog.mid(at, 5200);
        QVERIFY(confirm.contains(QStringLiteral("readonly property bool selfDemotion:")));
        QVERIFY(confirm.contains(
            QStringLiteral("isOwn && level < app.roomInfo.ownPowerLevel")));
        QVERIFY(confirm.contains(QStringLiteral("visible: roleConfirm.selfDemotion")));
        QVERIFY(confirm.contains(QStringLiteral("raise it back")));
        QVERIFY(confirm.contains(QStringLiteral("Lower your own role?")));

        QVERIFY(dialog.contains(
            QStringLiteral("memberMenu.targetIsOwn = member.isOwn === true")));
        QVERIFY(dialog.contains(QStringLiteral("memberMenu.targetIsOwn)")));

        QCOMPARE(dialog.count(QStringLiteral("setMemberPowerLevel(")), 1);
        QVERIFY(confirm.contains(QStringLiteral("setMemberPowerLevel(")));
        QCOMPARE(dialog.count(QStringLiteral("roleConfirm.openFor(")), 5);
    }

    void closeAndDeleteAreSeparateAndGated()
    {
        const QString dialog = readQml(QStringLiteral("SpaceSettingsDialog.qml"));
        const int close = dialog.indexOf(QStringLiteral("objectName: \"spaceCloseButton\""));
        QVERIFY(close > 0);
        const QString closeBlock = dialog.mid(
            close, dialog.indexOf(QStringLiteral("AppButton {"), close) - close);
        QVERIFY(closeBlock.contains(QStringLiteral("app.roomInfo.canChangeJoinRule")));
        QVERIFY(closeBlock.contains(QStringLiteral("root.infoIsOurs")));
        QVERIFY(closeBlock.contains(QStringLiteral("true, \"close\")")));
        QVERIFY(!closeBlock.contains(QStringLiteral("Delete")));

        const int del = dialog.indexOf(QStringLiteral("objectName: \"spaceDeleteButton\""));
        QVERIFY(del > 0);
        const QString deleteBlock = dialog.mid(
            del, dialog.indexOf(QStringLiteral("RoomCloseDialog {"), del) - del);
        QVERIFY(deleteBlock.contains(
            QStringLiteral("app.roomClosure.serverAdmin === \"yes\"")));
        QVERIFY(deleteBlock.contains(QStringLiteral("true, \"delete\")")));
        QVERIFY(dialog.contains(QStringLiteral("RoomCloseDialog { id: spaceCloseDialog }")));
        const int open = dialog.indexOf(QStringLiteral("function openFor(targetSpaceId)"));
        QVERIFY(open > 0);
        QVERIFY(dialog.mid(open, 1600).contains(
            QStringLiteral("app.roomClosure.checkServerAdmin()")));
    }

    void theCloseDialogRendersTheControllersText()
    {
        const QString dialog = readQml(QStringLiteral("RoomCloseDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY(dialog.contains(QStringLiteral("app.roomClosure")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.consequenceText")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.statusText")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.notice")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.confirm(")));
        QVERIFY(dialog.contains(QStringLiteral("enabled: root.ctl && root.ctl.canConfirm")));
        QVERIFY(dialog.contains(QStringLiteral("root.ctl.setTypedConfirmation(text)")));
        QVERIFY2(!dialog.contains(QStringLiteral("Popup.NoAutoClose")),
                 "the dialog holds the whole app behind a modal while it runs");
        QVERIFY(dialog.contains(QStringLiteral("ctl.viewClosed()")));
        QVERIFY(!dialog.contains(QStringLiteral("ctl.reset()")));
        const int cancel = dialog.indexOf(QStringLiteral("objectName: \"roomCloseCancel\""));
        QVERIFY(cancel > 0);
        const QString cancelBlock = dialog.mid(cancel, 500);
        QVERIFY(!cancelBlock.contains(QStringLiteral("enabled:")));
        QVERIFY(cancelBlock.contains(QStringLiteral("qsTr(\"Hide\")")));
        QVERIFY(!dialog.contains(QStringLiteral("Text.StyledText")));
        QVERIFY(!dialog.contains(QStringLiteral("Text.RichText")));
        const int at = dialog.indexOf(QStringLiteral("function openFor("));
        QVERIFY(at > 0);
        const QString body = dialog.mid(at, dialog.indexOf(QStringLiteral("\n    }"), at) - at);
        QVERIFY(!body.contains(QStringLiteral("running")));
        QVERIFY(body.contains(QStringLiteral("open()")));
    }

    void theRoomListsShowWholeRowsAndTheSummary()
    {
        for (const QString &name : { QStringLiteral("RoomCloseDialog.qml"),
                                     QStringLiteral("SpaceMemberActionDialog.qml") }) {
            const QString dialog = readQml(name);
            QVERIFY2(!dialog.isEmpty(), qPrintable(name));
            QVERIFY2(!dialog.contains(QStringLiteral("Math.min(contentHeight")),
                     qPrintable(name));
            QVERIFY2(dialog.contains(
                         QStringLiteral("Layout.preferredHeight: Math.min(count * rowHeight, 240)")),
                     qPrintable(name));
            QVERIFY2(dialog.contains(QStringLiteral("height: roomList.rowHeight")),
                     qPrintable(name));
        }
        const QString close = readQml(QStringLiteral("RoomCloseDialog.qml"));
        QVERIFY(close.contains(
            QStringLiteral("qsTr(\"%1 · also in another space\").arg(r.summary)")));
    }

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
