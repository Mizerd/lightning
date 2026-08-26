// 2026-08-23 navigation layouts: the host/presenter contract.
//
// A source-scan suite, deliberately. What it defends is not behaviour inside
// one component but the SEPARATION between them, and that separation is a
// property of which file contains what — which no runtime assertion can see.
//
// The invariants, each one a defect that would ship silently:
//
//  * The host owns the chrome. If a presenter grows its own workspace header
//    or search field, the two layouts fork and one of them stops getting
//    fixes. RoomsPanel keeps the header; neither presenter may declare one.
//  * A presenter never reaches up into the host by id. The extracted Classic
//    list originally called `leaveRoomConfirm.openFor(...)` and
//    `newConversationDialog.openDialog()` — resolved by scope, from a
//    delegate, through a parent chain. That is exactly how the reader
//    popover's click ended up silently dead (2026-08-19), and it breaks
//    without a warning the moment the component is instantiated anywhere
//    else. Signals only.
//  * Exactly one presenter is INSTANTIATED. Two visibility-gated room lists
//    is two sets of avatar fetches for one visible column, so the host uses
//    Loaders whose `active` is the layout choice.
//  * Channels is GLOBAL and never falls back. It used to require an active
//    Space and become Classic without one, so a user who chose it got the
//    other layout at Home — the layout silently depended on where you were.
//  * Classic is still the DEFAULT and the clamp target. It is the layout that
//    works in an account with no Spaces at all.
//  * The new theme tokens are DERIVED. A new required key in eleven palettes
//    is how a theme ends up with one undefined colour and a transparent row.
//  * The rail's drag lives in a MODEL. A JS array rebuilt on every change is a
//    model reset: no move transition, and the delegate holding the gesture
//    destroyed under the pointer.
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

/// The file with its comments removed.
///
/// Every BAN assertion below must scan this, never the raw source. A comment
/// that NAMES the thing it explains why we do not do is the documented way a
/// ban regex fires on prose — it has cost this repo real time twice, and it
/// punishes exactly the comments worth writing.
QString withoutComments(const QString &source)
{
    QString out = source;
    // Block comments first, so a `//` inside one is not treated as a line.
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    // Trailing comments too, but not a `//` inside a string literal — which
    // in these files only ever appears in a URL, and there are none.
    //
    // The `\\n` in that class is load-bearing and was missing until
    // 2026-08-25. A negated character class MATCHES NEWLINES, so `[^"']*`
    // ran from a trailing comment through every following line until it
    // found one ending in a quote — swallowing the code in between. It ate
    // two lines out of `setSpaceMuted` and the ban assertion that read them
    // simply reported the code absent. Every scan in this file that looks
    // AFTER a trailing comment was silently weakened by it.
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
        // The workspace header, the search field and the create/discover
        // dialogs are the host's. A presenter that grows its own is how the
        // two layouts stop getting the same fixes.
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
        // THE regression this suite is really for. These four ids live in
        // RoomsPanel and were resolved by scope from inside a delegate — the
        // 2026-08-19 dead-click shape, and silent the moment the component is
        // instantiated anywhere else.
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
        // Loader-gated, not visibility-gated: the layout that is not chosen
        // must build no ListView, no delegates and no empty state. Two live
        // room lists is two sets of avatar fetches for one visible column.
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
        // THE reason this layout was rebuilt. It used to show one Space's
        // hierarchy, so at Home there was nothing to show and the host
        // rendered Classic instead — the user chose a navigation layout and
        // got the other one, with nothing saying why.
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

    // Every command row the model can produce is DISPATCHED by the presenter
    // and WIRED by the host to a dialog that exists. A row that reaches none
    // of the three is a control that looks clickable and does nothing —
    // exactly the failure the five-way row chooser already carries a note
    // about, one layer up.
    void everyChannelActionIsDispatched()
    {
        const QString header = readSrc(QStringLiteral("models/SpaceChannelModel.h"));
        QVERIFY(!header.isEmpty());
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        const QString host = withoutComments(read(QStringLiteral("RoomsPanel.qml")));
        QVERIFY(!presenter.isEmpty());
        QVERIFY(!host.isEmpty());
        // SANITY FIRST: the stripper must still be able to see the chooser.
        QVERIFY2(presenter.contains(QStringLiteral("actionComponent")),
                 "the comment stripper ate the presenter's row chooser");

        // The ids, read out of the header's own accessors rather than
        // duplicated here — one definition, or this test pins a set the model
        // has moved on from.
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
        // The MODEL names each row's glyph, so there is one place to pin
        // against the bundled icon SUBSET rather than a chooser in QML.
        QVERIFY(header.contains(QStringLiteral("IconNameRole")));
        QVERIFY2(presenter.contains(QStringLiteral("rowLoader.model.iconName")),
                 "the presenter hardcodes glyph names instead of reading the "
                 "model's");
    }

    void theChannelsColumnCarriesLobbyRoomsAndMessageSearch()
    {
        // Sable's model, and the three entries that make it navigable on its
        // own now that Classic is not a fallback: somewhere to go Home, the
        // rooms that no Space folder lists, and a real search.
        const QString presenter =
            read(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY(!presenter.isEmpty());
        QVERIFY2(presenter.contains(QStringLiteral("signal lobbyActivated()")),
                 "the Lobby row does nothing");
        QVERIFY2(presenter.contains(
                     QStringLiteral("signal messageSearchRequested()")),
                 "the Message Search row does nothing");
        // Message Search opens the EXISTING global search, by signal, through
        // the host — never a filter over this list, and never a second dialog.
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

    // "In channels mode people tab does nothing." The chip was not inert: it
    // reached SpaceChannelModel and the model rebuilt correctly. Two things
    // made a working control read as a dead one, and this pins both.
    //
    //  * A scope deleted every DM before the filter ran. Matrix gives no way
    //    for a DM to be a Space's child, so the account-wide group was the
    //    only place one could live and dropping that group while scoped
    //    dropped DMs everywhere. People then had nothing to find, whatever it
    //    was doing.
    //  * The column had no wording for "this matched nothing", so the result
    //    was Lobby and Message Search over blank space — indistinguishable
    //    from a chip that had done nothing at all.
    //
    // The second half is the one that must not be "simplified" into `empty`:
    // that property answers "does this account have anything?" and answering
    // it with a fact about the filter sends the user looking for a problem
    // that is not there.
    void aFilterThatMatchesNothingSaysSoWithoutClaimingTheAccountIsEmpty()
    {
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        // SANITY FIRST, and deliberately. withoutComments() is a parser, and
        // its trailing-comment class ran across newlines until 2026-08-25 —
        // it swallowed the code it was meant to scan and reported it absent,
        // which is a PASS for a ban and a false failure for everything else.
        // A negative assertion over source this function has not proven it can
        // still see is worth nothing, so prove it first.
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
        // It has to NAME which view matched nothing. A message that does not
        // is the same silence with words on it. The two chips Channels still
        // offers are All and Unreads — People and Rooms are the rail's tabs
        // now — so the naming is by VIEW plus the search box and Unreads.
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

        // The model's half. `empty` keeps answering one question and
        // `matchCount` answers the other.
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

    // A DIRECT MESSAGE IS IN EXACTLY ONE VIEW: its own tab. Two earlier
    // designs put it in "Rooms" with every other unparented room, and then in
    // a "Direct messages" group every view had to carry so a scope could not
    // delete the only place it lived. Both are bans now, in the model AND in
    // the builder that would have to reintroduce them.
    void aDirectMessageIsOnlyInTheDirectMessagesTab()
    {
        QString model = withoutComments(
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp")));
        QVERIFY(!model.isEmpty());
        model.replace(QRegularExpression(QStringLiteral("\\s+")),
                      QStringLiteral(" "));
        // SANITY FIRST: prove the stripper can still see the code, or every
        // negative assertion below is vacuous.
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

        // Home SKIPS direct rooms outright.
        QVERIFY2(model.mid(home, people - home)
                     .contains(QStringLiteral("info.isDirect")),
                 "Home does not exclude DMs, so they are listed twice");
        // The Space builder drops a direct child that is somehow a DM, and
        // never builds a DM group of its own.
        const QString spaceBody = model.mid(space);
        QVERIFY2(spaceBody.contains(QStringLiteral("childInfo->isDirect")),
                 "a Space's view does not exclude DMs");
        QVERIFY2(!spaceBody.contains(QStringLiteral("directsGroupId")),
                 "the account-wide Direct messages group is back inside a "
                 "Space's own view");
        // ...and only the People builder makes one.
        QVERIFY(model.mid(people, space - people)
                    .contains(QStringLiteral("directsGroupId")));
    }

    // The rail's selection NARROWS this layout; it does not decide whether the
    // layout works. Those are different things and the difference is the whole
    // point: the old design produced nothing without a Space and the host
    // rendered Classic instead, so picking a Space could turn the layout off.
    // Now picking one shows that Space and its subspaces, Lobby shows
    // everything, and both states are this layout.
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
        // The host binds it, and to the rail's OWN selection.
        const QString host = withoutComments(read(QStringLiteral("RoomsPanel.qml")));
        QString flat = host;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral("property: \"scopeSpaceId\"")),
                 "the scope is never bound to anything");
        QVERIFY2(flat.contains(QStringLiteral(
                     "readonly property bool channelsUsable: channelsChosen")),
                 "the scope decides whether the layout renders again");
        // The selection is kept VERBATIM and classified. A room id is a
        // Space, peopleViewId() is the DM tab, everything else is Home — three
        // outcomes, so the setter cannot collapse to the two it used to have.
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
        // The rail's tab is CHANNELS ONLY: Classic reaches DMs through its
        // People chip and was asked to stay as it is.
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("roomNavigationLayout === 1")),
                 "the Direct Messages tab is offered in Classic too");
        QVERIFY2(rail.contains(QStringLiteral("peopleEntryVisible")),
                 "the rail never tells its model whether to draw the tab");
        // And the rooms come from the CLIENT, not from RoomListModel — that
        // model is scoped to the active Space and filtered by the chips, which
        // would make the global groups vanish the moment a Space was picked.
        QVERIFY2(!clean.contains(QStringLiteral("RoomListModel")),
                 "the Channels model reads the Space-scoped room list");
        QVERIFY(clean.contains(QStringLiteral("MatrixClient *client")));
    }

    // Sable's own column shows a picture per room. The first revision drew a
    // hash glyph instead, which made every room in a Space look identical.
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
        // The lock is still a CLAIM and still drawn — as a badge, not instead
        // of the picture.
        QVERIFY(row.contains(QStringLiteral("\"lock\"")));
    }

    // A Space is a room with no timeline, so muting it silences nothing:
    // "mute this space" has to mean each room inside it, or it is a control
    // that reports success and changes nothing.
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
        // Unmute restores "follow the account default", not "all messages":
        // asserting the loud mode for rooms that never asked for it is a
        // different choice from undoing a mute.
        QVERIFY2(body.contains(QStringLiteral("mute ? 2 : 3")),
                 "unmuting a Space asserts a mode rather than undoing one");
        const QString rail = read(QStringLiteral("SpacesRail.qml"));
        QVERIFY(rail.contains(QStringLiteral("objectName: \"railMuteSpace\"")));
    }

    // The editor's preview must show the column the user actually runs. A
    // preview of the other layout shows them where a colour lands somewhere
    // they never look.
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
        // Still entirely fake: a theme preview must not reach a real model.
        const QString clean = withoutComments(preview);
        QVERIFY2(!clean.contains(QStringLiteral("app.spaceChannels")),
                 "the theme preview binds to the real Channels model");
        QVERIFY2(!clean.contains(QStringLiteral("app.roomList")),
                 "the theme preview binds to the real room list");
    }

    void subspacesAreNeverNestedInTheChannelsColumn()
    {
        // Deliberate: a Space tree in a sidebar is unreadable by about three
        // levels, and the old design listed a subspace's rooms twice — under
        // the subspace's category AND transitively under the parent.
        const QString model =
            readSrc(QStringLiteral("models/SpaceChannelModel.cpp"));
        QVERIFY(!model.isEmpty());
        QString flat = withoutComments(model);
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        // `directChildRoomIds` since 2026-08-26: same DIRECT-children
        // contract and the same skip rules, resolved against the room map
        // this rebuild already built. The old accessor materialised the
        // WHOLE room list and a fresh hash of its own on every call, so
        // walking every Space cost (1 + numSpaces) materialisations per
        // rebuild — a real slice of the account switch that "takes longer
        // now". What matters here is unchanged: DIRECT children, never the
        // transitive tree.
        QVERIFY2(flat.contains(QStringLiteral("directChildRoomIds"))
                     || flat.contains(QStringLiteral("directChildRoomsDetailed")),
                 "a Space folder does not list its DIRECT children");
        QVERIFY2(!flat.contains(QStringLiteral("childRoomsDetailed(spaceId)")),
                 "a Space folder lists the TRANSITIVE tree, so a subspace's "
                 "rooms appear under it and under its parent");
        QVERIFY2(!flat.contains(QStringLiteral("childSpacesDetailed")),
                 "child Spaces are being turned into nested categories again");
    }

    void theRailDragLivesInAModelSoTheRowsCanMove()
    {
        // A JS array rebuilt on every change makes every change a model RESET:
        // no move transition, every delegate torn down, and the delegate
        // holding the gesture destroyed under the pointer. Both halves of
        // "kinda hard to tell exactly where you are moving them".
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
        // "spaces should always be their normal image and move freely without
        // a line appearing between them". The tile IS the feedback: it follows
        // the pointer while its neighbours animate around it, and where it
        // currently sits is where it will land — so a separate insertion line
        // claiming the same thing was noise on 68 px of chrome, and dimming
        // the tile made the one thing being looked at the hardest to see.
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY2(rail.contains(QStringLiteral("readonly property real dragLift:")),
                 "the dragged tile does not follow the pointer");
        QVERIFY2(rail.contains(QStringLiteral("y: 4 + spaceItem.dragLift")),
                 "the lift is computed but never applied to the tile");
        QVERIFY2(!rail.contains(QStringLiteral("railInsertionLine")),
                 "the insertion line came back");
        QVERIFY2(!rail.contains(QStringLiteral("railDragProxy")),
                 "the floating drag copy came back, so the tile is drawn twice");
        QVERIFY2(!rail.contains(QStringLiteral("opacity: spaceItem.dragged")),
                 "the dragged tile is dimmed again");
        // The GROUP target is the one thing still drawn on top of the
        // movement. It no longer needs a dwell: nothing moves while the
        // pointer is on a tile, so dragging THROUGH one changes the order not
        // at all, which is what the dwell was standing in for.
        QVERIFY2(rail.contains(QStringLiteral("function readingAt(")),
                 "the pointer reading is not one total function any more");
        QVERIFY2(rail.contains(QStringLiteral("dropTarget")),
                 "a release would group with nothing saying so");
        // And auto-scroll, so a long rail does not need drop-scroll-redrag.
        QVERIFY2(rail.contains(QStringLiteral("autoScroll")),
                 "a rail longer than the window cannot be dragged across");
    }

    // A released tile must stop rendering as dragged IMMEDIATELY. `refresh()`
    // is allowed to find the rows identical and emit nothing at all, which is
    // right for the row data and catastrophic for the drag flags: the tile
    // stayed dimmed with its line under it until an unrelated room update
    // happened to refresh the model. Reported in exactly those terms.
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
        // Folders are DEVICE-LOCAL organisation. Nothing about them may emit
        // m.space.child, m.space.parent or any other room state.
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
        // And a subspace row is not draggable at all, so a local rearrangement
        // cannot even look like it moves the hierarchy.
        QVERIFY(model.contains(QStringLiteral("hierarchyChild")));
    }

    void classicIsTheDefaultAndTheClampTarget()
    {
        // 0 is Classic, and an out-of-range stored value must land there —
        // Classic is the layout that works in an account with no Spaces.
        const QString manager =
            readSrc(QStringLiteral("app/SettingsManager.cpp"));
        QVERIFY(!manager.isEmpty());
        const int at = manager.indexOf(
            QStringLiteral("SettingsManager::roomNavigationLayout"));
        QVERIFY2(at >= 0, "roomNavigationLayout has no accessor");
        // Wide enough to clear the explanatory comment above the return.
        const QString accessor = withoutComments(manager.mid(at, 900));
        QVERIFY2(accessor.contains(QStringLiteral("kRoomNavLayout, 0")),
                 "the default is not Classic");
        QVERIFY2(accessor.contains(QStringLiteral("? 0 :")),
                 "an out-of-range value does not clamp to Classic");
    }

    void theSettingIsAccountScoped()
    {
        // appearanceValue, like every other Appearance choice: a Space-heavy
        // work account and a DM-only personal account should not be forced
        // into one shape. And the switch must RE-ANNOUNCE it, or the column
        // keeps the previous account's layout.
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
        // Specifically on the account switch, next to the other per-account
        // appearance re-announcements.
        const int switchAt =
            manager.indexOf(QStringLiteral("Q_EMIT messageLayoutChanged();"));
        QVERIFY(switchAt >= 0);
        QVERIFY2(manager.mid(switchAt, 200)
                     .contains(QStringLiteral("roomNavigationLayoutChanged")),
                 "the layout is not re-announced on an account switch");
    }

    void everyChannelsTokenIsDerivedFromAnExistingOne()
    {
        // No new required palette key. Eleven palettes with one missing key
        // is how a theme ends up drawing a transparent row, and the
        // no-QML-warnings gate is the only thing that catches it.
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
        // The lock glyph is a claim. It may only be drawn for encryption the
        // client KNOWS about; "not established yet" gets the plain hash.
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
        // These are PER-ROW delegates. A never-laid-out empty Text keeps
        // ItemObservesViewport forever and makes Qt walk the whole
        // instantiated tree on every scroll frame — the single most
        // expensive QML mistake recorded in this repo.
        for (const QString &name : { QStringLiteral("ChannelDelegate.qml"),
                                     QStringLiteral("ChannelCategoryHeader.qml"),
                                     QStringLiteral("ChannelNavRow.qml"),
                                     QStringLiteral("FolderTile.qml") }) {
            const QString source = read(name);
            QVERIFY2(!source.isEmpty(), qPrintable(name));
            QString flat = source;
            flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                         QStringLiteral(" "));
            // Every Label in these two files is inside a Loader's
            // sourceComponent, so the count of Labels must not exceed the
            // count of Loaders that wrap one.
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
        // Two hand-rolled pills is how one layout says "3" in danger ink and
        // the other in accent ink for the same room.
        QVERIFY(!read(QStringLiteral("UnreadBadge.qml")).isEmpty());
        QVERIFY(read(QStringLiteral("ChannelDelegate.qml"))
                    .contains(QStringLiteral("UnreadBadge {")));
        QVERIFY(read(QStringLiteral("ChannelCategoryHeader.qml"))
                    .contains(QStringLiteral("UnreadBadge {")));
        // The folder tile is its own component, so the composite cannot be
        // reimplemented somewhere else and drift.
        const QString rail = read(QStringLiteral("SpacesRail.qml"));
        QVERIFY(!read(QStringLiteral("FolderTile.qml")).isEmpty());
        QVERIFY(rail.contains(QStringLiteral("FolderTile {")));
        // Muting silences the count, never the mention: somebody naming you
        // is not noise.
        const QString badge = read(QStringLiteral("UnreadBadge.qml"));
        QVERIFY(badge.contains(QStringLiteral("root.mention ? AppTheme.dangerText")));
    }

    void aCollapsedCategoryStillReportsWhatItHides()
    {
        // Collapsing to save space must not silently mute a group.
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
        // The user asked not to be counted at, not to be lied to about
        // whether anything happened.
        const QString row = read(QStringLiteral("ChannelDelegate.qml"));
        QString flat = row;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY(flat.contains(QStringLiteral(
            "readonly property bool showsPill: root.highlightCount > 0 && !root.muted")));
        // And it re-reads the mode on an id change, because reuseItems is on
        // and a recycled row would otherwise inherit the previous room's.
        QVERIFY(row.contains(QStringLiteral("onRoomIdChanged")));
        QVERIFY(row.contains(QStringLiteral("refreshNotificationMode")));
    }

    // The row chooser must name every kind the MODEL can produce. It named
    // two of three, so a "section" row (a group label like "Direct messages"
    // or "Favourites") fell through to the channel-row component and rendered
    // as a room row with an empty room id — clickable-looking, opening
    // nothing, and carrying a room's context menu over a heading. Reported as
    // "when I click People it says direct messages at the top" and "I can't
    // left click room names".
    void theRowChooserNamesEveryRowKindTheModelCanProduce()
    {
        const QString presenter =
            read(QStringLiteral("RoomChannelsPresenter.qml"));
        QVERIFY(!presenter.isEmpty());
        // Every kind string SpaceChannelModel::data can return. It once named
        // two of three, so a group label fell through to the channel-row
        // component and rendered as a room row with an empty room id —
        // clickable-looking, opening nothing, and carrying a room's context
        // menu over a heading.
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

    // The Channels row offers the SAME actions the Classic row does, through
    // the SAME component — the alternative is two menus that drift, and what
    // it actually was is no menu at all.
    void bothLayoutsRowsUseTheOneSharedActionsMenu()
    {
        const QString classicRow = read(QStringLiteral("RoomDelegate.qml"));
        const QString channelRow = read(QStringLiteral("ChannelDelegate.qml"));
        QVERIFY(!classicRow.isEmpty());
        QVERIFY(!channelRow.isEmpty());
        QVERIFY(classicRow.contains(QStringLiteral("RoomActionsMenu {")));
        QVERIFY2(channelRow.contains(QStringLiteral("RoomActionsMenu {")),
                 "the Channels row has no actions menu");
        // Neither row re-declares the menu's rows: one definition only.
        QVERIFY(!classicRow.contains(QStringLiteral("roomFavouriteItem")));
        QVERIFY(!channelRow.contains(QStringLiteral("roomFavouriteItem")));
        // And the Channels row stays signal-only for every mutation, like the
        // Classic one — the presenter performs the writes.
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

    // The filter chips sit in a 300px column that CLIPS, and the four labels
    // are translated. They compact instead of running off the pane edge —
    // "Unreads" was clipped by the column boundary in a real screenshot.
    void theRoomFilterChipsCompactInsteadOfOverflowingTheColumn()
    {
        const QString host = read(QStringLiteral("RoomsPanel.qml"));
        const QString control = read(QStringLiteral("SegmentedControl.qml"));
        QVERIFY(!host.isEmpty());
        QVERIFY(!control.isEmpty());
        QVERIFY2(host.contains(QStringLiteral("fitWidth: true")),
                 "the room-list filter chips do not ask to be fitted");
        // A RowLayout, because a plain Row derives its implicitWidth from its
        // children's ASSIGNED widths — shrinking them shrinks the total the
        // scale was computed from, which measured as a polish() loop.
        QVERIFY(control.contains(QStringLiteral("RowLayout {")));
        QVERIFY(control.contains(QStringLiteral("Layout.fillWidth: root.overflowing")));
        QVERIFY(control.contains(QStringLiteral("Layout.maximumWidth: implicitWidth")));
        // Only while it genuinely does not fit: filling when it DOES fit
        // spreads four chips across the whole column.
        QVERIFY(control.contains(QStringLiteral("fitWidth && width > 0 && implicitWidth > width")));
        QVERIFY(control.contains(QStringLiteral("elide: Text.ElideRight")));
    }

    // One direction only: chips -> setting -> model. `current` bound to the
    // MODEL while every click wrote the SETTING meant any moment the two
    // disagreed left the chips reporting a filter the user had not chosen.
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

        // CHANNELS DROPS People and Rooms — the rail's two tabs ARE that
        // split — but it must MAP the stored value rather than rewrite it, or
        // switching layouts silently destroys the chip the user chose in
        // Classic. The mapping appears twice on purpose (the chip row and the
        // channel model's binding) and both must be the same rule.
        QString flat = withoutComments(host);
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        QVERIFY2(flat.contains(QStringLiteral(
                     "current: channelsLayout ? (app.settings.roomFilterMode === 3 ? 3 : 0) "
                     ": app.settings.roomFilterMode")),
                 "the chip row does not map the stored filter for Channels, so "
                 "a stored People/Rooms value selects no chip at all");
        QVERIFY2(flat.contains(QStringLiteral(
                     "property: \"filterMode\" value: app.settings.roomFilterMode === 3 ? 3 : 0")),
                 "the channel model is handed a filter whose chip is not on "
                 "screen, so the column filters for a reason nothing states");
        QVERIFY2(!flat.contains(QStringLiteral("app.settings.roomFilterMode = 0")),
                 "the stored filter is rewritten when Channels drops its chip, "
                 "so returning to Classic loses the user's choice");
    }

    // The room-list ORDER mirrors the SDK's own room list one for one,
    // because every Set/Remove/Truncate diff addresses it BY INDEX.
    //
    // This is a performance invariant with a nasty failure mode, and it was
    // broken: the spaces handler appended Space ids to the same list. As soon
    // as the room list grew past the point they were appended at, the SDK's
    // index i named a different room here than there — Set landed on the
    // wrong entry, saw an id that already existed, and was rejected as
    // malformed. Rejection asks Rust for a fresh snapshot, which re-appends
    // the spaces, which collides again: a loop that re-emitted the whole room
    // list, with its avatar fetches, many times a minute. It showed up as
    // "room_list malformed diff rejected" filling the log while account
    // switching, message sending and the room list itself all went slow.
    //
    // A source scan because the diff handler needs the Rust FFI to
    // instantiate; the assertion is exactly the line that caused it.
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

        // The other half: a snapshot of the SDK's list must not delete the
        // spaces, which are not in it.
        const int snapshot =
            source.indexOf(QStringLiteral("void RustSdkMatrixClient::handleRoomsEvent"));
        QVERIFY(snapshot > 0);
        const int afterSnapshot =
            source.indexOf(QStringLiteral("\nRoomInfo RustSdkMatrixClient::"),
                           snapshot);
        const QString snapBody = source.mid(
            snapshot,
            (afterSnapshot > snapshot ? afterSnapshot : source.size()) - snapshot);
        QVERIFY2(snapBody.contains(QStringLiteral("isSpace")),
                 "a room-list snapshot replaces the whole room map without "
                 "carrying the Spaces over, so the hierarchy disappears until "
                 "the next spaces event");
    }

    void theChannelsPresenterDrawsNoSecondGrouping()
    {
        // The MODEL is already ordered and grouped by the hierarchy. A
        // ListView section header on top of the category rows would draw the
        // same grouping twice.
        const QString presenter = withoutComments(
            read(QStringLiteral("RoomChannelsPresenter.qml")));
        QVERIFY2(!presenter.contains(QStringLiteral("section.property")),
                 "the Channels list adds a second grouping mechanism");
    }

    // Element-style local image hiding: the geometry contract is what makes it
    // usable, and the honesty contract is what makes it safe.
    void hidingAnImageIsLocalAndKeepsTheRowsGeometry()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const QString clean = withoutComments(delegate);
        // The state is not a delegate-local boolean: a timeline row is
        // destroyed the moment it leaves the cache buffer.
        QVERIFY2(clean.contains(QStringLiteral("app.mediaVisibility")),
                 "the hidden flag is not keyed through the store, so it is "
                 "lost the moment the row is recycled");
        QVERIFY2(clean.contains(QStringLiteral("MediaHiddenPlaceholder {")),
                 "there is no geometry-preserving placeholder");
        // The placeholder fills the media box and contributes no size of its
        // own — that is what keeps the timeline from moving.
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
        for (const QString &banned : { QStringLiteral("MatrixClient"),
                                       QStringLiteral("SettingsManager"),
                                       QStringLiteral("QSettings") }) {
            QVERIFY2(!store.contains(banned),
                     qPrintable(QStringLiteral("the hidden-image store reaches ")
                                + banned + QStringLiteral(", so it is not the "
                                                          "local session state "
                                                          "it claims to be")));
        }
        // Hide is offered while visible, Show is the placeholder's action, and
        // the already-hidden row is not offered a second Hide control.
        QVERIFY(clean.contains(QStringLiteral(
            "visible: root.mediaHideable && !root.mediaHidden")));
        QVERIFY(placeholder.contains(QStringLiteral("Show image")));
        // The placeholder's Label is not merely behind a Loader — the whole
        // body is gated on `hidden`, so a row that is never hidden creates no
        // Text at all. (A never-laid-out empty Text keeps
        // ItemObservesViewport for the delegate's life, and this is a per-row
        // delegate.)
        QVERIFY2(placeholder.contains(QStringLiteral("active: root.hidden")),
                 "the placeholder's contents are built for every media row");
    }

    void settingsOffersBothLayoutsAsPreviews()
    {
        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(settings.contains(QStringLiteral("navLayoutClassicCard")));
        QVERIFY(settings.contains(QStringLiteral("navLayoutChannelsCard")));
        // A diagram, not a live instance: a real presenter in a settings card
        // would need a room list and would change while you looked at it.
        const QString card = withoutComments(
            read(QStringLiteral("NavigationLayoutCard.qml")));
        QVERIFY(!card.isEmpty());
        QVERIFY2(!card.contains(QStringLiteral("app.roomList")),
                 "the preview card binds to the real room list");
        QVERIFY2(!card.contains(QStringLiteral("RoomListClassicPresenter")),
                 "the preview card instantiates a real presenter");
    }

    // THE DEFECT THIS EXISTS FOR: dropping a Space onto a Space never once
    // made a folder, through TWO rounds, and the model's own tests all passed
    // the whole time.
    //
    // They passed because they hand the model the row the pointer is aiming
    // at. Production could not produce it. Round one reordered as soon as the
    // pointer crossed a row's NEAR EDGE; round two moved the boundary to the
    // midpoint and then reordered anyway, because the resting branch ended in
    // `updateDrag(row, !dwellTimer.running)` and `running` is TRUE for the
    // whole 250 ms the dwell is being served. Either way the dragged block
    // took the row, the tile being aimed at stepped aside, and the row under
    // the pointer held the DRAGGED entry — which is never a group target.
    // (The same lesson as the row window: a policy test that invokes the
    // policy directly proves nothing about whether production reaches it.
    // tests/RailDragQmlTest.cpp is the one that drives a real pointer.)
    //
    // The fix is that the two readings are EXCLUSIVE and neither can disturb
    // the other's target: the TILE is the group target and NOTHING MOVES
    // while the pointer is on one; the GAP between tiles is the reorder
    // target. A gesture that never moves what it is aiming at cannot fail
    // the way both earlier rules did.
    void theRailNeverReordersIntoTheTileTheDragIsAimingAt()
    {
        const QString rail = withoutComments(read(QStringLiteral("SpacesRail.qml")));
        QVERIFY(!rail.isEmpty());
        QVERIFY2(rail.contains(QStringLiteral("function rowIsDraggedBlock(")),
                 "the dragged block's own slot is treated as a droppable row");

        // BOTH retired rules must stay retired. Each of these was the whole
        // gesture for a round, and each made grouping unreachable.
        QVERIFY2(!rail.contains(QStringLiteral("function pointerPushedThrough(")),
                 "the arrival-side midpoint rule is back");
        QVERIFY2(!rail.contains(QStringLiteral("function pointerOverTileCentre(")),
                 "the centre-band rule is back; reaching that band means "
                 "crossing the near edge first, which reorders");
        QVERIFY2(!rail.contains(QStringLiteral("dwellTimer")),
                 "the dwell is back — it existed to compensate for a reading "
                 "that moved things while the user was still aiming, and that "
                 "reading is gone");

        // THE INVARIANT. There is exactly one dispatch, and the branch that
        // reads a TILE may only arm or clear — never move. If a reorder call
        // ever appears in it, the defect is back.
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

        // The auto-scroll must go through the SAME dispatch. It used to end in
        // its own unconditional reorder, cancelling an armed grouping every
        // 16 ms while the pointer was near either end of the rail.
        const int scroll = rail.indexOf(QStringLiteral("id: autoScroll"));
        QVERIFY(scroll > 0);
        const QString scrollBody = rail.mid(scroll, 1400);
        QVERIFY2(scrollBody.contains(QStringLiteral("applyPointerReading(")),
                 "the auto-scroll has its own drag dispatch again");

        // And the model must offer three exclusive verbs, with no flag that
        // can turn an aim into a move.
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

    // Sable's Space menu, and the header that names which Space it belongs to
    // (the row it was opened from is no longer under the pointer once the menu
    // is up). Every action here is a real one: no dead rows.
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
        // The share link is the PUBLIC matrix.to permalink, never an
        // authenticated client or media URL.
        QVERIFY2(rail.contains(QStringLiteral("roomPermalink(")),
                 "the shared link is not the room permalink");
    }

    // A hidden menu row must take NO space. QQuickMenu lays its rows out in a
    // ListView that honours each item's height, and MenuSeparator's height
    // comes from its contentItem plus padding whether it is visible or not —
    // so the rail's Space menu opened with a 13px band above its first row,
    // left behind by the divider that belongs to the folder-only rows.
    void aHiddenMenuRowTakesNoHeight()
    {
        const QString sep = read(QStringLiteral("AppMenuSeparator.qml"));
        QVERIFY2(sep.contains(QStringLiteral("implicitHeight: visible ?")),
                 "a hidden separator still reserves its own height");
        const QString item = read(QStringLiteral("AppMenuItem.qml"));
        QVERIFY2(item.contains(QStringLiteral("implicitHeight: visible ?")),
                 "a hidden menu item still reserves its own height");
    }

    // Space settings is the ROOM settings backend behind a Space-shaped
    // surface. It must invent no Space-only storage: the reference client's
    // Cosmetics / Abbreviations / Emojis / Appearance pages have no Matrix
    // state behind them, and shipping them would mean writing a private
    // format only Lightning could read while presenting it as the Space's.
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
        // No local invention: nothing here may reach settings storage.
        QVERIFY2(!dialog.contains(QStringLiteral("app.settings")),
                 "space settings writes device-local state and presents it as "
                 "part of the Space");
        QVERIFY2(!dialog.contains(QStringLiteral("app.railLayout")),
                 "space settings writes the local rail arrangement");
        // The restricted join rule is displayed honestly and left alone: its
        // allow-rule list is not something this surface can build.
        QVERIFY2(dialog.contains(QStringLiteral("knock_restricted")),
                 "a space-restricted join rule is no longer detected, so this "
                 "surface would offer to overwrite it with an empty allow list");
        // Themed throughout — no literal colours anywhere.
        QVERIFY2(!dialog.contains(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}"))),
                 "space settings hardcodes a colour");
    }
};

QTEST_MAIN(NavigationLayoutContractTest)
#include "NavigationLayoutContractTest.moc"
