import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The Classic navigation layout's list body.
//
// A PRESENTER: it owns no workspace header, no search field and no dialogs.
// RoomsPanel (the host) owns those and swaps this in place of the Channels
// list, so the two layouts cannot fork the surrounding chrome — every user of
// either layout gets the same header and the same Ctrl-K hint.
//
// This is the layout Lightning shipped through 0.7.6 and it is the DEFAULT
// for a reason: it works in every account, including one with no Spaces at
// all. One activity-ordered list, invites then favourites then DMs then
// rooms, with previews and timestamps. Channels is the alternative answer for
// a Space-shaped workspace, not an improvement on this one.
//
// Extracted verbatim from RoomsPanel.qml, which is why the section-label and
// group-divider contract tests now scan THIS file: the invariants did not
// change, only the file the code lives in.
Item {
    id: root

    // The column is 300px and the pane next to it is not ours to draw on.
    // The empty state's action buttons have a real minimum width and
    // QtQuickLayouts does not shrink a button below it, so on a narrow
    // column the row overflowed and the buttons were painted over the
    // timeline — reported as "buttons get overlapped" and "the UI gets under
    // the screen". The buttons wrap now (see the Flow below); this is the
    // backstop that makes overflow impossible rather than merely unlikely.
    clip: true

    /// The room the timeline is showing.
    property string currentRoomId: ""

    // The host owns the dialogs, so the rows ask for them by signal rather
    // than reaching up into a parent by id — which is what made the reader
    // popover's click silently dead when it was a pane-root function.
    signal roomActivated(string roomId)

    // Mirrors RoomListModel's filter modes: 0 all, 1 People, 2 Rooms,
    // 3 Unreads. Read once here so the section delegate stays a binding
    // rather than repeating the mapping per header.
    readonly property string conversationSectionLabel: {
        switch (app.roomList ? app.roomList.filterMode : 0) {
        case 1:  return qsTr("People")
        case 2:  return qsTr("Rooms")
        case 3:  return qsTr("Unread")
        default: return qsTr("Conversations")
        }
    }
    signal createRequested(string mode)
    signal discoverRequested
    signal roomLinkCopyRequested(string roomId)
    /// The search field belongs to the HOST. `roomSearch` is not in this
    /// file's scope, so calling it here threw a ReferenceError and the button
    /// did nothing — the same class of dead click as the reader popover.
    signal clearSearchRequested
    signal leaveRoomRequested(string roomId, string roomName)
    signal inviteRejectRequested(string roomId, string roomName)

    ListView {
        id: roomList
        anchors.fill: parent
        clip: true
        model: app.roomList
        currentIndex: -1
        spacing: 0
        // Instantiate delegates a little past the viewport so their
        // avatars start fetching before the row scrolls into view.
        // Bounded prefetch: roughly one extra screen of rows.
        cacheBuffer: 600
        // Fast-scroll: recycle row delegates instead of re-creating
        // them (RoomDelegate keeps no per-instance state that could
        // leak across model rows).
        reuseItems: true

        ScrollBar.vertical: AppScrollBar {
            policy: ScrollBar.AsNeeded
        }

        // Section grouping driven by the "category" role from
        // RoomListModel. C++ sorts by RoomListModel::orderRankOf(), which
        // is the same function the role reads, so the sort and the headers
        // cannot disagree: invites, then one activity feed holding every
        // joined conversation.
        section.property: "category"
        section.criteria: ViewSection.FullString
        // Element pins its group headers. The default (InlineLabels)
        // scrolls the label away with its content, so halfway down a long
        // ROOMS section nothing on screen says which group you are in.
        // The delegate is opaque sidebar, so it occludes the rows it
        // floats over correctly; ListView already raises the current
        // section label above them.
        //
        // InlineLabels is OR-ed in, and leaving it out was a real defect
        // rather than a preference: labelPositioning is a FLAG SET, so
        // CurrentLabelAtStart alone means the pinned label for the current
        // section is the ONLY one drawn — every later section header
        // vanished. The list then showed "Favourites" at the top, the
        // favourites-group rule under the last starred room, and no header
        // over the DMs and rooms below it. Reported, accurately, as "a
        // random line under a room under favourites".
        section.labelPositioning: ViewSection.InlineLabels
                                  | ViewSection.CurrentLabelAtStart
        section.delegate: Rectangle {
            required property string section
            width: roomList.width
            height: 28
            color: AppTheme.sidebar

            Label {
                anchors {
                    left: parent.left
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing12
                }
                // Sentence case on the UI face, not 11px ExtraBold caps
                // at 1.2px tracking: uppercase-plus-tracking is
                // decorative typography carrying wayfinding text, and it
                // was re-typed inline in seven places across the app.
                // These are the shared section-label tokens.
                //
                // categoryOf() now yields exactly two values — "invite"
                // and "conversation" — because the joined rows below the
                // invites are ONE activity feed with DMs and rooms
                // interleaved. Splitting that by kind would repeat its
                // header every time the two alternate, which in a list
                // ordered by when people spoke is constantly.
                //
                // So the conversation section takes its name from the
                // active filter chip, which is what the section actually
                // contains. Under "All" it says Conversations, because
                // calling a list that holds both People or Rooms would be
                // a lie about half of it.
                text: section === "invite"
                      ? qsTr("Invites")
                      : root.conversationSectionLabel
                color: AppTheme.sectionLabelColor
                font.family: AppTheme.menuSectionFont
                font.pixelSize: AppTheme.menuSectionSize
                font.weight: AppTheme.menuSectionWeight
                font.letterSpacing: AppTheme.menuSectionTracking
            }
        }

        delegate: RoomDelegate {
            width: ListView.view.width
            selected: model.roomId === app.currentRoomId
            // The favourites rule is retired along with the favourites
            // GROUP: favourites are now interleaved with everything else
            // by recency, so the rows it separated are no longer adjacent
            // and a line anywhere in the feed would divide nothing. The
            // model reports an empty boundary permanently; the binding is
            // kept so the property stays live for whatever divides next.
            showGroupDivider: app.roomList.favouritesBoundaryRoomId.length > 0 && model.roomId === app.roomList.favouritesBoundaryRoomId
            onClicked: if (model.membership === "joined")
                root.roomActivated(model.roomId)
            onAcceptInvite: app.roomList.acceptInvite(model.roomId)
            onRejectInvite: app.roomList.rejectInvite(model.roomId)
            onMarkRead: app.roomList.markRoomRead(model.roomId)
            onMarkUnread: app.roomList.markRoomUnread(model.roomId)
            onSetFavourite: on => app.roomList.setRoomFavourite(model.roomId, on)
            onSetNotificationMode: mode => app.setRoomNotificationMode(model.roomId, mode)
            // Asked for by SIGNAL rather than by reaching up into the host
            // by id. The clipboard proxy and the confirm dialog belong to
            // the host, and a delegate that walks its parent chain by name
            // is how the reader popover's click ended up silently dead.
            onCopyRoomLink: root.roomLinkCopyRequested(model.roomId)
            onLeaveRoomRequested: root.leaveRoomRequested(model.roomId, model.name)
        }
    }

    // Empty / loading state — centred over the (empty) list area.
    // 2026-08-21: was one grey sentence floating in a 300px void. An
    // empty pane is a designed state, not a missing one: glyph, a
    // heading that names the state, one honest line, and the actions
    // that actually resolve it. The actions are the SAME dialogs the
    // header buttons open, so there is one create path, not two.
    ColumnLayout {
        id: roomListEmptyState
        objectName: "roomListEmptyState"
        visible: roomList.count === 0
        anchors.centerIn: parent
        width: parent.width - AppTheme.spacing24 * 2
        spacing: AppTheme.spacing12

        readonly property bool searching: app.roomList && (app.roomList.searchQuery || "").length > 0
        readonly property bool inSpace: app.spaces && app.spaces.activeSpaceId && app.spaces.activeSpaceId !== "" && app.spaces.activeSpaceId !== "@orphans"
        // "Signed out", "still syncing" and "genuinely empty" are three
        // different facts and the pane says which one it is — offering
        // "New message" while the initial sync is still running would
        // invite the user to act on an answer we do not have yet.
        readonly property int phase: !app.loggedIn ? 0 : !app.initialSyncDone ? 1 : searching ? 2 : 3

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 56
            implicitHeight: 56
            radius: width / 2
            color: AppTheme.chipNeutralFill
            Icon {
                anchors.centerIn: parent
                name: roomListEmptyState.phase === 0 ? "account_circle" : roomListEmptyState.phase === 2 ? "search" : "forum"
                size: 26
                color: AppTheme.textMuted
            }
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: {
                switch (roomListEmptyState.phase) {
                case 0:
                    return qsTr("Sign in to see rooms");
                case 1:
                    return qsTr("Loading rooms…");
                case 2:
                    return qsTr("No matches");
                // Keyed on the FILTER, not only on the Space. "This
                // Space is empty" under the People chip was answering a
                // question nobody asked — the Space's rooms are not what
                // that list is showing.
                default:
                    if (app.roomList.filterMode === 1)
                        return qsTr("No direct messages");
                    if (app.roomList.filterMode === 3)
                        return qsTr("Nothing unread");
                    return roomListEmptyState.inSpace ? qsTr("This Space is empty") : qsTr("No conversations yet");
                }
            }
            color: AppTheme.textPrimary
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            visible: text.length > 0
            text: {
                switch (roomListEmptyState.phase) {
                case 0:
                    return "";
                case 1:
                    return qsTr("Your rooms appear here once the " + "first sync finishes.");
                case 2:
                    return qsTr("Nothing in this list matches " + "\"%1\".").arg(app.roomList.searchQuery);
                default:
                    // 2026-08-28: People is SCOPED to the selected Space —
                    // the DMs with people who are in it — so the old
                    // "whichever Space is selected" is no longer true and
                    // would send a user looking for a chat that is one click
                    // away at Home. In a Space the line names the scope; at
                    // Home it says what the list is.
                    if (app.roomList.filterMode === 1)
                        return roomListEmptyState.inSpace ? qsTr("No direct messages with people in " + "this Space. All of them are under “All rooms”.") : qsTr("Direct messages appear here.");
                    if (app.roomList.filterMode === 3)
                        return qsTr("Rooms with unread messages " + "appear here.");
                    return roomListEmptyState.inSpace ? qsTr("Rooms added to this Space will " + "show up here.") : qsTr("Start a direct message, or find " + "a room to join.");
                }
            }
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
        }

        AppButton {
            objectName: "roomListClearSearchButton"
            Layout.alignment: Qt.AlignHCenter
            visible: roomListEmptyState.phase === 2
            text: qsTr("Clear search")
            onClicked: root.clearSearchRequested()
        }

        // A GridLayout that drops to one column, not a RowLayout: two buttons
        // side by side need about 300px and this column can be narrower than
        // that. A RowLayout keeps them on one line at their minimum width and
        // lets the line run past the pane — which is how they ended up painted
        // over the timeline. Stacking is the honest answer to "there is not
        // enough room", and the layout still sizes to its content so
        // AlignHCenter keeps the block centred like everything above it.
        GridLayout {
            Layout.alignment: Qt.AlignHCenter
            columns: roomListEmptyState.width >= 300 ? 2 : 1
            rowSpacing: AppTheme.spacing8
            columnSpacing: AppTheme.spacing8
            visible: roomListEmptyState.phase === 3
            AppButton {
                objectName: "roomListEmptyNewButton"
                kind: "primary"
                visible: app.loggedIn && app.conversations && app.conversations.supported
                text: qsTr("New message")
                onClicked: root.createRequested("dm")
            }
            AppButton {
                objectName: "roomListEmptyDiscoverButton"
                visible: app.loggedIn && app.discovery && app.discovery.supported
                text: qsTr("Explore rooms")
                onClicked: root.discoverRequested()
            }
        }
    }
    // Desktop autoscroll (2026-08-18 tester report). Sibling of the
    // view, middle button only, so row clicks and hover are untouched.
    MiddleClickScroller {
        objectName: "roomListMiddleClickScroller"
        anchors.fill: parent
        z: 1
        view: roomList
    }
}
