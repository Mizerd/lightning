import QtQuick
import QtQuick.Controls
import MatrixClient

// The room row context menu, shared by both navigation layouts; takes the
// room's fields as properties. Signal-only: the host performs every mutation,
// so a refused write cannot desynchronize a row.
AppMenu {
    id: root
    menuWidth: AppTheme.menuWidthRoom

    /// The room this menu acts on, and the fields its rows read.
    property string roomId: ""
    property string roomName: ""
    property string canonicalAlias: ""
    property bool isDirect: false
    property bool isFavourite: false

    signal markRead()
    signal markUnread()
    // Carries the value to write, not a toggle.
    signal setFavourite(bool on)
    signal setNotificationMode(int mode)
    signal copyRoomLink()
    signal leaveRoomRequested()
    // Mono room-address header: the canonical alias when there is one,
    // otherwise the display name without a fabricated "#".
    contextLabel: root.isDirect
                  ? root.roomName
                  : (root.canonicalAlias.length > 0
                     ? root.canonicalAlias : root.roomName)
    // Favourite at the top, showing the current state. Offered only where the
    // backend can write the tag (a device-local favourite would differ from
    // other clients).
    AppMenuItem {
        objectName: "roomFavouriteItem"
        visible: app.roomList.roomFavouritesSupported
        // One glyph for both states: the Material Symbols font is FILL=0, so
        // there is no filled star; the text carries the state.
        iconName: "star"
        text: root.isFavourite ? qsTr("Remove from favourites")
                               : qsTr("Add to favourites")
        onTriggered: root.setFavourite(!root.isFavourite)
    }
    AppMenuSeparator { visible: app.roomList.roomFavouritesSupported }
    AppMenuItem {
        iconName: "check"
        text: qsTr("Mark as read")
        onTriggered: root.markRead()
    }
    AppMenuItem {
        iconName: "visibility_off"
        text: qsTr("Mark as unread")
        onTriggered: root.markUnread()
    }
    AppMenuSeparator {}
    // Per-room notification mode. roomNotificationMode is a Q_INVOKABLE, so it
    // is re-queried (refreshMode()) rather than bound; radioSelected binds to
    // the local currentMode and is never assigned. With server push rules the
    // setting also caches the account's server mode (see AppController).
    AppMenu {
        id: notificationsFlyout
        objectName: "roomNotificationsFlyout"
        title: qsTr("Notifications")
        submenuIconName: "notifications"
        menuWidth: AppTheme.menuWidthFlyout
        // Flyout header: a bare mono caption.
        contextLabel: qsTr("Notify mode")
        contextBolt: false
        property int currentMode: 0
        // True while the last server push-rule write failed; the disclaimer
        // then says the mode was kept on this device.
        property bool syncFailed: false
        function refreshMode() {
            currentMode = app.settings.roomNotificationMode(root.roomId)
            syncFailed = app.roomNotificationModeSyncFailed(root.roomId)
        }
        onAboutToShow: {
            refreshMode()
            // Re-query the server rule on open so changes from other clients
            // land. A no-op without server push-rule support.
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
            // The SDK's MentionsAndKeywordsOnly mode keeps keyword rules
            // firing.
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
        // "Follow account default", as in Room Information, so mode 3 has a
        // selected radio. Server-capable backends only.
        AppMenuItem {
            visible: app.serverRoomNotificationModes
            text: qsTr("Follow account default")
            radio: true
            radioSelected: notificationsFlyout.currentMode === 3
            onTriggered: root.setNotificationMode(3)
        }
        Label {
            objectName: "roomNotificationDisclaimer"
            // Sized to the menu so the text wraps (an unsized wrapping Text
            // uses its implicit width, the whole sentence) and its height
            // counts toward the menu's. Not a row, so it does not affect
            // AppMenu's fit and cannot loop.
            width: notificationsFlyout.width - notificationsFlyout.leftPadding
                   - notificationsFlyout.rightPadding
            leftPadding: AppTheme.menuItemPadding
            rightPadding: AppTheme.menuItemPadding
            topPadding: AppTheme.spacing4
            bottomPadding: AppTheme.spacing6
            // The Rust backend writes the account's server push rules ("saved",
            // not live-synced); a failed write is admitted, with a retry on the
            // next reconnection. Other backends are device-local.
            text: app.serverRoomNotificationModes
                  ? (notificationsFlyout.syncFailed
                     ? qsTr("Couldn't save to the server — "
                            + "kept on this device. "
                            + "Retried when you reconnect.")
                     : qsTr("Saved to your account's notification "
                            + "settings (server push rules)."))
                  : qsTr("Local setting: it does not change this "
                         + "room's server push rules.")
            color: AppTheme.stormTextFaint
            font.pixelSize: AppTheme.fontMicro
            wrapMode: Text.WordWrap
        }
    }
    AppMenuSeparator {}
    AppMenuItem {
        iconName: "link"
        text: qsTr("Copy room link")
        onTriggered: root.copyRoomLink()
    }
    AppMenuItem {
        iconName: "logout"
        text: qsTr("Leave room")
        danger: true
        onTriggered: root.leaveRoomRequested()
    }
}
