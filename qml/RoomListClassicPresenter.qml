import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root

    clip: true

    property string currentRoomId: ""
    property int selectionAnchor: -1
    function selectAt(row, roomId, modifiers) {
        if ((modifiers & Qt.ShiftModifier) && selectionAnchor >= 0)
            app.roomBatch.selectRange(app.roomList.roomIdsBetween(selectionAnchor, row))
        else
            app.roomBatch.toggle(roomId)
        selectionAnchor = row
    }
    Keys.onEscapePressed: app.roomBatch.exitSelection()

    readonly property bool atRest: !roomList.moving && roomList.atYBeginning

    signal roomActivated(string roomId)
    signal roomOpenedByPointer(string roomId)

    property string pendingAcceptRoomId: ""
    function openAcceptedRoom() {
        const id = pendingAcceptRoomId
        if (id === "" || app.roomList.findRoom(id).membership !== "joined")
            return
        pendingAcceptRoomId = ""
        roomActivated(id)
    }
    Connections {
        target: app.roomList
        function onDataChanged() { root.openAcceptedRoom() }
        function onRowsInserted() { root.openAcceptedRoom() }
        function onModelReset() { root.openAcceptedRoom() }
    }
    Timer {
        interval: 30000
        running: root.pendingAcceptRoomId !== ""
        onTriggered: root.pendingAcceptRoomId = ""
    }
    Connections {
        target: app
        function onCurrentRoomIdChanged() { root.pendingAcceptRoomId = "" }
    }
    Connections {
        target: app.accounts
        function onActiveUserIdChanged() { root.pendingAcceptRoomId = "" }
    }

    readonly property string conversationSectionLabel: {
        switch (app.roomList ? app.roomList.filterMode : 0) {
        case 1:  return qsTr("People")
        case 2:  return qsTr("Rooms")
        case 3:  return qsTr("Unread")
        case 4:  return qsTr("Favourites")
        default: return qsTr("Conversations")
        }
    }
    signal createRequested(string mode)
    signal discoverRequested
    signal roomLinkCopyRequested(string roomId)
    signal clearSearchRequested
    signal leaveRoomRequested(string roomId, string roomName)
    signal chatBackgroundRequested(string roomId, string roomName)
    signal inviteRequested(string roomId)
    signal inviterProfileRequested(var member, Item anchor)
    signal inviteRejectRequested(string roomId, string roomName)

    ListView {
        id: roomList
        objectName: "roomListView"
        anchors.fill: parent
        // The SplitView divider beside this column (MainScreen's handle) has
        // a 9px grab mask reaching 4px back into it, which is exactly where
        // the scrollbar lives: a press on the thumb or the track went to the
        // divider and the list never moved (C04-F02). Keep the bar clear of it.
        anchors.rightMargin: 5
        clip: true
        model: app.roomList
        currentIndex: -1
        spacing: 0
        cacheBuffer: 600
        reuseItems: true
        keyNavigationEnabled: false
        function focusRow(i) {
            if (count <= 0)
                return
            const target = Math.max(0, Math.min(count - 1, i))
            currentIndex = target
            positionViewAtIndex(target, ListView.Contain)
            Qt.callLater(function() {
                if (roomList.currentItem)
                    roomList.currentItem.forceActiveFocus(Qt.TabFocusReason)
            })
        }
        Keys.onPressed: (event) => {
            const page = Math.max(1, Math.floor(height / 56) - 1)
            const here = currentIndex < 0 ? 0 : currentIndex
            const prior = currentIndex
            switch (event.key) {
            case Qt.Key_Down:     focusRow(currentIndex < 0 ? 0 : here + 1); break
            case Qt.Key_Up:       focusRow(here - 1); break
            case Qt.Key_PageDown: focusRow(here + page); break
            case Qt.Key_PageUp:   focusRow(here - page); break
            case Qt.Key_Home:     focusRow(0); break
            case Qt.Key_End:      focusRow(count - 1); break
            default: return
            }
            if ((event.modifiers & Qt.ShiftModifier) && app.roomBatch.selecting
                && roomList.currentIndex >= 0 && prior >= 0) {
                if (root.selectionAnchor < 0) root.selectionAnchor = prior
                app.roomBatch.selectRange(app.roomList.roomIdsBetween(root.selectionAnchor, roomList.currentIndex))
            }
            event.accepted = true
        }

        ScrollBar.vertical: AppScrollBar {
            policy: ScrollBar.AsNeeded
        }

        Connections {
            target: app.roomList
            function onFilterModeChanged() {
                Qt.callLater(() => roomList.positionViewAtBeginning())
            }
        }

        section.property: "category"
        section.criteria: ViewSection.FullString
        section.labelPositioning: ViewSection.InlineLabels
                                  | ViewSection.CurrentLabelAtStart
        // Re-attached whenever the list empties: ListView keeps the section
        // label items it made for rows that are gone, and when rows come back
        // (an account switch: the old account's list empties, the next one
        // fills it) a stale label is drawn again where its old section began.
        // Live 2026-10-26, Classic + People filter, signing out one of two
        // accounts: a second "People" header floated mid-list over nothing.
        // Setting the delegate releases every section item, so the next rows
        // get fresh labels for the sections they actually have.
        property bool sectionLabelsAttached: true
        function reattachSectionLabels() {
            sectionLabelsAttached = false
            sectionLabelsAttached = true
        }
        onCountChanged: if (count === 0) reattachSectionLabels()
        section.delegate: sectionLabelsAttached ? sectionHeaderComponent : null

        Component {
            id: sectionHeaderComponent
            Rectangle {
                required property string section
                objectName: "roomListSectionHeader"
                width: roomList.width
                height: 28
                color: AppTheme.roomList
                // A header only heads rows. When a filter or an empty Space leaves
                // the list with none, ListView keeps the last section's label
                // instance (the pinned CurrentLabelAtStart one included), and it
                // was drawn over the empty state ("InvRooms added to this Space",
                // live 2026-10-26). Opacity, never `visible`: ListView parks a
                // label it no longer needs with setVisible(false), and a
                // binding here would re-show the parked item at its old place
                // on every count change (doubled headers, live 2026-11-01).
                opacity: roomList.count > 0 ? 1 : 0

                Label {
                    anchors {
                        left: parent.left
                        verticalCenter: parent.verticalCenter
                        leftMargin: AppTheme.spacing12
                    }
                    text: section === "call"
                          ? qsTr("In a call")
                          : section === "invite"
                          ? qsTr("Invites")
                          : (section === "favourite"
                             ? qsTr("Favourites")
                             : (section === "lowpriority"
                                ? qsTr("Low priority")
                                : root.conversationSectionLabel))
                    color: AppTheme.roomListSection
                    font.family: AppTheme.menuSectionFont
                    font.pixelSize: AppTheme.menuSectionSize
                    font.weight: AppTheme.menuSectionWeight
                    font.letterSpacing: AppTheme.menuSectionTracking
                }
            }
        }

        delegate: RoomDelegate {
            id: rowDelegate
            sharedRowMenu: sharedRowMenuHost
            width: ListView.view.width
            selected: model.roomId === app.currentRoomId
                      || (app.roomBatch.selectionCount >= 0 && app.roomBatch.isSelected(model.roomId))
            activeFocusOnTab: ListView.view.currentIndex < 0
                              ? index === 0
                              : index === ListView.view.currentIndex
            onActiveFocusChanged: if (activeFocus && ListView.view)
                ListView.view.currentIndex = index
            Keys.onReturnPressed: (event) => rowDelegate.openFromKeyboard(event)
            Keys.onEnterPressed: (event) => rowDelegate.openFromKeyboard(event)
            Keys.onSpacePressed: (event) => {
                if (event.isAutoRepeat)
                    event.accepted = true
                else if (app.roomBatch.selecting || (event.modifiers & Qt.ControlModifier)) {
                    root.selectAt(index, model.roomId, event.modifiers)
                    event.accepted = true
                }
                else
                    rowDelegate.openFromKeyboard(event)
            }
            Keys.onEscapePressed: app.roomBatch.exitSelection()
            function openFromKeyboard(event) {
                if (model.membership === "joined") {
                    root.roomActivated(model.roomId)
                    event.accepted = true
                }
            }
            showGroupDivider: app.roomList.favouritesBoundaryRoomId.length > 0 && model.roomId === app.roomList.favouritesBoundaryRoomId
            onClicked: {
                root.pendingAcceptRoomId = ""
                ListView.view.currentIndex = index
                const modifiers = app.currentKeyboardModifiers()
                if (app.roomBatch.selecting || (modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
                    root.selectAt(index, model.roomId, modifiers)
                    return
                }
                if (model.membership === "joined") {
                    root.roomActivated(model.roomId)
                    root.roomOpenedByPointer(model.roomId)
                }
            }
            onAcceptInvite: {
                root.pendingAcceptRoomId = model.roomId
                app.roomList.acceptInvite(model.roomId)
            }
            onRejectInvite: app.roomList.rejectInvite(model.roomId)
            onMarkRead: app.roomList.markRoomRead(model.roomId)
            onMarkUnread: app.roomList.markRoomUnread(model.roomId)
            onSetFavourite: on => app.roomList.setRoomFavourite(model.roomId, on)
            onSetLowPriority: on => app.roomList.setRoomLowPriority(model.roomId, on)
            onSetNotificationMode: mode => app.setRoomNotificationMode(model.roomId, mode)
            onCopyRoomLink: root.roomLinkCopyRequested(model.roomId)
            onLeaveRoomRequested: root.leaveRoomRequested(model.roomId, model.name)
            onChatBackgroundRequested: root.chatBackgroundRequested(model.roomId, model.name)
            onInviteRequested: root.inviteRequested(model.roomId)
            onSelectRoomRequested: root.selectAt(index, model.roomId, 0)
            onInviterProfileRequested: (member, anchor) =>
                root.inviterProfileRequested(member, anchor)
        }
    }

    RoomListAttentionLocator {
        z: 30
        anchors.fill: parent
        list: roomList
        attentionModel: app.roomList
    }

    ColumnLayout {
        id: roomListEmptyState
        objectName: "roomListEmptyState"
        visible: roomList.count === 0
        anchors.centerIn: parent
        width: parent.width - AppTheme.spacing24 * 2
        spacing: AppTheme.spacing12

        readonly property bool searching: app.roomList && (app.roomList.searchQuery || "").length > 0
        readonly property bool inSpace: app.spaces && app.spaces.activeSpaceId && app.spaces.activeSpaceId !== "" && app.spaces.activeSpaceId !== "@orphans"
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
                default:
                    if (app.roomList.filterMode === 1)
                        return qsTr("No direct messages");
                    if (app.roomList.filterMode === 3)
                        return qsTr("Nothing unread");
                    if (app.roomList.filterMode === 4)
                        return qsTr("No favourites yet");
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
                    if (app.roomList.filterMode === 1)
                        return roomListEmptyState.inSpace ? qsTr("No direct messages with people in " + "this Space. All of them are under “All rooms”.") : qsTr("Direct messages appear here.");
                    if (app.roomList.filterMode === 3)
                        return qsTr("Rooms with unread messages " + "appear here.");
                    if (app.roomList.filterMode === 4)
                        return qsTr("Favourite a room from its menu to pin it here.");
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
    MiddleClickScroller {
        id: middleClickScroller
        objectName: "roomListMiddleClickScroller"
        anchors.fill: parent
        z: 1
        view: roomList
    }
    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            middleClickScroller.stop()
        }
        function onAccountSwitchingChanged() {
            if (app.accountSwitching)
                middleClickScroller.stop()
        }
    }

    Loader {
        id: sharedRowMenuHost
        objectName: "sharedRoomMenuHost"
        asynchronous: true
        property var menuRoom: ({})

        function openFor(room) {
            menuRoom = room
            asynchronous = false
            active = true
            if (!item) {
                active = false
                active = true
            }
            if (!item)
                return false
            item.popup()
            return true
        }
        function closeIfRoomLeftTheList() {
            if (!item || !item.visible)
                return
            const rows = app.roomList.roomIdsBetween(0, roomList.count - 1)
            if (rows.indexOf(menuRoom.roomId) < 0)
                item.close()
        }

        Connections {
            target: app.roomList
            function onRowsRemoved() {
                Qt.callLater(sharedRowMenuHost.closeIfRoomLeftTheList)
            }
            function onModelReset() {
                Qt.callLater(sharedRowMenuHost.closeIfRoomLeftTheList)
            }
        }

        sourceComponent: RoomActionsMenu {
            objectName: "roomContextMenu"
            readonly property var room: sharedRowMenuHost.menuRoom
            roomId: room.roomId || ""
            roomName: room.roomName || ""
            canonicalAlias: room.canonicalAlias || ""
            isDirect: room.isDirect === true
            isFavourite: room.isFavourite === true
            isLowPriority: room.isLowPriority === true
            isUnread: room.isUnread === true
            onMarkRead: app.roomList.markRoomRead(roomId)
            onMarkUnread: app.roomList.markRoomUnread(roomId)
            onSetFavourite: on => app.roomList.setRoomFavourite(roomId, on)
            onSetLowPriority: on => app.roomList.setRoomLowPriority(roomId, on)
            onSetNotificationMode: mode => app.setRoomNotificationMode(roomId, mode)
            onCopyRoomLink: root.roomLinkCopyRequested(roomId)
            onLeaveRoomRequested: root.leaveRoomRequested(roomId, roomName)
            onChatBackgroundRequested: root.chatBackgroundRequested(roomId, roomName)
            onInviteRequested: root.inviteRequested(roomId)
            onSelectRoomRequested: {
                const rows = app.roomList.roomIdsBetween(0, roomList.count - 1)
                root.selectAt(rows.indexOf(roomId), roomId, 0)
            }
        }
    }
}
