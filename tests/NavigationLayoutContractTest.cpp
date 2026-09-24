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

/// The file with its comments removed. Ban assertions must scan this, never
/// the raw source, or a comment explaining the ban trips it.
QString withoutComments(const QString &source)
{
    QString out = source;
    // Block comments first, so a `//` inside one is not treated as a line.
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

} // namespace

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
        // The workspace header, search field and create/discover dialogs
        // belong to the host.
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

    void noPresenterReachesUpIntoTheHostById()
    {
        // These host ids must not be resolved by scope from inside a
        // presenter delegate.
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
        // ...and it asks by signal instead.
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
                     "active: !parent.channelsUsable")),
                 "the Classic presenter is not gated on the layout choice");
        QVERIFY2(flat.contains(QStringLiteral(
                     "active: parent.channelsUsable")),
                 "the Channels presenter is not gated on the layout choice");
    }

    void channelsIsGlobalAndNeverFallsBackToClassic()
    {
        // Channels renders at Home too; the host never falls back to Classic.
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
        // Settings must not promise the old fallback either.
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY2(!settings.contains(QStringLiteral("always use Classic")),
                 "Settings still tells the user Channels falls back to "
                 "Classic");
    }

    // Every command row the model can produce is dispatched by the presenter
    // and wired by the host to a dialog that exists.
    void everyChannelActionIsDispatched()
    {
        const QString header = readSrc(QStringLiteral("models/SpaceChannelModel.h"));
        QVERIFY(!header.isEmpty());
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        const QString host = withoutComments(read(QStringLiteral("RoomsPanel.qml")));
        QVERIFY(!presenter.isEmpty());
        QVERIFY(!host.isEmpty());
        // Sanity: the stripper must still see the chooser.
        QVERIFY2(presenter.contains(QStringLiteral("actionComponent")),
                 "the comment stripper ate the presenter's row chooser");

        // The ids come from the header's own accessors, so this cannot pin a
        // stale set.
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
        // ...and the presenter's signals reach a real dialog in the host.
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
        // The model names each row's glyph, so there is one place to check
        // against the bundled icon subset.
        QVERIFY(header.contains(QStringLiteral("IconNameRole")));
        QVERIFY2(presenter.contains(QStringLiteral("rowLoader.model.iconName")),
                 "the presenter hardcodes glyph names instead of reading the "
                 "model's");
    }

    void theChannelsColumnCarriesLobbyRoomsAndMessageSearch()
    {
        // Channels is navigable on its own: Home, the rooms no Space lists,
        // and a real search.
        const QString presenter =
            read(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY(!presenter.isEmpty());
        QVERIFY2(presenter.contains(QStringLiteral("signal lobbyActivated()")),
                 "the Lobby row does nothing");
        QVERIFY2(presenter.contains(
                     QStringLiteral("signal messageSearchRequested()")),
                 "the Message Search row does nothing");
        // Message Search opens the existing global search via the host, never
        // a filter over this list or a second dialog.
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        QVERIFY(host.contains(QStringLiteral("signal messageSearchRequested()")));
        const QString shell = read(QStringLiteral("MainScreen.qml"));
        QVERIFY2(shell.contains(
                     QStringLiteral("onMessageSearchRequested: messageSearchDialog.openDialog()")),
                 "the Message Search row is wired to nothing that exists");
        // Lobby is navigation, not a fabricated room.
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

    // A filter that matches nothing says so, naming the view, without
    // claiming the account is empty: `empty` answers "does this account have
    // anything?" and `matchCount` answers the filter.
    void aFilterThatMatchesNothingSaysSoWithoutClaimingTheAccountIsEmpty()
    {
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        // Sanity first: prove the stripper can still see the code before any
        // negative assertion.
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
        // The message names which view matched nothing (by view, the search
        // box and the Unreads chip).
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

        // The model's half: `empty` and `matchCount` stay separate questions.
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

    // The account-wide DM list (`directsGroupId`) lives only in its own tab: a
    // DM is never a Space's child. A Space view may carry its own People group
    // (DMs with that Space's members) under a different group id.
    void aDirectMessageIsOnlyInTheDirectMessagesTab()
    {
        QString model = withoutComments(
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp")));
        QVERIFY(!model.isEmpty());
        model.replace(QRegularExpression(QStringLiteral("\\s+")),
                      QStringLiteral(" "));
        // Sanity first, or every negative assertion below is vacuous.
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

        // Home skips direct rooms.
        QVERIFY2(model.mid(home, people - home)
                     .contains(QStringLiteral("info.isDirect")),
                 "Home does not exclude DMs, so they are listed twice");
        // The Space builder drops a direct child that is a DM and builds no
        // DM group of its own.
        const QString spaceBody = model.mid(space);
        QVERIFY2(spaceBody.contains(QStringLiteral("childInfo->isDirect")),
                 "a Space's view does not exclude DMs");
        QVERIFY2(!spaceBody.contains(QStringLiteral("directsGroupId")),
                 "the account-wide Direct messages group is back inside a "
                 "Space's own view");
        // ...but it does build the Space-scoped People group, under a
        // different id (SpacePeopleScopeTest proves it appears). Anchored
        // between the call and the definition, since `spaceBody` runs to the
        // end of the file and would also match the definition.
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
        // ...and only the People builder makes one.
        QVERIFY(model.mid(people, space - people)
                    .contains(QStringLiteral("directsGroupId")));
    }

    // The rail's selection narrows Channels; it never decides whether the
    // layout works. A Space shows itself and its subspaces, Lobby shows
    // everything.
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
        // The host binds it to the rail's own selection.
        const QString host = withoutComments(read(QStringLiteral("RoomsPanel.qml")));
        QString flat = host;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("property: \"scopeSpaceId\"")),
                 "the scope is never bound to anything");
        QVERIFY2(flat.contains(QStringLiteral(
                     "readonly property bool channelsUsable: channelsChosen")),
                 "the scope decides whether the layout renders again");
        // The selection is kept verbatim and classified three ways: a Space
        // id, peopleViewId() (the DM tab), or Home.
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
        // The rail's People tab is Channels-only; Classic reaches DMs through
        // its People chip.
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("roomNavigationLayout === 1")),
                 "the Direct Messages tab is offered in Classic too");
        QVERIFY2(rail.contains(QStringLiteral("peopleEntryVisible")),
                 "the rail never tells its model whether to draw the tab");
        // "Other rooms" is Classic-only: in Channels, buildHome() already
        // lists exactly the rooms in no Space.
        QVERIFY2(rail.contains(QStringLiteral("orphansEntryVisible")),
                 "the rail never tells its model whether to draw \"Other "
                 "rooms\", so it is offered in Channels where it duplicates "
                 "Home");
        QVERIFY2(rail.contains(QStringLiteral("app.spaces.activeSpaceId === \"@orphans\"")),
                 "switching to Channels leaves the selection on a tile that "
                 "no longer exists");
        // ...which holds because buildHome() skips every room a Space lists;
        // if that changes, "Other rooms" becomes a real view again.
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
        // Rooms come from the client, not RoomListModel, which is scoped to
        // the active Space and filtered by the chips.
        QVERIFY2(!clean.contains(QStringLiteral("RoomListModel")),
                 "the Channels model reads the Space-scoped room list");
        QVERIFY(clean.contains(QStringLiteral("MatrixClient *client")));
    }

    // A channel row shows the room's avatar, not a hash glyph.
    void aChannelRowShowsTheRoomsAvatar()
    {
        const QString row = read(QStringLiteral("ChannelDelegate.qml"));
        QVERIFY(!row.isEmpty());
        QVERIFY2(row.contains(QStringLiteral("Avatar {")),
                 "a channel row draws no avatar, so every room in a Space "
                 "looks identical");
        QVERIFY2(row.contains(QStringLiteral("mxc: root.avatarUrl")),
                 "the avatar is never given the room's own picture");
        const QString presenter =
            read(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY2(presenter.contains(QStringLiteral("avatarUrl: rowLoader.model.avatarUrl")),
                 "the presenter never passes the avatar down");
        // The lock is still drawn, as a badge.
        QVERIFY(row.contains(QStringLiteral("\"lock\"")));
    }

    // Muting a Space mutes each room inside it; a Space has no timeline of
    // its own.
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
        // Unmute restores "follow the account default", not "all messages".
        QVERIFY2(body.contains(QStringLiteral("mute ? 2 : 3")),
                 "unmuting a Space asserts a mode rather than undoing one");
        const QString rail = read(QStringLiteral("SpacesRail.qml"));
        QVERIFY(rail.contains(QStringLiteral("objectName: \"railMuteSpace\"")));
    }

    // The theme editor previews whichever layout the user runs.
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
        // Still entirely fake: a theme preview must not touch a real model.
        const QString clean = withoutComments(preview);
        QVERIFY2(!clean.contains(QStringLiteral("app.spaceChannels")),
                 "the theme preview binds to the real Channels model");
        QVERIFY2(!clean.contains(QStringLiteral("app.roomList")),
                 "the theme preview binds to the real room list");
    }

    void subspacesAreNeverNestedInTheChannelsColumn()
    {
        // Subspaces are never nested in the Channels column: deep trees are
        // unreadable in a sidebar, and a subspace's rooms would be listed
        // twice.
        const QString model =
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp"));
        QVERIFY(!model.isEmpty());
        QString flat = withoutComments(model);
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        // Direct children only (never the transitive tree), resolved against
        // the room map this rebuild already built.
        QVERIFY2(flat.contains(QStringLiteral("directChildRoomIds"))
                     || flat.contains(QStringLiteral("directChildRoomsDetailed")),
                 "a Space folder does not list its DIRECT children");
        QVERIFY2(!flat.contains(QStringLiteral("childRoomsDetailed(spaceId)")),
                 "a Space folder lists the TRANSITIVE tree, so a subspace's "
                 "rooms appear under it and under its parent");
        QVERIFY2(!flat.contains(QStringLiteral("childSpacesDetailed")),
                 "child Spaces are being turned into nested categories again");
    }

    // The rail reveals only a Space's direct child rooms, not every
    // descendant's. A source scan, because the defect is a call site picking
    // the wrong one of two correct accessors; RailLayoutTest covers behaviour.
    void theRailRevealsOnlyDirectChildRooms()
    {
        const QString rail =
            withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("directChildRoomsDetailed(")),
                 "the rail does not ask for a Space's direct children at all");
        // Keyed on the receiver, not the argument name.
        // `spaces.directChildRoomsDetailed(` does not contain
        // `spaces.childRoomsDetailed(`.
        QVERIFY2(!rail.contains(QStringLiteral("spaces.childRoomsDetailed(")),
                 "the rail reveals a Space's TRANSITIVE rooms, so every "
                 "subspace's rooms appear under it AND under each of its "
                 "ancestors");
        // The transitive accessor still exists for its own callers; this bans a
        // call site, not the function. Anchored on the definition in
        // comment-stripped source (the name is a substring of the direct one).
        const QString manager = withoutComments(
            readSrc(QStringLiteral("spaces/SpaceManager.cpp")));
        QVERIFY2(manager.contains(
                     QStringLiteral("SpaceManager::childRoomsDetailed(")),
                 "the transitive accessor was deleted rather than left to "
                 "the surfaces that legitimately want the whole subtree");
    }

    void theRailDragLivesInAModelSoTheRowsCanMove()
    {
        // A JS array rebuilt on every change makes each change a model reset:
        // no move transition, and the delegate holding the gesture destroyed.
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("model: app.railEntries")),
                 "the rail still binds its list to a JavaScript array");
        QVERIFY2(!rail.contains(QStringLiteral("app.railLayout.arrange(")),
                 "the rail arranges its own rows in QML again");
        // The move and displaced transitions are what the model exists for.
        QVERIFY2(rail.contains(QStringLiteral("move: Transition")),
                 "the rail has no move transition, so a reorder cannot animate");
        QVERIFY2(rail.contains(QStringLiteral("displaced: Transition")),
                 "the rows the moved one pushed past do not animate");
        // A real beginMoveRows, not a reset dressed up as one.
        const QString model = readSrc(QStringLiteral("spaces/RailEntryModel.cpp"));
        QVERIFY(model.contains(QStringLiteral("beginMoveRows")));
        // Nothing is written while the pointer is down.
        QString flatModel = withoutComments(model);
        flatModel.replace(QRegularExpression(QStringLiteral("\\s+")),
                          QStringLiteral(" "));
        QVERIFY2(flatModel.contains(QStringLiteral("if (m_dragging) {")),
                 "a refresh during a drag is applied rather than deferred");
    }

    void theDraggedTileFollowsThePointerAtFullOpacity()
    {
        // The dragged tile is the feedback: it follows the pointer at full
        // appearance while neighbours animate, with no separate insertion line
        // or dimming.
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY2(rail.contains(QStringLiteral("readonly property real dragLift:")),
                 "the dragged tile does not follow the pointer");
        // Keyed on the mechanism, not the offset: the tile's inset is scaled,
        // so a literal would break on correct changes.
        QVERIFY2(rail.contains(QStringLiteral("+ spaceItem.dragLift")),
                 "the lift is computed but never applied to the tile");
        QVERIFY2(!rail.contains(QStringLiteral("railInsertionLine")),
                 "the insertion line came back");
        QVERIFY2(!rail.contains(QStringLiteral("railDragProxy")),
                 "the floating drag copy came back, so the tile is drawn twice");
        QVERIFY2(!rail.contains(QStringLiteral("opacity: spaceItem.dragged")),
                 "the dragged tile is dimmed again");
        // The group target is the only thing drawn over the movement; no
        // dwell is needed because nothing moves while the pointer is on a
        // tile.
        QVERIFY2(rail.contains(QStringLiteral("function readingAt(")),
                 "the pointer reading is not one total function any more");
        QVERIFY2(rail.contains(QStringLiteral("dropTarget")),
                 "a release would group with nothing saying so");
        // Auto-scroll, so a long rail can be dragged across.
        QVERIFY2(rail.contains(QStringLiteral("autoScroll")),
                 "a rail longer than the window cannot be dragged across");
    }

    // Releasing a drag announces the cleared flags even when refresh() finds
    // the rows unchanged and emits nothing; otherwise the tile stays drawn as
    // dragged.
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
        // Folders are device-local: nothing about them emits m.space.child,
        // m.space.parent or other room state.
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
        // A subspace row is not draggable, so a local rearrangement cannot look
        // like it moves the hierarchy.
        QVERIFY(model.contains(QStringLiteral("hierarchyChild")));
    }

    void classicIsTheDefaultAndTheClampTarget()
    {
        // 0 is Classic, and an out-of-range stored value lands there.
        const QString manager =
            readSrc(QStringLiteral("app/SettingsManager.cpp"));
        QVERIFY(!manager.isEmpty());
        const int at = manager.indexOf(
            QStringLiteral("SettingsManager::roomNavigationLayout"));
        QVERIFY2(at >= 0, "roomNavigationLayout has no accessor");
        // Wide enough to skip the comment above the return.
        const QString accessor = withoutComments(manager.mid(at, 900));
        QVERIFY2(accessor.contains(QStringLiteral("kRoomNavLayout, 0")),
                 "the default is not Classic");
        QVERIFY2(accessor.contains(QStringLiteral("? 0 :")),
                 "an out-of-range value does not clamp to Classic");
    }

    void theSettingIsAccountScoped()
    {
        // Stored per account via appearanceValue, and re-announced on account
        // switch so the column does not keep the previous account's layout.
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
        // On the account switch, beside the other appearance re-announcements.
        const int switchAt =
            manager.indexOf(QStringLiteral("Q_EMIT messageLayoutChanged();"));
        QVERIFY(switchAt >= 0);
        QVERIFY2(manager.mid(switchAt, 200)
                     .contains(QStringLiteral("roomNavigationLayoutChanged")),
                 "the layout is not re-announced on an account switch");
    }

    void everyChannelsTokenIsDerivedFromAnExistingOne()
    {
        // No new required palette key: a key missing from one of eleven
        // palettes draws a transparent row.
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
            // The rail's folder container.
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
        // The lock glyph is drawn only for encryption the client knows about;
        // unknown gets the plain hash.
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
        // Per-row delegates: an empty, never-laid-out Text keeps
        // ItemObservesViewport and makes Qt walk the whole tree on every
        // scroll frame.
        for (const QString &name : { QStringLiteral("ChannelDelegate.qml"),
                                     QStringLiteral("ChannelCategoryHeader.qml"),
                                     QStringLiteral("ChannelNavRow.qml"),
                                     QStringLiteral("FolderTile.qml") }) {
            const QString source = read(name);
            QVERIFY2(!source.isEmpty(), qPrintable(name));
            QString flat = source;
            flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                         QStringLiteral(" "));
            // Every Label is inside a Loader's sourceComponent, so Labels must
            // not outnumber the Loaders wrapping them.
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
        // One shared badge, so both layouts use the same ink for a count.
        QVERIFY(!read(QStringLiteral("UnreadBadge.qml")).isEmpty());
        QVERIFY(read(QStringLiteral("ChannelDelegate.qml"))
                    .contains(QStringLiteral("UnreadBadge {")));
        QVERIFY(read(QStringLiteral("ChannelCategoryHeader.qml"))
                    .contains(QStringLiteral("UnreadBadge {")));
        // The folder tile is its own component, so it cannot be reimplemented
        // elsewhere and drift.
        const QString rail = read(QStringLiteral("SpacesRail.qml"));
        QVERIFY(!read(QStringLiteral("FolderTile.qml")).isEmpty());
        QVERIFY(rail.contains(QStringLiteral("FolderTile {")));
        // Muting silences the count, never a mention.
        const QString badge = read(QStringLiteral("UnreadBadge.qml"));
        QVERIFY(badge.contains(QStringLiteral("root.mention ? AppTheme.dangerText")));
    }

    void aCollapsedCategoryStillReportsWhatItHides()
    {
        // Collapsing a group must not silently hide its highlights.
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
        // Muted rooms drop the count but still show that something happened.
        const QString row = read(QStringLiteral("ChannelDelegate.qml"));
        QString flat = row;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        // The pill covers plain unread as well as mentions; muting removes it.
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
        // Re-read the mode on an id change: rows are reused.
        QVERIFY(row.contains(QStringLiteral("onRoomIdChanged")));
        QVERIFY(row.contains(QStringLiteral("refreshNotificationMode")));
    }

    // The row chooser names every row kind the model can produce; an unnamed
    // kind fell through to the channel row and rendered a heading as a
    // clickable room with an empty id.
    void theRowChooserNamesEveryRowKindTheModelCanProduce()
    {
        const QString presenter =
            read(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY(!presenter.isEmpty());
        // Every kind string SpaceChannelModel::data can return.
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
        // The model's own closed set, so the two cannot drift.
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

    // Channels rows use the same shared actions menu component as Classic
    // rows.
    void bothLayoutsRowsUseTheOneSharedActionsMenu()
    {
        const QString classicRow = read(QStringLiteral("RoomDelegate.qml"));
        const QString channelRow = read(QStringLiteral("ChannelDelegate.qml"));
        QVERIFY(!classicRow.isEmpty());
        QVERIFY(!channelRow.isEmpty());
        QVERIFY(classicRow.contains(QStringLiteral("RoomActionsMenu {")));
        QVERIFY2(channelRow.contains(QStringLiteral("RoomActionsMenu {")),
                 "the Channels row has no actions menu");
        // Neither row re-declares the menu's rows.
        QVERIFY(!classicRow.contains(QStringLiteral("roomFavouriteItem")));
        QVERIFY(!channelRow.contains(QStringLiteral("roomFavouriteItem")));
        // The Channels row stays signal-only for every mutation; the presenter
        // performs the writes.
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

    // The translated filter chips compact instead of overflowing the clipping
    // 300 px column.
    void theRoomFilterChipsCompactInsteadOfOverflowingTheColumn()
    {
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        const QString control = read(QStringLiteral("SegmentedControl.qml"));
        QVERIFY(!host.isEmpty());
        QVERIFY(!control.isEmpty());
        QVERIFY2(host.contains(QStringLiteral("fitWidth: true")),
                 "the room-list filter chips do not ask to be fitted");
        // A RowLayout: a plain Row derives implicitWidth from its children's
        // assigned widths, so shrinking them loops polish().
        QVERIFY(control.contains(QStringLiteral("RowLayout {")));
        QVERIFY(control.contains(QStringLiteral("Layout.fillWidth: root.overflowing")));
        QVERIFY(control.contains(QStringLiteral("Layout.maximumWidth: implicitWidth")));
        // Fill only while it does not fit; otherwise four chips spread across
        // the column.
        QVERIFY(control.contains(QStringLiteral("fitWidth && width > 0 && implicitWidth > width")));
        QVERIFY(control.contains(QStringLiteral("elide: Text.ElideRight")));
    }

    // One direction only: chips -> setting -> model. Chips read the setting
    // they write, never the model.
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

        // Channels maps the stored value rather than rewriting it, so switching
        // layouts keeps the chip chosen in Classic. There is one mapping, read
        // by both the chip row and the channel model; neither may carry its
        // own copy.
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

    // The room-list order mirrors the SDK's room list one for one, because
    // every Set/Remove/Truncate diff addresses it by index. Appending Space ids
    // to it made diffs land on the wrong entry, get rejected, and trigger a
    // snapshot loop. A source scan because the handler needs the Rust FFI.
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

        // Snapshots keeping the Spaces is a pure function in
        // RustRoomRegistry.cpp, tested by RustRoomRegistryTest. Pin only that
        // the handler delegates to it rather than rebuilding inline.
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
        // The model is already grouped by hierarchy; ListView section headers
        // would draw the grouping twice.
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        // read() returns "" for a missing file and the only assertion is
        // negative, so guard that the file was read.
        QVERIFY2(!presenter.isEmpty(),
                 "RoomChannelsPresenter.qml could not be read — this contract "
                 "would otherwise pass vacuously");
        QVERIFY2(!presenter.contains(QStringLiteral("section.property")),
                 "the Channels list adds a second grouping mechanism");
    }

    // Local image hiding keeps the row's geometry and never touches Matrix.
    void hidingAnImageIsLocalAndKeepsTheRowsGeometry()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const QString clean = withoutComments(delegate);
        // Keyed through the store: a timeline row is destroyed when it leaves
        // the cache buffer.
        QVERIFY2(clean.contains(QStringLiteral("app.mediaVisibility")),
                 "the hidden flag is not keyed through the store, so it is "
                 "lost the moment the row is recycled");
        QVERIFY2(clean.contains(QStringLiteral("MediaHiddenPlaceholder {")),
                 "there is no geometry-preserving placeholder");
        // The placeholder fills the media box and contributes no size, so the
        // timeline does not move.
        const QString placeholder = withoutComments(
            read(QStringLiteral("MediaHiddenPlaceholder.qml")));
        QVERIFY(!placeholder.isEmpty());
        QVERIFY2(!placeholder.contains(QStringLiteral("implicitWidth:"))
                     && !placeholder.contains(QStringLiteral("implicitHeight:")),
                 "the placeholder contributes its own implicit size, so the "
                 "row resizes when an image is hidden");
        // Purely local: no redaction, no edit, nothing sent.
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
        // Persisted locally and account-scoped, but never reaching the Matrix
        // client: there is no standard for it, so writing it would put a
        // Lightning-only key into the account.
        QVERIFY2(!store.contains(QStringLiteral("MatrixClient")),
                 "the hidden-image store reaches the Matrix client, so it is "
                 "no longer purely local rendering state");
        QVERIFY2(!store.contains(QStringLiteral("AccountData"))
                     && !store.contains(QStringLiteral("account_data")),
                 "hidden images are being written to Matrix account data");
        // The allowed persistence stays account-scoped: the setter has no
        // global fallback.
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
        // Hide is offered while visible and Show on the placeholder; a hidden
        // row gets no second Hide.
        QVERIFY(clean.contains(QStringLiteral(
            "visible: root.mediaHideable && !root.mediaHidden")));
        QVERIFY(placeholder.contains(QStringLiteral("Show image")));
        // The placeholder's body is gated on `hidden`, so a row that is never
        // hidden creates no Text.
        QVERIFY2(placeholder.contains(QStringLiteral("active: root.hidden")),
                 "the placeholder's contents are built for every media row");
    }

    void settingsOffersBothLayoutsAsPreviews()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral("navLayoutClassicCard")));
        QVERIFY(settings.contains(QStringLiteral("navLayoutChannelsCard")));
        // A diagram, not a live presenter (which would need a room list and
        // change while viewed).
        const QString card = withoutComments(
            read(QStringLiteral("NavigationLayoutCard.qml")));
        QVERIFY(!card.isEmpty());
        QVERIFY2(!card.contains(QStringLiteral("app.roomList")),
                 "the preview card binds to the real room list");
        QVERIFY2(!card.contains(QStringLiteral("RoomListClassicPresenter")),
                 "the preview card instantiates a real presenter");
    }

    // The rail never reorders into the tile a drag is aiming at: the tile is
    // the group target and nothing moves while the pointer is on it; the gap
    // between tiles is the reorder target. RailDragQmlTest drives this with a
    // real pointer; this pins the mechanism in source.
    void theRailNeverReordersIntoTheTileTheDragIsAimingAt()
    {
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("function rowIsDraggedBlock(")),
                 "the dragged block's own slot is treated as a droppable row");

        // Both retired band rules stay retired.
        QVERIFY2(!rail.contains(QStringLiteral("function pointerPushedThrough(")),
                 "the arrival-side midpoint rule is back");
        QVERIFY2(!rail.contains(QStringLiteral("function pointerOverTileCentre(")),
                 "the centre-band rule is back; reaching that band means "
                 "crossing the near edge first, which reorders");
        QVERIFY2(!rail.contains(QStringLiteral("dwellTimer")),
                 "the dwell is back — it existed to compensate for a reading "
                 "that moved things while the user was still aiming, and that "
                 "reading is gone");

        // One dispatch; the branch that reads a tile may only arm or clear
        // grouping, never move.
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

        // Auto-scroll goes through the same dispatch, not its own reorder.
        const int scroll = rail.indexOf(QStringLiteral("id: autoScroll"));
        QVERIFY(scroll > 0);
        const QString scrollBody = rail.mid(scroll, 1400);
        QVERIFY2(scrollBody.contains(QStringLiteral("applyPointerReading(")),
                 "the auto-scroll has its own drag dispatch again");

        // The model offers three exclusive verbs, with no flag that turns an
        // aim into a move.
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

    // The rail's Space menu carries real Space actions and a header naming
    // the Space it belongs to.
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
        // The shared link is the public matrix.to permalink, never an
        // authenticated URL.
        QVERIFY2(rail.contains(QStringLiteral("roomPermalink(")),
                 "the shared link is not the room permalink");

        // Home offers an account-wide mark-all-read through the room list,
        // which also clears the Activity bell.
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

    // A hidden menu row takes no height: MenuSeparator keeps its height when
    // invisible, leaving a gap in the ListView.
    void aHiddenMenuRowTakesNoHeight()
    {
        const QString sep = read(QStringLiteral("AppMenuSeparator.qml"));
        QVERIFY2(sep.contains(QStringLiteral("implicitHeight: visible ?")),
                 "a hidden separator still reserves its own height");
        const QString item = read(QStringLiteral("AppMenuItem.qml"));
        QVERIFY2(item.contains(QStringLiteral("implicitHeight: visible ?")),
                 "a hidden menu item still reserves its own height");
    }

    // Space settings writes room state and nothing else: no Space-only local
    // storage presented as the Space's.
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
        // Nothing here may reach settings storage.
        QVERIFY2(!dialog.contains(QStringLiteral("app.settings")),
                 "space settings writes device-local state and presents it as "
                 "part of the Space");
        QVERIFY2(!dialog.contains(QStringLiteral("app.railLayout")),
                 "space settings writes the local rail arrangement");
        // A restricted join rule is shown and left alone; this surface cannot
        // build its allow list.
        QVERIFY2(dialog.contains(QStringLiteral("knock_restricted")),
                 "a space-restricted join rule is no longer detected, so this "
                 "surface would offer to overwrite it with an empty allow list");
        // Themed throughout: no literal colours.
        QVERIFY2(!dialog.contains(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}"))),
                 "space settings hardcodes a colour");
    }
};

QTEST_MAIN(NavigationLayoutContractTest)
#include "NavigationLayoutContractTest.moc"
