import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// The Classic layout's list body. A presenter: RoomsPanel (the host) owns the
// header, search and dialogs and swaps this in place of the Channels list, so
// both layouts share the same chrome. The default layout, since it works for
// any account: one activity-ordered list (invites, favourites, then
// conversations) with previews and timestamps. The section-label and
// group-divider contract tests scan this file.
Item {
    id: root

    // Backstop against overflow into the timeline; the empty-state buttons wrap
    // (see below).
    clip: true

    /// The room the timeline is showing.
    property string currentRoomId: ""

    // The host owns the dialogs, so rows ask by signal rather than reaching
    // into a parent by id.
    signal roomActivated(string roomId)

    // Mirrors RoomListModel's filter modes: 0 all, 1 People, 2 Rooms, 3
    // Unreads. Read once so the section delegate stays a binding.
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
    /// The search field belongs to the host; it is not in this file's scope.
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
        // Instantiate a screen's worth of rows past the viewport so avatars
        // start fetching early.
        cacheBuffer: 600
        // Recycle delegates; RoomDelegate keeps no per-instance state that
        // could leak across rows.
        reuseItems: true

        ScrollBar.vertical: AppScrollBar {
            policy: ScrollBar.AsNeeded
        }

        // Sections from the "category" role; C++ sorts by the same
        // RoomListModel::orderRankOf(), so sort and headers cannot disagree.
        section.property: "category"
        section.criteria: ViewSection.FullString
        // Pinned group headers, as Element does. labelPositioning is a flag
        // set: CurrentLabelAtStart alone would draw only the current section's
        // label and drop every other header, so InlineLabels is OR-ed in. The
        // delegate is opaque sidebar so it covers the rows it floats over.
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
                // Sentence case with the shared section-label tokens.
                // categoryOf() yields "invite", "favourite" and "conversation":
                // joined rows are one activity feed with DMs and rooms
                // interleaved, so splitting by kind would repeat headers
                // constantly. The conversation section is named after the
                // active filter chip ("Conversations" under All).
                text: section === "invite"
                      ? qsTr("Invites")
                      : (section === "favourite"
                         ? qsTr("Favourites")
                         : root.conversationSectionLabel)
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
            // Favourites are now interleaved by recency, so the model reports
            // an empty boundary; the binding stays for whatever divides next.
            showGroupDivider: app.roomList.favouritesBoundaryRoomId.length > 0 && model.roomId === app.roomList.favouritesBoundaryRoomId
            onClicked: if (model.membership === "joined")
                root.roomActivated(model.roomId)
            onAcceptInvite: app.roomList.acceptInvite(model.roomId)
            onRejectInvite: app.roomList.rejectInvite(model.roomId)
            onMarkRead: app.roomList.markRoomRead(model.roomId)
            onMarkUnread: app.roomList.markRoomUnread(model.roomId)
            onSetFavourite: on => app.roomList.setRoomFavourite(model.roomId, on)
            onSetNotificationMode: mode => app.setRoomNotificationMode(model.roomId, mode)
            // By signal: the clipboard proxy and confirm dialog belong to the
            // host.
            onCopyRoomLink: root.roomLinkCopyRequested(model.roomId)
            onLeaveRoomRequested: root.leaveRoomRequested(model.roomId, model.name)
        }
    }

    // Empty/loading state: glyph, heading, one line and the actions that
    // resolve it, using the same dialogs as the header buttons.
    ColumnLayout {
        id: roomListEmptyState
        objectName: "roomListEmptyState"
        visible: roomList.count === 0
        anchors.centerIn: parent
        width: parent.width - AppTheme.spacing24 * 2
        spacing: AppTheme.spacing12

        readonly property bool searching: app.roomList && (app.roomList.searchQuery || "").length > 0
        readonly property bool inSpace: app.spaces && app.spaces.activeSpaceId && app.spaces.activeSpaceId !== "" && app.spaces.activeSpaceId !== "@orphans"
        // Signed out, still syncing and genuinely empty are different states;
        // offering "New message" mid-sync would invite acting on missing data.
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
                // Keyed on the filter, not only on the Space.
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
                    // People is scoped to the selected Space (DMs with its
                    // members); say so in a Space, and say what the list is at
                    // Home.
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

        // A GridLayout that drops to one column: a RowLayout would overflow a
        // narrow pane at the buttons' minimum widths.
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
    // Middle-click autoscroll, a sibling of the view; middle button only.
    MiddleClickScroller {
        objectName: "roomListMiddleClickScroller"
        anchors.fill: parent
        z: 1
        view: roomList
    }
}
