import QtQuick
import QtQuick.Controls
import MatrixClient

// The room row context menu, shared by BOTH navigation layouts.
//
// Extracted from RoomDelegate.qml, verbatim apart from taking the room's
// fields as properties instead of reading a `model` that only a Classic row
// has. The Channels layout had NO menu at all, so favourite / mark read /
// notifications / copy link / leave were unreachable in it — reported as
// "favorite, mute and all the other actions are unavailable". Duplicating
// 160 lines into a second delegate is how two menus drift; this is one menu
// with two hosts.
//
// Signal-only, exactly as before: nothing here writes a setting or calls the
// bridge. The host performs every mutation, so a refused server write cannot
// leave a row and the account disagreeing.
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
    // Carries the value to WRITE, not a toggle request.
    signal setFavourite(bool on)
    signal setNotificationMode(int mode)
    signal copyRoomLink()
    signal leaveRoomRequested()
    // Storm §4 2b: mono room-address header. Mono is for ADDRESSES —
    // use the canonical alias when the room has one; otherwise the
    // display name WITHOUT a fabricated # prefix.
    contextLabel: root.isDirect
                  ? root.roomName
                  : (root.canonicalAlias.length > 0
                     ? root.canonicalAlias : root.roomName)
    // Element classic puts Favourite at the top of the room menu, as a
    // toggle showing the CURRENT state. Offered only where the backend
    // can write the tag: a device-local "favourite" would mean something
    // different here than on every other client on the account.
    AppMenuItem {
        objectName: "roomFavouriteItem"
        visible: app.roomList.roomFavouritesSupported
        // One glyph for both states: the icon font is Material Symbols
        // at FILL=0, so "star" is already the OUTLINE and there is no
        // filled counterpart to switch to. The row's text carries the
        // state, which is how the read/unread rows below do it too.
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
    // v0.6.5 (SPEC 1d): per-room notification mode, three radio rows
    // bound to the REAL setting (SettingsManager::roomNotification-
    // Mode is Q_INVOKABLE, not a property, so it is re-queried explicitly
    // rather than bound directly — see refreshMode() below). radioSelected
    // stays a pure binding on the local currentMode property; it is never
    // imperatively assigned (AppMenuItem itself never self-toggles it).
    // On backends with server push-rule support the setting doubles as
    // the cache of the account's server mode (see AppController).
    AppMenu {
        id: notificationsFlyout
        objectName: "roomNotificationsFlyout"
        title: qsTr("Notifications")
        submenuIconName: "notifications"
        menuWidth: AppTheme.menuWidthFlyout
        // Storm §4 2b: flyout header is a bare mono caption, no bolt.
        contextLabel: qsTr("Notify mode")
        contextBolt: false
        property int currentMode: 0
        // True while the room's last server push-rule write failed —
        // the disclaimer then says the mode was kept on this device
        // instead of claiming it was saved to the account.
        property bool syncFailed: false
        function refreshMode() {
            currentMode = app.settings.roomNotificationMode(root.roomId)
            syncFailed = app.roomNotificationModeSyncFailed(root.roomId)
        }
        onAboutToShow: {
            refreshMode()
            // Poll-on-open: re-query the server rule so a change made
            // in another client lands in the cache (and, via the
            // Connections below, in this flyout). A guarded no-op on
            // backends without server push-rule support.
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
            // "& keywords" is what the rule actually does: the SDK's
            // MentionsAndKeywordsOnly mode keeps keyword rules firing.
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
        // v0.7: the same "follow account default" choice Room
        // Information offers. Without it a room set to mode 3 shows NO
        // selected radio here — two entry points to one setting
        // disagreeing, with this one rendering a state it cannot
        // express. Server-capable backends only: with a device-local
        // backend there is no account rule to defer to.
        AppMenuItem {
            visible: app.serverRoomNotificationModes
            text: qsTr("Follow account default")
            radio: true
            radioSelected: notificationsFlyout.currentMode === 3
            onTriggered: root.setNotificationMode(3)
        }
        Label {
            objectName: "roomNotificationDisclaimer"
            leftPadding: AppTheme.menuItemPadding
            rightPadding: AppTheme.menuItemPadding
            topPadding: AppTheme.spacing4
            bottomPadding: AppTheme.spacing6
            // Backend-honest: the Rust backend writes the account's
            // server push rules through the SDK ("saved", not
            // continuously synced — there is no live push-rule watcher
            // yet); a failed write is admitted instead of claimed
            // saved; other backends keep the mode strictly
            // device-local.
            // v0.7: a failed write is now retried on the next
            // reconnection, so the failure line says so. It still
            // admits the failure first — the retry is a promise to try
            // again, never a claim that the rule was saved.
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
