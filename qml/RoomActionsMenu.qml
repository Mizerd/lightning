import QtQuick
import QtQuick.Controls
import MatrixClient

// The room row context menu, shared by both navigation layouts; takes the
// room's fields as properties. Signal-only: the host performs every mutation,
// so a refused write cannot desynchronize a row.
AppMenu {
    id: root
    menuWidth: AppTheme.menuWidthRoom

    property string roomId: ""
    property string roomName: ""
    property string canonicalAlias: ""
    property bool isDirect: false
    property bool isFavourite: false
    property bool isLowPriority: false
    property bool isUnread: true
    property bool isInvite: false

    signal acceptInviteRequested()
    signal rejectInviteRequested()
    signal markRead()
    signal markUnread()
    signal setFavourite(bool on)
    signal setLowPriority(bool on)
    signal setNotificationMode(int mode)
    signal copyRoomLink()
    signal leaveRoomRequested()
    signal chatBackgroundRequested()
    signal inviteRequested()
    signal selectRoomRequested()

    AppMenuItem {
        objectName: "roomSelectItem"
        visible: !root.isInvite
        iconName: "check_circle"
        text: qsTr("Select room")
        onTriggered: root.selectRoomRequested()
    }

    function syncFlyoutRow() {
        for (var i = 0; i < root.count; ++i) {
            var row = root.itemAt(i)
            if (row && row.subMenu === notificationsFlyout) {
                row.visible = !root.isInvite
                row.enabled = !root.isInvite
            }
        }
    }
    onIsInviteChanged: syncFlyoutRow()
    onCountChanged: syncFlyoutRow()
    Component.onCompleted: syncFlyoutRow()
    contextLabel: root.isDirect
                  ? root.roomName
                  : (root.canonicalAlias.length > 0
                     ? root.canonicalAlias : root.roomName)
    AppMenuItem {
        objectName: "roomFavouriteItem"
        visible: app.roomList.roomFavouritesSupported
                 && !root.isInvite
        // One glyph for both states: the Material Symbols font is FILL=0, so
        // there is no filled star; the text carries the state.
        iconName: "star"
        text: root.isFavourite ? qsTr("Remove from favourites")
                               : qsTr("Add to favourites")
        onTriggered: root.setFavourite(!root.isFavourite)
    }
    AppMenuItem {
        objectName: "roomLowPriorityItem"
        visible: app.roomList.roomFavouritesSupported && !root.isInvite
        enabled: visible
        iconName: "expand_more"
        text: root.isLowPriority ? qsTr("Remove from low priority")
                                 : qsTr("Move to low priority")
        onTriggered: root.setLowPriority(!root.isLowPriority)
    }
    AppMenuSeparator {
        visible: app.roomList.roomFavouritesSupported && !root.isInvite
    }
    AppMenuItem {
        objectName: "roomMarkReadItem"
        visible: !root.isInvite && root.isUnread
        // Hidden rows must not take the keyboard (MASTER-3 M6).
        enabled: visible
        iconName: "done_all"
        text: qsTr("Mark as read")
        onTriggered: root.markRead()
    }
    AppMenuItem {
        objectName: "roomMarkUnreadItem"
        visible: !root.isInvite
        enabled: visible
        iconName: "visibility_off"
        text: qsTr("Mark as unread")
        onTriggered: root.markUnread()
    }
    AppMenuSeparator { visible: !root.isInvite }
    AppMenu {
        id: notificationsFlyout
        objectName: "roomNotificationsFlyout"
        title: qsTr("Notifications")
        submenuIconName: "notifications"
        menuWidth: AppTheme.menuWidthFlyout
        contextLabel: qsTr("Notify mode")
        contextBolt: false
        property int currentMode: 0
        property bool syncFailed: false
        function refreshMode() {
            currentMode = app.settings.roomNotificationMode(root.roomId)
            syncFailed = app.roomNotificationModeSyncFailed(root.roomId)
        }
        onAboutToShow: {
            refreshMode()
            app.requestRoomNotificationMode(root.roomId)
        }
        Connections {
            target: app.settings
            function onRoomNotificationModeChanged(roomId) {
                if (roomId === root.roomId)
                    notificationsFlyout.refreshMode()
            }
        }
        Connections {
            target: app
            function onRoomNotificationModeSyncStateChanged(roomId) {
                if (roomId === root.roomId)
                    notificationsFlyout.refreshMode()
            }
        }
        AppMenuItem {
            text: qsTr("All messages")
            radio: true
            radioSelected: notificationsFlyout.currentMode === 0
            onTriggered: root.setNotificationMode(0)
        }
        AppMenuItem {
            text: qsTr("Mentions & keywords")
            radio: true
            radioSelected: notificationsFlyout.currentMode === 1
            onTriggered: root.setNotificationMode(1)
        }
        AppMenuItem {
            text: qsTr("Muted")
            radio: true
            radioSelected: notificationsFlyout.currentMode === 2
            onTriggered: root.setNotificationMode(2)
        }
        AppMenuItem {
            visible: app.serverRoomNotificationModes
            text: qsTr("Follow account default")
            radio: true
            radioSelected: notificationsFlyout.currentMode === 3
            onTriggered: root.setNotificationMode(3)
        }
        Label {
            objectName: "roomNotificationDisclaimer"
            width: notificationsFlyout.width - notificationsFlyout.leftPadding
                   - notificationsFlyout.rightPadding
            leftPadding: AppTheme.menuItemPadding
            rightPadding: AppTheme.menuItemPadding
            topPadding: AppTheme.spacing4
            bottomPadding: AppTheme.spacing6
            text: app.serverRoomNotificationModes
                  ? (notificationsFlyout.syncFailed
                     ? qsTr("Couldn't save to the server — "
                            + "kept on this device. "
                            + "Retried when you reconnect.")
                     : (notificationsFlyout.currentMode === 3
                        ? qsTr("This room has no override — your "
                               + "account's notification settings "
                               + "apply.")
                        : qsTr("Saved to your account's notification "
                               + "settings (server push rules).")))
                  : qsTr("Local setting: it does not change this "
                         + "room's server push rules.")
            color: AppTheme.stormTextFaint
            font.pixelSize: AppTheme.fontMicro
            wrapMode: Text.WordWrap
        }
    }
    AppMenuSeparator { visible: !root.isInvite }
    AppMenuItem {
        objectName: "roomChatBackgroundItem"
        visible: !!app.backdrops && !root.isInvite
        enabled: visible
        iconName: "image"
        text: qsTr("Chat background…")
        onTriggered: root.chatBackgroundRequested()
    }
    AppMenuItem {
        objectName: "roomInviteItem"
        readonly property bool openRoom: !!app.roomInfo
                                         && app.roomInfo.roomId === root.roomId
        visible: !root.isInvite && !root.isDirect
                 && (!openRoom || app.roomInfo.canInvite)
        enabled: visible
        iconName: "person_add"
        text: qsTr("Invite people…")
        onTriggered: root.inviteRequested()
    }
    AppMenuItem {
        objectName: "roomCopyLinkItem"
        visible: !root.isInvite
        enabled: visible
        iconName: "link"
        text: qsTr("Copy room link")
        onTriggered: root.copyRoomLink()
    }
    AppMenuItem {
        objectName: "roomLeaveItem"
        visible: !root.isInvite
        enabled: visible
        iconName: "logout"
        text: qsTr("Leave room")
        danger: true
        onTriggered: root.leaveRoomRequested()
    }
    AppMenuItem {
        objectName: "roomAcceptInviteItem"
        visible: root.isInvite
        enabled: visible
        iconName: "person_add"
        text: qsTr("Accept invitation")
        onTriggered: root.acceptInviteRequested()
    }
    AppMenuItem {
        objectName: "roomRejectInviteItem"
        visible: root.isInvite
        enabled: visible
        iconName: "close"
        text: qsTr("Reject invitation")
        danger: true
        onTriggered: root.rejectInviteRequested()
    }
}
