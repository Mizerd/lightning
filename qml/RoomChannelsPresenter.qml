import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: root

    property string currentRoomId: ""
    property int selectionAnchor: -1
    function selectAt(row, roomId, modifiers) {
        if ((modifiers & Qt.ShiftModifier) && selectionAnchor >= 0)
            app.roomBatch.selectRange(app.spaceChannels.roomIdsBetween(selectionAnchor, row))
        else
            app.roomBatch.toggle(roomId)
        selectionAnchor = row
    }
    Keys.onEscapePressed: app.roomBatch.exitSelection()
    readonly property bool atRest: !channelList.moving && channelList.atYBeginning
    readonly property bool lobbyActive: app.currentRoomId === ""

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
    signal lobbyActivated()
    signal createRoomRequested()
    signal joinAddressRequested()
    signal exploreSpacesRequested()
    signal createChatRequested()
    signal messageSearchRequested()
    signal roomLinkCopyRequested(string roomId)
    signal leaveRoomRequested(string roomId, string roomName)
    signal chatBackgroundRequested(string roomId, string roomName)
    signal inviteRequested(string roomId)

    Loader {
        anchors.centerIn: parent
        width: parent.width - AppTheme.spacing24 * 2
        active: app.spaceChannels.empty
        visible: active
        sourceComponent: Label {
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
            text: qsTr("No conversations yet. Rooms you join, and the spaces " + "they belong to, will show up here.")
        }
    }

    Loader {
        anchors.centerIn: parent
        width: parent.width - AppTheme.spacing24 * 2
        active: app.spaceChannels.matchCount === 0 && !app.spaceChannels.empty
        visible: active
        sourceComponent: Label {
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
            color: AppTheme.textMuted
            font.pixelSize: AppTheme.textBody
            text: {
                if (app.spaceChannels.searchQuery.trim().length > 0)
                    return qsTr("Nothing in this list matches \"%1\".").arg(app.spaceChannels.searchQuery);
                if (app.spaceChannels.filterMode === 3)
                    return qsTr("Nothing unread. Everything in this view has been read.");
                if (app.spaceChannels.viewKind === "people")
                    return qsTr("No direct messages yet. Start one with Create Chat.");
                if (app.spaceChannels.viewKind === "space")
                    return qsTr("Nothing in this space yet. Open Lobby to add a room.");
                return qsTr("No rooms outside your spaces. Pick a space on the left, or create a room.");
            }
        }
    }

    ListView {
        id: channelList
        objectName: "channelList"
        anchors.fill: parent
        clip: true
        model: app.spaceChannels
        currentIndex: -1
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Escape && app.roomBatch.selecting) {
                app.roomBatch.exitSelection()
                event.accepted = true
                return
            }
            if (!app.roomBatch.selecting || !(event.modifiers & Qt.ShiftModifier)
                || (event.key !== Qt.Key_Up && event.key !== Qt.Key_Down)) return
            const previous = currentIndex < 0 ? 0 : currentIndex
            const next = Math.max(0, Math.min(count - 1,
                              previous + (event.key === Qt.Key_Down ? 1 : -1)))
            currentIndex = next
            positionViewAtIndex(next, ListView.Contain)
            if (root.selectionAnchor < 0) root.selectionAnchor = previous
            app.roomBatch.selectRange(app.spaceChannels.roomIdsBetween(root.selectionAnchor, next))
            event.accepted = true
        }
        spacing: 0
        cacheBuffer: 400
        // Safe to recycle: ChannelDelegate re-queries its mute mode on every id
        // change.
        reuseItems: true
        // Stop at the edges: the default flick overshoot drew a blank gap above
        // the first row after a fast wheel burst.
        boundsBehavior: Flickable.StopAtBounds

        Connections {
            target: app.spaceChannels
            function onFilterModeChanged() {
                Qt.callLater(() => channelList.positionViewAtBeginning())
            }
        }

        ScrollBar.vertical: AppScrollBar {
            policy: ScrollBar.AsNeeded
        }

        header: Item {
            width: channelList.width
            height: AppTheme.spacing8
        }
        footer: Item {
            width: channelList.width
            height: AppTheme.spacing12
        }

        delegate: Loader {
            id: rowLoader
            width: channelList.width
            required property var model
            required property int index

            // One Loader choosing a component per row kind (the kinds share
            // nothing). The chooser must name every kind the model produces, or
            // a row falls through to the channel-row component.
            sourceComponent: rowLoader.model.kind === "lobby" ? lobbyComponent : (rowLoader.model.kind === "search" ? searchComponent : (rowLoader.model.kind === "action" ? actionComponent : (rowLoader.model.kind === "space" ? spaceComponent : (rowLoader.model.kind === "group" ? groupComponent : channelComponent))))

            Component {
                id: lobbyComponent
                ChannelNavRow {
                    width: channelList.width
                    label: rowLoader.model.name
                    iconName: rowLoader.model.iconName
                    current: root.lobbyActive
                    onClicked: root.lobbyActivated()
                }
            }

            Component {
                id: searchComponent
                ChannelNavRow {
                    width: channelList.width
                    label: rowLoader.model.name
                    iconName: rowLoader.model.iconName
                    onClicked: root.messageSearchRequested()
                }
            }

            Component {
                id: actionComponent
                ChannelNavRow {
                    width: channelList.width
                    label: rowLoader.model.name
                    iconName: rowLoader.model.iconName
                    onClicked: {
                        var id = rowLoader.model.roomId
                        if (id === "@new-room")
                            root.createRoomRequested()
                        else if (id === "@join-address")
                            root.joinAddressRequested()
                        else if (id === "@explore")
                            root.exploreSpacesRequested()
                        else if (id === "@new-chat")
                            root.createChatRequested()
                    }
                }
            }

            Component {
                id: channelComponent
                ChannelDelegate {
                    width: channelList.width
                    roomId: rowLoader.model.roomId
                    channelName: rowLoader.model.name
                    avatarUrl: rowLoader.model.avatarUrl
                    identityColorKey: rowLoader.model.identityColorKey
                    isDirect: rowLoader.model.isDirect
                    isInvite: rowLoader.model.isInvite
                    encrypted: rowLoader.model.encrypted
                    unreadCount: rowLoader.model.unreadCount
                    highlightCount: rowLoader.model.highlightCount
                    hasUnread: rowLoader.model.hasUnread
                    threadUnreadKind: rowLoader.model.threadUnreadKind || 0
                    isFavourite: rowLoader.model.isFavourite
                    isLowPriority: rowLoader.model.isLowPriority
                    depth: rowLoader.model.depth
                    active: rowLoader.model.roomId === root.currentRoomId
                    batchSelected: app.roomBatch.selectionCount >= 0
                                   && app.roomBatch.isSelected(rowLoader.model.roomId)
                    onHoveredChanged: app.spaceChannels.setRoomHovered(
                        rowLoader.model.roomId, hovered)
                    onClicked: {
                        root.pendingAcceptRoomId = ""
                        const byPointer = pointerActivation
                        pointerActivation = false
                        if (heldForMenu) {
                            heldForMenu = false
                            return
                        }
                        const modifiers = app.currentKeyboardModifiers()
                        if (app.roomBatch.selecting || (modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
                            root.selectAt(rowLoader.index, rowLoader.model.roomId, modifiers)
                            return
                        }
                        root.roomActivated(rowLoader.model.roomId)
                        if (byPointer)
                            root.roomOpenedByPointer(rowLoader.model.roomId)
                    }
                    onMarkRead: app.roomList.markRoomRead(rowLoader.model.roomId)
                    onMarkUnread: app.roomList.markRoomUnread(rowLoader.model.roomId)
                    onSetFavourite: on => app.roomList.setRoomFavourite(rowLoader.model.roomId, on)
                    onSetLowPriority: on => app.roomList.setRoomLowPriority(rowLoader.model.roomId, on)
                    onSetNotificationMode: mode => app.setRoomNotificationMode(rowLoader.model.roomId, mode)
                    onCopyRoomLink: root.roomLinkCopyRequested(rowLoader.model.roomId)
                    onLeaveRoomRequested: root.leaveRoomRequested(rowLoader.model.roomId, rowLoader.model.name)
                    onAcceptInvite: {
                        root.pendingAcceptRoomId = rowLoader.model.roomId
                        app.roomList.acceptInvite(rowLoader.model.roomId)
                    }
                    onRejectInvite: app.roomInfo.leaveRoom(rowLoader.model.roomId)
                    onChatBackgroundRequested: root.chatBackgroundRequested(rowLoader.model.roomId, rowLoader.model.name)
                    onInviteRequested: root.inviteRequested(rowLoader.model.roomId)
                    onSelectRoomRequested: root.selectAt(rowLoader.index, rowLoader.model.roomId, 0)
                }
            }

            Component {
                id: groupComponent
                ChannelCategoryHeader {
                    width: channelList.width
                    headerId: rowLoader.model.roomId
                    headerName: rowLoader.model.name
                    showsAvatar: false
                    collapsed: rowLoader.model.collapsed
                    hiddenUnread: rowLoader.model.hiddenUnread
                    hiddenHighlight: rowLoader.model.hiddenHighlight
                    onClicked: app.spaceChannels.toggleCollapsed(rowLoader.model.roomId)
                }
            }

            Component {
                id: spaceComponent
                ChannelCategoryHeader {
                    width: channelList.width
                    headerId: rowLoader.model.roomId
                    headerName: rowLoader.model.name
                    avatarUrl: rowLoader.model.avatarUrl
                    identityColorKey: rowLoader.model.identityColorKey
                    showsAvatar: true
                    collapsed: rowLoader.model.collapsed
                    hiddenUnread: rowLoader.model.hiddenUnread
                    hiddenHighlight: rowLoader.model.hiddenHighlight
                    onClicked: app.spaceChannels.toggleCollapsed(rowLoader.model.roomId)
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onTapped: app.openSpaceHome(rowLoader.model.roomId)
                    }
                }
            }
        }
    }
    RoomListAttentionLocator {
        z: 30
        anchors.fill: channelList
        list: channelList
        attentionModel: app.spaceChannels
    }

    MiddleClickScroller {
        id: middleClickScroller
        objectName: "channelsMiddleClickScroller"
        anchors.fill: parent
        z: 1
        view: channelList
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
}
