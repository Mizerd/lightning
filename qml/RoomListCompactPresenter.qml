import QtQuick
import QtQuick.Controls
import MatrixClient

Item {
    id: root
    property int notificationRevision: 0
    Connections {
        target: app.settings
        function onRoomNotificationModeChanged() { root.notificationRevision++ }
    }
    signal roomActivated(string roomId)
    signal lobbyActivated()
    signal messageSearchRequested()
    signal actionRequested(string actionId)
    signal expandRequested()
    signal roomLinkCopyRequested(string roomId)
    signal leaveRoomRequested(string roomId, string roomName)
    signal chatBackgroundRequested(string roomId, string roomName)
    signal inviteRequested(string roomId)

    function openRoomMenu(row, unread) {
        roomMenu.roomId = row.roomId
        roomMenu.roomName = row.name || ""
        roomMenu.canonicalAlias = row.canonicalAlias || ""
        roomMenu.isDirect = row.isDirect === true
        roomMenu.isFavourite = row.isFavourite === true
        roomMenu.isLowPriority = row.isLowPriority === true
        roomMenu.isUnread = unread
        roomMenu.isInvite = row.membership === "invited" || row.isInvite === true
        roomMenu.popup()
    }
    RoomActionsMenu {
        id: roomMenu
        objectName: "compactRoomContextMenu"
        selectable: false
        onAcceptInviteRequested: app.roomList.acceptInvite(roomId)
        onRejectInviteRequested: app.roomList.rejectInvite(roomId)
        onMarkRead: app.roomList.markRoomRead(roomId)
        onMarkUnread: app.roomList.markRoomUnread(roomId)
        onSetFavourite: on => app.roomList.setRoomFavourite(roomId, on)
        onSetLowPriority: on => app.roomList.setRoomLowPriority(roomId, on)
        onSetNotificationMode: mode => app.setRoomNotificationMode(roomId, mode)
        onCopyRoomLink: root.roomLinkCopyRequested(roomId)
        onLeaveRoomRequested: root.leaveRoomRequested(roomId, roomName)
        onChatBackgroundRequested: root.chatBackgroundRequested(roomId, roomName)
        onInviteRequested: root.inviteRequested(roomId)
    }
    readonly property bool channels: app.settings.roomNavigationLayout === 1

    ListView {
        id: classicList
        objectName: "compactClassicList"
        anchors.fill: parent
        visible: !root.channels
        model: visible ? app.roomList : null
        clip: true
        Connections {
            target: classicList.visible ? app : null
            // Again once the rows it jumped past have their real heights.
            function onRoomCycled(row) {
                classicList.positionViewAtIndex(row, ListView.Contain)
                Qt.callLater(classicList.positionViewAtIndex, row, ListView.Contain)
            }
        }
        currentIndex: -1
        keyNavigationEnabled: true
        reuseItems: true
        delegate: RoomListCompactRow {
            label: model.name || ""
            rowId: model.roomId || ""
            avatarUrl: model.membership === "invited"
                       && !(app.accountPrefs && app.accountPrefs.inviteAvatarsAllowed)
                       ? "" : (model.avatarUrl || "")
            identityColorKey: model.identityColorKey || ""
            direct: model.isDirect === true
            selected: model.roomId === app.currentRoomId
            unreadCount: model.unreadCount || 0
            highlightCount: model.highlightCount || 0
            hasUnread: model.hasUnread || model.markedUnread
                       || model.membership === "invited"
            threadUnreadKind: model.threadUnreadKind || 0
            muted: {
                void root.notificationRevision
                return app.settings.roomNotificationMode(model.roomId) === 2
            }
            onClicked: {
                if (model.membership === "joined") root.roomActivated(model.roomId)
                else root.expandRequested()
            }
            onContextMenuRequested: root.openRoomMenu(model, model.rawUnread === true)
        }
    }

    ListView {
        id: channelsList
        objectName: "compactChannelsList"
        anchors.fill: parent
        visible: root.channels
        model: visible ? app.spaceChannels : null
        clip: true
        Connections {
            target: channelsList.visible ? app : null
            // Again once the rows it jumped past have their real heights.
            function onRoomCycled(row) {
                channelsList.positionViewAtIndex(row, ListView.Contain)
                Qt.callLater(channelsList.positionViewAtIndex, row, ListView.Contain)
            }
        }
        currentIndex: -1
        keyNavigationEnabled: true
        reuseItems: true
        delegate: RoomListCompactRow {
            readonly property string kind: model.kind || ""
            label: model.name || ""
            rowId: model.roomId || ""
            avatarUrl: model.avatarUrl || ""
            identityColorKey: model.identityColorKey || ""
            direct: model.isDirect === true
            selected: kind === "room" && model.roomId === app.currentRoomId
            folder: kind === "space" || kind === "group"
            folderCollapsed: model.collapsed === true
            iconName: kind === "group" ? "folder"
                      : kind === "space" || kind === "room" ? ""
                      : model.iconName || ""
            unreadCount: folder ? (model.hiddenUnread || 0) : (model.unreadCount || 0)
            highlightCount: folder ? (model.hiddenHighlight || 0) : (model.highlightCount || 0)
            hasUnread: model.hasUnread || model.isInvite
            threadUnreadKind: model.threadUnreadKind || 0
            muted: {
                void root.notificationRevision
                return kind === "room"
                       && app.settings.roomNotificationMode(model.roomId) === 2
            }
            onContextMenuRequested: if (kind === "room")
                root.openRoomMenu(model, model.rawUnread === true || model.hasUnread
                                  || model.unreadCount > 0 || model.highlightCount > 0)
            onHoveredChanged: if (kind === "room")
                app.spaceChannels.setRoomHovered(model.roomId, hovered)
            onClicked: {
                if (kind === "room") {
                    if (model.isInvite) root.expandRequested()
                    else root.roomActivated(model.roomId)
                }
                else if (folder) app.spaceChannels.toggleCollapsed(model.roomId)
                else if (kind === "lobby") root.lobbyActivated()
                else if (kind === "search") root.messageSearchRequested()
                else if (kind === "action") root.actionRequested(model.roomId)
            }
        }
    }
}
