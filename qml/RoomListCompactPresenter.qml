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
    readonly property bool channels: app.settings.roomNavigationLayout === 1

    ListView {
        id: classicList
        objectName: "compactClassicList"
        anchors.fill: parent
        visible: !root.channels
        model: visible ? app.roomList : null
        clip: true
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
        }
    }

    ListView {
        id: channelsList
        objectName: "compactChannelsList"
        anchors.fill: parent
        visible: root.channels
        model: visible ? app.spaceChannels : null
        clip: true
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
