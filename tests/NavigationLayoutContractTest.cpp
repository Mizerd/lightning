// Navigation layouts: the host/presenter contract. A source-scan suite,
// because what it defends is which file contains what:
//
//  * The host (RoomsPanel) owns the chrome; no presenter declares its own
//    header or search field, or the layouts fork.
//  * A presenter never reaches into the host by id (resolved by scope from a
//    delegate, that breaks silently); it emits signals.
//  * Exactly one presenter is instantiated: Loaders whose `active` is the
//    layout choice, not visibility gates.
//  * Channels is global and never falls back to Classic.
//  * Classic is the default and the clamp target (it works with no Spaces).
//  * New theme tokens are derived, not new required palette keys.
//  * The rail's drag lives in a model, not a JS array rebuilt (reset) on
//    every change.
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QtTest/QtTest>

namespace {

QString read(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString withoutComments(const QString &source)
{
    QString out = source;
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    // Trailing comments too (no `//` appears inside a string literal in these
    // files). The `\\n` in the class matters: a negated class matches
    // newlines and would otherwise swallow following code lines.
    out.remove(QRegularExpression(QStringLiteral("(?m)\\s//[^\"'\\n]*$")));
    return out;
}

QString readSrc(const QString &relative)
{
    QFile file(QStringLiteral(SRC_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

}

class NavigationLayoutContractTest : public QObject
{
    Q_OBJECT

private slots:
    void bothPresentersExistAndTheHostChoosesBetweenThem()
    {
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QVERIFY(!host.isEmpty());
        QVERIFY(!read(QStringLiteral("RoomListClassicPresenter.qml")).isEmpty());
        QVERIFY(!read(QStringLiteral("RoomChannelsPresenter.qml")).isEmpty());

        QVERIFY2(host.contains(QStringLiteral("RoomListClassicPresenter {")),
                 "the host never instantiates the Classic presenter");
        QVERIFY2(host.contains(QStringLiteral("RoomChannelsPresenter {")),
                 "the host never instantiates the Channels presenter");
        QVERIFY2(host.contains(QStringLiteral("roomNavigationLayout")),
                 "the host never reads the layout preference");
    }

    void theHostKeepsTheChromeAndNeitherPresenterDeclaresItsOwn()
    {
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QVERIFY(host.contains(QStringLiteral("headerRow")));
        QVERIFY(host.contains(QStringLiteral("newConversationDialog")));

        for (const QString &name :
             { QStringLiteral("RoomListClassicPresenter.qml"),
               QStringLiteral("RoomChannelsPresenter.qml") }) {
            const QString presenter = withoutComments(read(name));
            QVERIFY2(!presenter.contains(QStringLiteral("id: headerRow")),
                     qPrintable(name + " declares its own workspace header"));
            QVERIFY2(!presenter.contains(QStringLiteral("NewConversationDialog")),
                     qPrintable(name + " declares its own create dialog"));
            QVERIFY2(!presenter.contains(QStringLiteral("VoiceConnectedBar")),
                     qPrintable(name + " declares its own call footer"));
        }
    }

    void theHostOwnsTheRoomListSortButtonInItsHeaderActions()
    {
        const QString host = withoutComments(read(QStringLiteral("RoomsPanel.qml")));
        const int actions = host.indexOf(QStringLiteral("id: headerActions"));
        const int button = host.indexOf(QStringLiteral("roomListOptionsButton"));
        const int newBtn = host.indexOf(QStringLiteral("id: newConversationBtn"));
        QVERIFY2(actions >= 0 && button > actions && newBtn > button,
                 "the sort button is not the first of the header actions");
        QVERIFY2(host.contains(QStringLiteral("RoomListSortMenu {")),
                 "the header button opens no sort menu");
        for (const QString &name :
             { QStringLiteral("RoomListClassicPresenter.qml"),
               QStringLiteral("RoomChannelsPresenter.qml") }) {
            QVERIFY2(!withoutComments(read(name))
                          .contains(QStringLiteral("RoomListSortMenu")),
                     qPrintable(name + " declares its own sort menu"));
        }
    }

    void noPresenterReachesUpIntoTheHostById()
    {
        const QStringList hostIds = {
            QStringLiteral("newConversationDialog"),
            QStringLiteral("discoverJoinDialog"),
            QStringLiteral("leaveRoomConfirm"),
            QStringLiteral("roomLinkClipboard"),
        };
        for (const QString &name :
             { QStringLiteral("RoomListClassicPresenter.qml"),
               QStringLiteral("RoomChannelsPresenter.qml") }) {
            const QString presenter = withoutComments(read(name));
            QVERIFY(!presenter.isEmpty());
            for (const QString &hostId : hostIds) {
                QVERIFY2(!presenter.contains(hostId),
                         qPrintable(name + " reaches the host's '" + hostId
                                    + "' by id instead of by signal"));
            }
        }
        const QString classic =
            read(QStringLiteral("RoomListClassicPresenter.qml"));
        QVERIFY(classic.contains(QStringLiteral("signal leaveRoomRequested")));
        QVERIFY(classic.contains(QStringLiteral("signal roomLinkCopyRequested")));
        QVERIFY(classic.contains(QStringLiteral("signal createRequested")));
        QVERIFY(classic.contains(QStringLiteral("signal roomActivated")));
    }

    void onlyOnePresenterIsInstantiated()
    {
        // Loader-gated, not visibility-gated: the unchosen layout builds no
        // ListView and no delegates (no duplicate avatar fetches).
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QString flat = host;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral(
                     "active: !root.compact && !parent.channelsUsable")),
                 "the Classic presenter is not gated on the layout choice");
        QVERIFY2(flat.contains(QStringLiteral(
                     "active: !root.compact && parent.channelsUsable")),
                 "the Channels presenter is not gated on the layout choice");
        QVERIFY2(flat.contains(QStringLiteral(
                     "active: root.compact")),
                 "the compact presenter is not gated on the compact width");
    }

    void channelsIsGlobalAndNeverFallsBackToClassic()
    {
        const QString host = withoutComments(read(QStringLiteral("RoomsPanel.qml")));
        QString flat = host;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral(
                     "readonly property bool channelsUsable: channelsChosen")),
                 "the host gates Channels on something other than the user's "
                 "choice, so the layout still depends on where the user is");
        QVERIFY2(!flat.contains(QStringLiteral("spaceChannels.spaceId")),
                 "the host still requires an active Space before using "
                 "Channels");
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY2(!settings.contains(QStringLiteral("always use Classic")),
                 "Settings still tells the user Channels falls back to "
                 "Classic");
    }

    void everyChannelActionIsDispatched()
    {
        const QString header = readSrc(QStringLiteral("models/SpaceChannelModel.h"));
        QVERIFY(!header.isEmpty());
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        const QString host = withoutComments(read(QStringLiteral("RoomsPanel.qml")));
        QVERIFY(!presenter.isEmpty());
        QVERIFY(!host.isEmpty());
        QVERIFY2(presenter.contains(QStringLiteral("actionComponent")),
                 "the comment stripper ate the presenter's row chooser");

        const QRegularExpression idRe(QStringLiteral(
            "static QString (\\w+ActionId)\\(\\) \\{ return QStringLiteral\\(\"([^\"]+)\"\\)"));
        QStringList ids;
        auto it = idRe.globalMatch(header);
        while (it.hasNext())
            ids.append(it.next().captured(2));
        QVERIFY2(ids.size() >= 4,
                 "the action ids are no longer readable from the header, so "
                 "this test is pinning nothing");

        for (const QString &id : std::as_const(ids)) {
            QVERIFY2(presenter.contains(QStringLiteral("\"%1\"").arg(id)),
                     qPrintable(QStringLiteral(
                         "the presenter never names %1, so that row renders "
                         "as a control that does nothing").arg(id)));
        }
        const QStringList wired = { QStringLiteral("onCreateRoomRequested"),
                                    QStringLiteral("onCreateChatRequested"),
                                    QStringLiteral("onJoinAddressRequested"),
                                    QStringLiteral("onExploreSpacesRequested") };
        for (const QString &handler : wired) {
            QVERIFY2(host.contains(handler),
                     qPrintable(QStringLiteral("the host never handles %1")
                                    .arg(handler)));
        }
        QVERIFY2(host.contains(QStringLiteral("newConversationDialog.openDialog"))
                     && host.contains(QStringLiteral("discoverJoinDialog.openDialog")),
                 "the command rows open something other than the host's own "
                 "shared dialogs, so there are now two create paths");
        QVERIFY(header.contains(QStringLiteral("IconNameRole")));
        QVERIFY2(presenter.contains(QStringLiteral("rowLoader.model.iconName")),
                 "the presenter hardcodes glyph names instead of reading the "
                 "model's");
    }

    void theChannelsColumnCarriesLobbyRoomsAndMessageSearch()
    {
        const QString presenter =
            read(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY(!presenter.isEmpty());
        QVERIFY2(presenter.contains(QStringLiteral("signal lobbyActivated()")),
                 "the Lobby row does nothing");
        QVERIFY2(presenter.contains(
                     QStringLiteral("signal messageSearchRequested()")),
                 "the Message Search row does nothing");
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QVERIFY(host.contains(QStringLiteral("signal messageSearchRequested()")));
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY2(shell.contains(
                     QStringLiteral("onMessageSearchRequested: messageSearchDialog.openDialog()")),
                 "the Message Search row is wired to nothing that exists");
        const QString controller =
            readSrc(QStringLiteral("app/AppController.cpp"));
        QVERIFY(controller.contains(QStringLiteral("void AppController::openLobby()")));
        const QString model =
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp"));
        QVERIFY2(!model.contains(QStringLiteral("sendTextMessage"))
                     && !model.contains(QStringLiteral("setAccountData")),
                 "the Channels model writes to the account to represent its "
                 "own navigation rows");
    }

    void aFilterThatMatchesNothingSaysSoWithoutClaimingTheAccountIsEmpty()
    {
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        QVERIFY2(presenter.contains(QStringLiteral("app.spaceChannels.empty")),
                 "the comment stripper ate the presenter's empty state, so "
                 "nothing this test asserts about that file is being read");

        QVERIFY2(presenter.contains(QStringLiteral("matchCount === 0")),
                 "the column cannot tell a filter that matched nothing from a "
                 "filter that did nothing, so it renders silence for both");
        QVERIFY2(presenter.contains(
                     QStringLiteral("&& !app.spaceChannels.empty")),
                 "the filter-miss message is not held off an empty account, so "
                 "the two states collide");
        QVERIFY2(presenter.contains(QStringLiteral("filterMode === 3")),
                 "the filter-miss message never mentions the Unreads chip");
        QVERIFY2(presenter.contains(QStringLiteral("searchQuery")),
                 "the filter-miss message never mentions the search box");
        QVERIFY2(presenter.contains(QStringLiteral("viewKind === \"people\"")),
                 "an empty Direct Messages tab is not named, so it reads as "
                 "the account being empty");
        QVERIFY2(presenter.contains(QStringLiteral("viewKind === \"space\"")),
                 "an empty Space is not named, so it reads as the account "
                 "being empty");

        const QString header = withoutComments(
            readSrc(QStringLiteral("models/SpaceChannelModel.h")));
        QVERIFY(!header.isEmpty());
        QVERIFY2(header.contains(QStringLiteral("int matchCount")),
                 "there is no count of what survived the filter, so the "
                 "presenter has nothing to key its message on");
        QVERIFY2(header.contains(QStringLiteral("directsGroupId")),
                 "the Direct Messages tab has no group id for its chats");
        QVERIFY2(header.contains(QStringLiteral("peopleViewId")),
                 "there is no shared name for the rail selection that means "
                 "Direct Messages, so the tab and the view can disagree about "
                 "which string selects which");
    }

    void aDirectMessageIsOnlyInTheDirectMessagesTab()
    {
        QString model = withoutComments(
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp")));
        QVERIFY(!model.isEmpty());
        model.replace(QRegularExpression(QStringLiteral("\\s+")),
                      QStringLiteral(" "));
        QVERIFY2(model.contains(QStringLiteral("int SpaceChannelModel::buildPeople")),
                 "the comment stripper ate the model, so nothing this test "
                 "asserts about it is being read");

        const int home = model.indexOf(
            QStringLiteral("int SpaceChannelModel::buildHome"));
        const int people = model.indexOf(
            QStringLiteral("int SpaceChannelModel::buildPeople"));
        const int space = model.indexOf(
            QStringLiteral("int SpaceChannelModel::buildSpace"));
        QVERIFY(home >= 0 && people > home && space > people);

        QVERIFY2(model.mid(home, people - home)
                     .contains(QStringLiteral("info.isDirect")),
                 "Home does not exclude DMs, so they are listed twice");
        const QString spaceBody = model.mid(space);
        QVERIFY2(spaceBody.contains(QStringLiteral("childInfo->isDirect")),
                 "a Space's view does not exclude DMs");
        QVERIFY2(!spaceBody.contains(QStringLiteral("directsGroupId")),
                 "the account-wide Direct messages group is back inside a "
                 "Space's own view");
        const int appendPeople = model.indexOf(
            QStringLiteral("int SpaceChannelModel::appendSpacePeople"));
        QVERIFY2(appendPeople > space,
                 "the Space People builder is gone, or moved above the Space "
                 "builder where this scan cannot see the call");
        QVERIFY2(model.mid(space, appendPeople - space)
                     .contains(QStringLiteral("appendSpacePeople(")),
                 "a Space view no longer builds its People group, so the "
                 "People filter is scoped in Classic and absent in Channels");
        QVERIFY2(!model.mid(people, space - people)
                      .contains(QStringLiteral("spacePeopleGroupId")),
                 "the Space-scoped People group leaked into the account-wide "
                 "Direct Messages tab");
        QVERIFY(model.mid(people, space - people)
                    .contains(QStringLiteral("directsGroupId")));
    }

    void theRailSelectionNarrowsChannelsRatherThanEnablingIt()
    {
        const QString header =
            readSrc(QStringLiteral("models/SpaceChannelModel.h"));
        QVERIFY(!header.isEmpty());
        const QString clean = withoutComments(header);
        QVERIFY2(clean.contains(QStringLiteral("scopeSpaceId")),
                 "the rail's selection does nothing to the Channels column, so "
                 "clicking a Space is a no-op there");
        QVERIFY2(!clean.contains(QStringLiteral("emptyHierarchy")),
                 "the Channels model still reports one Space's emptiness");
        const QString host = withoutComments(read(QStringLiteral("RoomsPanel.qml")));
        QString flat = host;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("property: \"scopeSpaceId\"")),
                 "the scope is never bound to anything");
        QVERIFY2(flat.contains(QStringLiteral(
                     "readonly property bool channelsUsable: channelsChosen")),
                 "the scope decides whether the layout renders again");
        const QString model =
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp"));
        const int at = model.indexOf(
            QStringLiteral("void SpaceChannelModel::setScopeSpaceId"));
        QVERIFY(at >= 0);
        const QString setter = model.mid(at, 900);
        QVERIFY2(setter.contains(QStringLiteral("QLatin1Char('!')")),
                 "a pseudo id would scope the column to nothing at all");
        QVERIFY2(setter.contains(QStringLiteral("peopleViewId()")),
                 "the setter cannot tell the Direct Messages tab from Home, so "
                 "selecting it produces the Home view");
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("roomNavigationLayout === 1")),
                 "the Direct Messages tab is offered in Classic too");
        QVERIFY2(rail.contains(QStringLiteral("peopleEntryVisible")),
                 "the rail never tells its model whether to draw the tab");
        QVERIFY2(rail.contains(QStringLiteral("orphansEntryVisible")),
                 "the rail never tells its model whether to draw \"Other "
                 "rooms\", so it is offered in Channels where it duplicates "
                 "Home");
        QVERIFY2(rail.contains(QStringLiteral("app.spaces.activeSpaceId === \"@orphans\"")),
                 "switching to Channels leaves the selection on a tile that "
                 "no longer exists");
        const QString channelsModel = withoutComments(
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp")));
        const int home = channelsModel.indexOf(
            QStringLiteral("int SpaceChannelModel::buildHome"));
        QVERIFY(home >= 0);
        const int homeEnd = channelsModel.indexOf(
            QStringLiteral("int SpaceChannelModel::build"), home + 1);
        QVERIFY2(channelsModel.mid(home, homeEnd - home)
                     .contains(QStringLiteral("roomInAnySpace")),
                 "Channels' Home no longer skips rooms that belong to a "
                 "Space, so \"Other rooms\" is not redundant any more and "
                 "hiding its tile drops a real view");
        QVERIFY2(!clean.contains(QStringLiteral("RoomListModel")),
                 "the Channels model reads the Space-scoped room list");
        QVERIFY(clean.contains(QStringLiteral("MatrixClient *client")));
    }

    void aChannelRowShowsTheRoomsAvatar()
    {
        const QString row = read(QStringLiteral("ChannelDelegate.qml"));
        QVERIFY(!row.isEmpty());
        QVERIFY2(row.contains(QStringLiteral("Avatar {")),
                 "a channel row draws no avatar, so every room in a Space "
                 "looks identical");
        QVERIFY2(row.contains(QStringLiteral("mxc: root.inviteAvatarWithheld ? \"\" : root.avatarUrl")),
                 "the avatar is never given the room's own picture");
        const int gate = row.indexOf(QStringLiteral("property bool inviteAvatarWithheld:"));
        QVERIFY2(gate > 0, "the invite-avatar gate is gone");
        const QString gateBody = row.mid(gate, row.indexOf(QLatin1Char(')'), gate) - gate);
        QVERIFY2(gateBody.contains(QStringLiteral("root.isInvite")),
                 "the avatar gate is not limited to invitations, so joined "
                 "rooms lose their picture");
        const QString presenter =
            read(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY2(presenter.contains(QStringLiteral("avatarUrl: rowLoader.model.avatarUrl")),
                 "the presenter never passes the avatar down");
        QVERIFY(row.contains(QStringLiteral("\"lock\"")));
    }

    void mutingASpaceMutesTheRoomsInsideIt()
    {
        const QString controller =
            readSrc(QStringLiteral("app/AppController.cpp"));
        const int at = controller.indexOf(
            QStringLiteral("void AppController::setSpaceMuted"));
        QVERIFY2(at >= 0, "there is no way to mute a whole Space");
        QString body = withoutComments(controller.mid(at, 1400));
        body.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(body.contains(QStringLiteral("roomsInSpace(spaceId)")),
                 "muting a Space does not reach the rooms in it");
        QVERIFY2(body.contains(QStringLiteral("setRoomNotificationMode(roomId, mode)")),
                 "muting a Space does not go through the one per-room path, "
                 "so its writes cannot report or retry like every other one");
        QVERIFY2(body.contains(QStringLiteral("mute ? 2 : 3")),
                 "unmuting a Space asserts a mode rather than undoing one");
        const QString rail = read(QStringLiteral("SpacesRail.qml"));
        QVERIFY(rail.contains(QStringLiteral("objectName: \"railMuteSpace\"")));
    }

    void theThemeEditorPreviewsWhicheverLayoutIsChosen()
    {
        const QString preview = read(QStringLiteral("ThemePreviewDemo.qml"));
        QVERIFY(!preview.isEmpty());
        QVERIFY2(preview.contains(QStringLiteral("property bool channels:")),
                 "the theme preview can only draw the Classic column");
        QVERIFY2(preview.contains(QStringLiteral("fakeChannelRows")),
                 "the theme preview has no Channels shape to draw");
        const QString editor = read(QStringLiteral("ThemeEditorDialog.qml"));
        QVERIFY2(editor.contains(QStringLiteral("roomNavigationLayout === 1")),
                 "the editor never tells the preview which layout to draw");
        const QString clean = withoutComments(preview);
        QVERIFY2(!clean.contains(QStringLiteral("app.spaceChannels")),
                 "the theme preview binds to the real Channels model");
        QVERIFY2(!clean.contains(QStringLiteral("app.roomList")),
                 "the theme preview binds to the real room list");
    }

    void subspacesAreNeverNestedInTheChannelsColumn()
    {
        const QString model =
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp"));
        QVERIFY(!model.isEmpty());
        QString flat = withoutComments(model);
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("directChildRoomIds"))
                     || flat.contains(QStringLiteral("directChildRoomsDetailed")),
                 "a Space folder does not list its DIRECT children");
        QVERIFY2(!flat.contains(QStringLiteral("childRoomsDetailed(spaceId)")),
                 "a Space folder lists the TRANSITIVE tree, so a subspace's "
                 "rooms appear under it and under its parent");
        QVERIFY2(!flat.contains(QStringLiteral("childSpacesDetailed")),
                 "child Spaces are being turned into nested categories again");
    }

    void theRailRevealsOnlyDirectChildRooms()
    {
        const QString rail =
            withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("directChildRoomsDetailed(")),
                 "the rail does not ask for a Space's direct children at all");
        QVERIFY2(!rail.contains(QStringLiteral("spaces.childRoomsDetailed(")),
                 "the rail reveals a Space's TRANSITIVE rooms, so every "
                 "subspace's rooms appear under it AND under each of its "
                 "ancestors");
        const QString manager = withoutComments(
            readSrc(QStringLiteral("spaces/SpaceManager.cpp")));
        QVERIFY2(manager.contains(
                     QStringLiteral("SpaceManager::childRoomsDetailed(")),
                 "the transitive accessor was deleted rather than left to "
                 "the surfaces that legitimately want the whole subtree");
    }

    void theRailDragLivesInAModelSoTheRowsCanMove()
    {
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("model: app.railEntries")),
                 "the rail still binds its list to a JavaScript array");
        QVERIFY2(!rail.contains(QStringLiteral("app.railLayout.arrange(")),
                 "the rail arranges its own rows in QML again");
        QVERIFY2(rail.contains(QStringLiteral("move: Transition")),
                 "the rail has no move transition, so a reorder cannot animate");
        QVERIFY2(rail.contains(QStringLiteral("displaced: Transition")),
                 "the rows the moved one pushed past do not animate");
        const QString model = readSrc(QStringLiteral("spaces/RailEntryModel.cpp"));
        QVERIFY(model.contains(QStringLiteral("beginMoveRows")));
        QString flatModel = withoutComments(model);
        flatModel.replace(QRegularExpression(QStringLiteral("\\s+")),
                          QStringLiteral(" "));
        QVERIFY2(flatModel.contains(QStringLiteral("if (m_dragging) {")),
                 "a refresh during a drag is applied rather than deferred");
    }

    void theDraggedTileFollowsThePointerAtFullOpacity()
    {
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY2(rail.contains(QStringLiteral("readonly property real dragLift:")),
                 "the dragged tile does not follow the pointer");
        QVERIFY2(rail.contains(QStringLiteral("+ spaceItem.dragLift")),
                 "the lift is computed but never applied to the tile");
        QVERIFY2(!rail.contains(QStringLiteral("railInsertionLine")),
                 "the insertion line came back");
        QVERIFY2(!rail.contains(QStringLiteral("railDragProxy")),
                 "the floating drag copy came back, so the tile is drawn twice");
        QVERIFY2(!rail.contains(QStringLiteral("opacity: spaceItem.dragged")),
                 "the dragged tile is dimmed again");
        QVERIFY2(rail.contains(QStringLiteral("function readingAt(")),
                 "the pointer reading is not one total function any more");
        QVERIFY2(rail.contains(QStringLiteral("dropTarget")),
                 "a release would group with nothing saying so");
        QVERIFY2(rail.contains(QStringLiteral("autoScroll")),
                 "a rail longer than the window cannot be dragged across");
    }

    void releasingADragAnnouncesTheClearedFlags()
    {
        const QString model = readSrc(QStringLiteral("spaces/RailEntryModel.cpp"));
        QVERIFY(!model.isEmpty());
        const int at = model.indexOf(QStringLiteral("void RailEntryModel::endDrag"));
        QVERIFY(at >= 0);
        QString body = model.mid(at, 1800);
        body.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(body.contains(QStringLiteral("{ DraggedRole, DropTargetRole }")),
                 "endDrag clears the drag flags without announcing them, so a "
                 "released tile keeps rendering as dragged");
    }

    void aLocalFolderNeverTouchesMatrixState()
    {
        const QString store = readSrc(QStringLiteral("spaces/RailLayoutStore.cpp"));
        const QString model = readSrc(QStringLiteral("spaces/RailEntryModel.cpp"));
        QVERIFY(!store.isEmpty());
        QVERIFY(!model.isEmpty());
        for (const QString &source : { store, model }) {
            const QString clean = withoutComments(source);
            for (const QString &banned :
                 { QStringLiteral("m.space.child"),
                   QStringLiteral("m.space.parent"),
                   QStringLiteral("addRoomToSpace"),
                   QStringLiteral("setSpaceChildSuggested"),
                   QStringLiteral("sendStateEvent") }) {
                QVERIFY2(!clean.contains(banned),
                         qPrintable(QStringLiteral("the rail's local layout "
                                                   "reaches Matrix state via ")
                                        + banned));
            }
        }
        QVERIFY(model.contains(QStringLiteral("hierarchyChild")));
    }

    void classicIsTheDefaultAndTheClampTarget()
    {
        const QString manager =
            readSrc(QStringLiteral("app/SettingsManager.cpp"));
        QVERIFY(!manager.isEmpty());
        const int at = manager.indexOf(
            QStringLiteral("SettingsManager::roomNavigationLayout"));
        QVERIFY2(at >= 0, "roomNavigationLayout has no accessor");
        const QString accessor = withoutComments(manager.mid(at, 900));
        QVERIFY2(accessor.contains(QStringLiteral("kRoomNavLayout, 0")),
                 "the default is not Classic");
        QVERIFY2(accessor.contains(QStringLiteral("? 0 :")),
                 "an out-of-range value does not clamp to Classic");
    }

    void theSettingIsAccountScoped()
    {
        const QString manager =
            readSrc(QStringLiteral("app/SettingsManager.cpp"));
        const int at = manager.indexOf(
            QStringLiteral("SettingsManager::roomNavigationLayout"));
        QVERIFY(at >= 0);
        QVERIFY2(manager.mid(at, 400).contains(
                     QStringLiteral("appearanceValue")),
                 "the layout is not account-scoped");
        QVERIFY2(manager.contains(
                     QStringLiteral("Q_EMIT roomNavigationLayoutChanged();")),
                 "the layout is never announced");
        const int switchAt =
            manager.indexOf(QStringLiteral("Q_EMIT messageLayoutChanged();"));
        QVERIFY(switchAt >= 0);
        QVERIFY2(manager.mid(switchAt, 200)
                     .contains(QStringLiteral("roomNavigationLayoutChanged")),
                 "the layout is not re-announced on an account switch");
    }

    void everyChannelsTokenIsDerivedFromAnExistingOne()
    {
        const QString theme = read(QStringLiteral("AppTheme.qml"));
        QVERIFY(!theme.isEmpty());
        const QStringList tokens = {
            QStringLiteral("channelCategoryText"),
            QStringLiteral("channelText"),
            QStringLiteral("channelTextUnread"),
            QStringLiteral("channelSelected"),
            QStringLiteral("channelSelectedText"),
            QStringLiteral("channelHover"),
            QStringLiteral("channelUnreadMark"),
            QStringLiteral("railFolderSurface"),
        };
        QString flat = theme;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        for (const QString &token : tokens) {
            const QString declaration =
                QStringLiteral("readonly property color ") + token + ":";
            QVERIFY2(flat.contains(declaration),
                     qPrintable(token + " is not declared"));
            const int at = flat.indexOf(declaration);
            const QString body = flat.mid(at, 220);
            QVERIFY2(body.contains(QStringLiteral("!== undefined")),
                     qPrintable(token + " does not fall back when a palette "
                                        "omits it"));
        }
    }

    void theChannelRowNeverClaimsUnknownEncryption()
    {
        const QString manager =
            readSrc(QStringLiteral("spaces/SpaceManager.cpp"));
        const int at = manager.indexOf(
            QStringLiteral("SpaceManager::directChildRoomsDetailed"));
        QVERIFY2(at >= 0, "there is no direct-children accessor");
        QString body = manager.mid(at, 2000);
        body.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(body.contains(
                     QStringLiteral("it->encrypted && it->encryptionKnown")),
                 "encryption is reported without checking it is known");
    }

    void everyEmptyCapableLabelInTheNewRowsSitsBehindALoader()
    {
        for (const QString &name : { QStringLiteral("ChannelDelegate.qml"),
                                     QStringLiteral("ChannelCategoryHeader.qml"),
                                     QStringLiteral("ChannelNavRow.qml"),
                                     QStringLiteral("FolderTile.qml") }) {
            const QString source = read(name);
            QVERIFY2(!source.isEmpty(), qPrintable(name));
            QString flat = source;
            flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                         QStringLiteral(" "));
            const int labels = flat.count(QStringLiteral("Label {"));
            const int wrapped =
                flat.count(QStringLiteral("sourceComponent: Label {"));
            QVERIFY2(labels == wrapped,
                     qPrintable(QStringLiteral("%1 has %2 Labels but only %3 "
                                               "behind a Loader")
                                    .arg(name)
                                    .arg(labels)
                                    .arg(wrapped)));
        }
    }

    void theUnreadPillIsSharedRatherThanReimplemented()
    {
        QVERIFY(!read(QStringLiteral("UnreadBadge.qml")).isEmpty());
        QVERIFY(read(QStringLiteral("ChannelDelegate.qml"))
                    .contains(QStringLiteral("UnreadBadge {")));
        QVERIFY(read(QStringLiteral("ChannelCategoryHeader.qml"))
                    .contains(QStringLiteral("UnreadBadge {")));
        const QString rail = read(QStringLiteral("SpacesRail.qml"));
        QVERIFY(!read(QStringLiteral("FolderTile.qml")).isEmpty());
        QVERIFY(rail.contains(QStringLiteral("FolderTile {")));
        const QString badge = read(QStringLiteral("UnreadBadge.qml"));
        QVERIFY(badge.contains(QStringLiteral(
            "root.mention ? AppTheme.mentionBadgeInk : root.muted")));
        QVERIFY(badge.contains(QStringLiteral(
            "root.mention ? AppTheme.roomMentionBadge : root.muted")));
    }

    void aCollapsedCategoryStillReportsWhatItHides()
    {
        const QString header = read(QStringLiteral("ChannelCategoryHeader.qml"));
        QVERIFY(header.contains(QStringLiteral("hiddenHighlight")));
        QVERIFY(header.contains(QStringLiteral("hiddenUnread")));
        QString flat = header;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("active: root.collapsed")),
                 "the hidden-activity indicator is not tied to collapse");
    }

    void aMutedChannelKeepsItsUnreadWeightButLosesItsPill()
    {
        const QString row = read(QStringLiteral("ChannelDelegate.qml"));
        QString flat = row;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("showsPill:")),
                 "the pill predicate is gone");
        const int pillAt = flat.indexOf(QStringLiteral("showsPill:"));
        const QString pill = flat.mid(pillAt, 220);
        QVERIFY2(pill.contains(QStringLiteral("!root.muted")),
                 "a muted channel is being counted at again");
        QVERIFY2(pill.contains(QStringLiteral("root.unreadCount > 0"))
                     || pill.contains(QStringLiteral("root.hasUnread")),
                 "the pill is mentions-only again, so an ordinary unread "
                 "conversation shows nothing but a 3px bar");
        QVERIFY(row.contains(QStringLiteral("onRoomIdChanged")));
        QVERIFY(row.contains(QStringLiteral("refreshNotificationMode")));
    }

    void theRowChooserNamesEveryRowKindTheModelCanProduce()
    {
        const QString presenter =
            read(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY(!presenter.isEmpty());
        for (const QString &kind : { QStringLiteral("lobby"),
                                     QStringLiteral("search"),
                                     QStringLiteral("space"),
                                     QStringLiteral("group") }) {
            QVERIFY2(presenter.contains(QStringLiteral("=== \"") + kind
                                        + QStringLiteral("\"")),
                     qPrintable(QStringLiteral("the chooser does not name the ")
                                + kind + QStringLiteral(" kind")));
        }
        QVERIFY(presenter.contains(QStringLiteral("ChannelNavRow {")));
        QVERIFY(presenter.contains(QStringLiteral("ChannelCategoryHeader {")));
        QVERIFY(presenter.contains(QStringLiteral("ChannelDelegate {")));
        const QString model =
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp"));
        for (const QString &kind : { QStringLiteral("lobby"),
                                     QStringLiteral("search"),
                                     QStringLiteral("group"),
                                     QStringLiteral("space"),
                                     QStringLiteral("room") }) {
            QVERIFY2(model.contains(QStringLiteral("QStringLiteral(\"") + kind
                                    + QStringLiteral("\")")),
                     qPrintable(kind));
        }
    }

    void bothLayoutsRowsUseTheOneSharedActionsMenu()
    {
        const QString classicRow = read(QStringLiteral("RoomDelegate.qml"));
        const QString channelRow = read(QStringLiteral("ChannelDelegate.qml"));
        QVERIFY(!classicRow.isEmpty());
        QVERIFY(!channelRow.isEmpty());
        QVERIFY(classicRow.contains(QStringLiteral("RoomActionsMenu {")));
        QVERIFY2(channelRow.contains(QStringLiteral("RoomActionsMenu {")),
                 "the Channels row has no actions menu");
        QVERIFY(!classicRow.contains(QStringLiteral("roomFavouriteItem")));
        QVERIFY(!channelRow.contains(QStringLiteral("roomFavouriteItem")));
        for (const QString &sig : { QStringLiteral("signal markRead()"),
                                    QStringLiteral("signal markUnread()"),
                                    QStringLiteral("signal setFavourite(bool on)"),
                                    QStringLiteral("signal setNotificationMode(int mode)"),
                                    QStringLiteral("signal copyRoomLink()"),
                                    QStringLiteral("signal leaveRoomRequested()") }) {
            QVERIFY2(channelRow.contains(sig), qPrintable(sig));
        }
        QVERIFY(!channelRow.contains(QStringLiteral("app.roomList.setRoomFavourite")));
        QVERIFY(!channelRow.contains(QStringLiteral("app.setRoomNotificationMode")));
    }

    void theRoomFilterChipsScrollInsteadOfOverflowingTheColumn()
    {
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        const QString control = read(QStringLiteral("SegmentedControl.qml"));
        QVERIFY(!host.isEmpty());
        QVERIFY(!control.isEmpty());
        QVERIFY2(host.contains(QStringLiteral("contentWidth: Math.max(filterChips.implicitWidth, width)")),
                 "the room-list filter chips do not scroll at their natural width");
        QVERIFY(control.contains(QStringLiteral("RowLayout {")));
        QVERIFY(control.contains(QStringLiteral("Layout.fillWidth: root.overflowing")));
        QVERIFY(control.contains(QStringLiteral("Layout.maximumWidth: implicitWidth")));
        QVERIFY(control.contains(QStringLiteral("fitWidth && width > 0 && implicitWidth > width")));
        QVERIFY(control.contains(QStringLiteral("elide: Text.ElideRight")));
    }

    void theFilterChipsReadTheSettingTheyWrite()
    {
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QVERIFY(!host.isEmpty());
        QVERIFY2(host.contains(QStringLiteral("app.settings.roomFilterMode")),
                 "the chips no longer read the setting they write");
        QVERIFY2(!host.contains(QStringLiteral("current: app.roomList.filterMode")),
                 "the chips report the model while every click writes the "
                 "setting, so any moment the two disagree shows a filter the "
                 "user did not choose");
        QVERIFY(host.contains(QStringLiteral("app.settings.roomFilterMode = value")));

        QString flat = withoutComments(host);
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("readonly property int channelsFilterMode")),
                 "the shared Channels filter mapping is gone");
        QVERIFY2(flat.contains(QStringLiteral(
                     "current: channelsLayout ? filterChips.channelsFilterMode "
                     ": app.settings.roomFilterMode")),
                 "the chip row does not read the shared mapping, so a stored "
                 "People/Rooms value selects no chip at all");
        QVERIFY2(flat.contains(QStringLiteral(
                     "property: \"filterMode\" value: filterChips.channelsFilterMode")),
                 "the channel model carries its own copy of the mapping again, "
                 "which is what made the People chip inert");
        QVERIFY2(!flat.contains(QStringLiteral("value: app.settings.roomFilterMode === 3 ? 3 : 0")),
                 "a second copy of the mapping is back");
        QVERIFY2(!flat.contains(QStringLiteral("app.settings.roomFilterMode = 0")),
                 "the stored filter is rewritten when Channels drops its chip, "
                 "so returning to Classic loses the user's choice");
    }

    void theRoomOrderMirrorsTheSdkRoomListAndNothingElse()
    {
        QFile file(QStringLiteral(SRC_DIR "/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY(!source.isEmpty());

        const int spaces =
            source.indexOf(QStringLiteral("void RustSdkMatrixClient::handleSpacesEvent"));
        QVERIFY(spaces > 0);
        const int nextFunction =
            source.indexOf(QStringLiteral("\nvoid RustSdkMatrixClient::"),
                           spaces + 10);
        const QString body = source.mid(
            spaces, (nextFunction > spaces ? nextFunction : source.size()) - spaces);
        QVERIFY(!body.isEmpty());
        QVERIFY2(!body.contains(QStringLiteral("m_roomOrder.append")),
                 "handleSpacesEvent appends to the ordered room list again — "
                 "every SDK diff index after that point addresses the wrong "
                 "room, and each rejection triggers a full resync");
        QVERIFY2(!body.contains(QStringLiteral("m_roomOrder.insert")),
                 "handleSpacesEvent inserts into the ordered room list");

        const int snapshot =
            source.indexOf(QStringLiteral("void RustSdkMatrixClient::handleRoomsEvent"));
        QVERIFY(snapshot > 0);
        const int afterSnapshot =
            source.indexOf(QStringLiteral("\nvoid RustSdkMatrixClient::"),
                           snapshot + 10);
        const QString snapBody = source.mid(
            snapshot,
            (afterSnapshot > snapshot ? afterSnapshot : source.size()) - snapshot);
        QVERIFY2(snapBody.contains(QStringLiteral("rust_rooms::")),
                 "the room-list snapshot handler no longer delegates to the "
                 "registry: it is rebuilding the room map itself, so the "
                 "Spaces-survive rule and the index-space rule are now in two "
                 "places and only one of them is tested");
        QVERIFY2(!snapBody.contains(QStringLiteral("m_rooms.clear()")),
                 "the snapshot handler clears the room map directly, which is "
                 "what deleted the Spaces from the hierarchy");
    }

    void theChannelsPresenterDrawsNoSecondGrouping()
    {
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        QVERIFY2(!presenter.isEmpty(),
                 "RoomChannelsPresenter.qml could not be read — this contract "
                 "would otherwise pass vacuously");
        QVERIFY2(!presenter.contains(QStringLiteral("section.property")),
                 "the Channels list adds a second grouping mechanism");
    }

    void hidingAnImageIsLocalAndKeepsTheRowsGeometry()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const QString clean = withoutComments(delegate);
        QVERIFY2(clean.contains(QStringLiteral("app.mediaVisibility")),
                 "the hidden flag is not keyed through the store, so it is "
                 "lost the moment the row is recycled");
        QVERIFY2(clean.contains(QStringLiteral("MediaHiddenPlaceholder {")),
                 "there is no geometry-preserving placeholder");
        const QString placeholder = withoutComments(
            read(QStringLiteral("MediaHiddenPlaceholder.qml")));
        QVERIFY(!placeholder.isEmpty());
        QVERIFY2(!placeholder.contains(QStringLiteral("implicitWidth:"))
                     && !placeholder.contains(QStringLiteral("implicitHeight:")),
                 "the placeholder contributes its own implicit size, so the "
                 "row resizes when an image is hidden");
        for (const QString &banned : { QStringLiteral("composer.redact"),
                                       QStringLiteral("beginEdit"),
                                       QStringLiteral("setAccountData") }) {
            const int at = clean.indexOf(QStringLiteral("setMediaHidden"));
            QVERIFY(at >= 0);
            QVERIFY2(!clean.mid(at, 400).contains(banned), qPrintable(banned));
        }
        const QString store =
            readSrc(QStringLiteral("media/MediaVisibilityStore.cpp"));
        QVERIFY(!store.isEmpty());
        QVERIFY2(!store.contains(QStringLiteral("MatrixClient")),
                 "the hidden-image store reaches the Matrix client, so it is "
                 "no longer purely local rendering state");
        QVERIFY2(!store.contains(QStringLiteral("AccountData"))
                     && !store.contains(QStringLiteral("account_data")),
                 "hidden images are being written to Matrix account data");
        const QString settings =
            readSrc(QStringLiteral("app/SettingsManager.cpp"));
        QVERIFY(!settings.isEmpty());
        const int at = settings.indexOf(
            QStringLiteral("void SettingsManager::setHiddenMediaKeys"));
        QVERIFY2(at >= 0, "the hidden-media setter is gone");
        const int end = settings.indexOf(QStringLiteral("\n}\n"), at);
        QVERIFY(end > at);
        const QString body = settings.mid(at, end - at);
        QVERIFY2(body.contains(QStringLiteral("slugForSavedAccount")),
                 "hidden images are not scoped to the account that hid them");
        QVERIFY2(!body.contains(QStringLiteral("appearanceValue")),
                 "hidden images use the mirroring accessor, so one account "
                 "would inherit another's");
        QVERIFY(clean.contains(QStringLiteral(
            "visible: root.mediaHideable && !root.mediaHidden")));
        QVERIFY(placeholder.contains(QStringLiteral("Show image")));
        QVERIFY2(placeholder.contains(QStringLiteral("active: root.hidden")),
                 "the placeholder's contents are built for every media row");
    }

    void settingsOffersBothLayoutsAsPreviews()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral("navLayoutClassicCard")));
        QVERIFY(settings.contains(QStringLiteral("navLayoutChannelsCard")));
        const QString card = withoutComments(
            read(QStringLiteral("NavigationLayoutCard.qml")));
        QVERIFY(!card.isEmpty());
        QVERIFY2(!card.contains(QStringLiteral("app.roomList")),
                 "the preview card binds to the real room list");
        QVERIFY2(!card.contains(QStringLiteral("RoomListClassicPresenter")),
                 "the preview card instantiates a real presenter");
    }

    void theRailNeverReordersIntoTheTileTheDragIsAimingAt()
    {
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("function rowIsDraggedBlock(")),
                 "the dragged block's own slot is treated as a droppable row");

        QVERIFY2(!rail.contains(QStringLiteral("function pointerPushedThrough(")),
                 "the arrival-side midpoint rule is back");
        QVERIFY2(!rail.contains(QStringLiteral("function pointerOverTileCentre(")),
                 "the centre-band rule is back; reaching that band means "
                 "crossing the near edge first, which reorders");
        QVERIFY2(!rail.contains(QStringLiteral("dwellTimer")),
                 "the dwell is back — it existed to compensate for a reading "
                 "that moved things while the user was still aiming, and that "
                 "reading is gone");

        const int at = rail.indexOf(QStringLiteral("function applyPointerReading("));
        QVERIFY2(at > 0, "the single pointer dispatch is gone, so the "
                         "auto-scroll can reorder behind the pointer's back");
        const int end = rail.indexOf(QStringLiteral("function updateTileDrag("), at);
        QVERIFY(end > at);
        const QString dispatch = rail.mid(at, end - at);
        const int rowBranch = dispatch.indexOf(QStringLiteral("reading.row !== undefined"));
        QVERIFY(rowBranch > 0);
        const int gapCall = dispatch.indexOf(QStringLiteral("hoverGap("));
        QVERIFY2(gapCall > rowBranch,
                 "the tile branch reorders — that is the original defect");
        QVERIFY2(dispatch.contains(QStringLiteral("hoverGroup(")),
                 "the tile branch does not arm grouping at all");
        QVERIFY2(dispatch.contains(QStringLiteral("clearDropTarget()")),
                 "the dragged block's own slot does not disarm a stale target");

        const int scroll = rail.indexOf(QStringLiteral("id: autoScroll"));
        QVERIFY(scroll > 0);
        const QString scrollBody = rail.mid(scroll, 1400);
        QVERIFY2(scrollBody.contains(QStringLiteral("applyPointerReading(")),
                 "the auto-scroll has its own drag dispatch again");

        const QString model = readSrc(QStringLiteral("spaces/RailEntryModel.h"));
        QVERIFY2(model.contains(QStringLiteral("void hoverGroup(int row)")),
                 "the model cannot be told the pointer is on a tile");
        QVERIFY2(model.contains(QStringLiteral("void hoverGap(int gap)")),
                 "the model cannot be told the pointer is in a gap");
        QVERIFY2(model.contains(QStringLiteral("void clearDropTarget()")),
                 "the model has no way to clear a target without reordering");
        QVERIFY2(!model.contains(QStringLiteral("void updateDrag(")),
                 "the one-verb-with-a-flag API is back, and its false branch "
                 "reorders into the row the pointer is aiming at");
    }

    void theRailSpaceMenuCarriesTheSpaceActions()
    {
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        for (const auto *name : { "railMarkSpaceRead", "railMuteSpace",
                                  "railSpaceInvite", "railSpaceCopyLink",
                                  "railSpaceShareLink", "railSpaceSettings" }) {
            QVERIFY2(rail.contains(QLatin1String(name)),
                     qPrintable(QStringLiteral("the Space menu lost %1")
                                    .arg(QLatin1String(name))));
        }
        QVERIFY2(rail.contains(QStringLiteral("contextLabel:")),
                 "the menu no longer names the Space it acts on");
        QVERIFY2(rail.contains(QStringLiteral("roomPermalink(")),
                 "the shared link is not the room permalink");

        QVERIFY2(rail.contains(QStringLiteral("railMarkAllRoomsRead")),
                 "the Home menu has no way to mark everything read");
        const int sweep = rail.indexOf(QStringLiteral("railMarkAllRoomsRead"));
        const QString sweepItem = rail.mid(sweep, 500);
        QVERIFY2(sweepItem.contains(
                     QStringLiteral("app.roomList.markAllRoomsRead()")),
                 "the sweep does not go through the room list");
        QVERIFY2(sweepItem.contains(QStringLiteral("railMenu.spaceId === \"\"")),
                 "the account-wide sweep is offered on a Space tile, where it "
                 "would silently mark rooms outside it");
        QVERIFY2(sweepItem.contains(QStringLiteral("enabled:")),
                 "the sweep is offered with nothing unread to sweep");
    }

    void aHiddenMenuRowTakesNoHeight()
    {
        const QString sep = read(QStringLiteral("AppMenuSeparator.qml"));
        QVERIFY2(sep.contains(QStringLiteral("implicitHeight: visible ?")),
                 "a hidden separator still reserves its own height");
        const QString item = read(QStringLiteral("AppMenuItem.qml"));
        QVERIFY2(item.contains(QStringLiteral("implicitHeight: visible ?")),
                 "a hidden menu item still reserves its own height");
    }

    void spaceSettingsWritesRoomStateAndNothingElse()
    {
        const QString dialog = withoutComments(
            read(QStringLiteral("SpaceSettingsDialog.qml")));
        QVERIFY(!dialog.isEmpty());
        for (const auto *call : { "setRoomName(", "setRoomTopic(",
                                  "setRoomAvatar(", "setJoinRule(",
                                  "setCanonicalAlias(" }) {
            QVERIFY2(dialog.contains(QLatin1String(call)),
                     qPrintable(QStringLiteral("space settings lost %1")
                                    .arg(QLatin1String(call))));
        }
        QVERIFY2(!dialog.contains(QStringLiteral("app.settings")),
                 "space settings writes device-local state and presents it as "
                 "part of the Space");
        QVERIFY2(!dialog.contains(QStringLiteral("app.railLayout")),
                 "space settings writes the local rail arrangement");
        QVERIFY2(dialog.contains(QStringLiteral("knock_restricted")),
                 "a space-restricted join rule is no longer detected, so this "
                 "surface would offer to overwrite it with an empty allow list");
        QVERIFY2(!dialog.contains(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}"))),
                 "space settings hardcodes a colour");
    }

    void theRailActivityIndicatorsAreWiredToTheRoomList()
    {
        const QString controller =
            withoutComments(readSrc(QStringLiteral("app/AppController.cpp")));
        QVERIFY(!controller.isEmpty());
        QVERIFY2(controller.contains(QStringLiteral(
                     "m_railEntries->setRoomSources(m_client.get(), "
                     "m_settings.get())")),
                 "RailEntryModel is never given the room list, so no Space "
                 "tile can show unread activity");

        const QString rail =
            withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY2(rail.contains(QStringLiteral("required property bool hasUnread")),
                 "the rail delegate does not declare hasUnread; an undeclared "
                 "role reads as undefined");
        QVERIFY2(rail.contains(QStringLiteral("required property int mentionCount")),
                 "the rail delegate does not declare mentionCount");
        const int badge = rail.indexOf(QStringLiteral("\"railMentionBadge\""));
        QVERIFY2(badge >= 0, "the rail has no mention badge");
        QVERIFY2(rail.mid(badge, 200).contains(
                     QStringLiteral("spaceItem.mentionCount > 0")),
                 "the mention badge is not keyed on mentionCount");
        const int dot = rail.indexOf(QStringLiteral("\"railUnreadDot\""));
        QVERIFY2(dot >= 0, "the rail has no unread dot");
        QVERIFY2(rail.mid(dot, 200).contains(QStringLiteral("spaceItem.hasUnread")),
                 "the unread dot is not keyed on hasUnread");
    }
};

QTEST_MAIN(NavigationLayoutContractTest)
#include "NavigationLayoutContractTest.moc"
