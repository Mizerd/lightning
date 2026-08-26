import QtQuick
import QtQuick.Controls
import MatrixClient

// One channel row in the Channels navigation layout.
//
// Sable-first, as directed: the room's AVATAR, its name, and nothing else at
// rest. No message preview and no timestamp — the whole point of a channel
// list is that it is scannable and stays put, and a preview line triples the
// row height while changing on every message. Classic is where previews live;
// the two layouts are different answers, not one with knobs.
//
// The avatar is not decoration and it is not optional: Sable's own column
// shows one per room, and the first revision of this row drew a hash glyph
// instead, which made every room in a Space look identical. The glyph still
// appears — as a small badge over the avatar — for the two things it genuinely
// says that a picture cannot: this room is a DM, or this room is encrypted.
//
// Weight is the unread signal. A read channel sits at `channelText`, an
// unread one at `channelTextUnread` with a medium weight, and a mention adds
// the only count pill in the layout. Nothing here moves when a message
// arrives.
//
// Delegate discipline (this is instantiated per row): every Label whose text
// can legitimately be empty lives behind a Loader. A never-laid-out empty
// Text keeps ItemObservesViewport forever and makes Qt walk the whole
// instantiated tree on every scroll frame.
ItemDelegate {
    id: root

    property string roomId: ""
    property string channelName: ""
    /// The room's own avatar (an mxc uri), empty until it resolves — the
    /// shared Avatar element then renders palette initials, which is the same
    /// fallback every other room surface uses.
    property string avatarUrl: ""
    /// Colour key for that fallback, matching the room list's policy.
    property string identityColorKey: ""
    property bool isDirect: false
    /// A room the account has been invited to but not joined. It gets its own
    /// glyph and always reads as unread: an invite is an action waiting on the
    /// user, and drawing it like a quiet read room is how one gets missed.
    property bool isInvite: false
    property bool encrypted: false
    property int unreadCount: 0
    property int highlightCount: 0
    property bool hasUnread: false
    property bool active: false
    /// Indentation level: 0 at the top of the column, 1 inside a folder.
    property int depth: 0
    /// Element-parity favourite flag, for the context menu's toggle. The row
    /// draws nothing from it — a channel list has no star column.
    property bool isFavourite: false

    // The same actions the Classic row offers. Signal-only: the presenter
    // performs every mutation, exactly as RoomDelegate's host does.
    signal markRead()
    signal markUnread()
    signal setFavourite(bool on)
    signal setNotificationMode(int mode)
    signal copyRoomLink()
    signal leaveRoomRequested()

    // SettingsManager::roomNotificationMode is Q_INVOKABLE, not a property,
    // so it cannot be bound. Re-queried on the two events that can change
    // the answer for THIS row — the id changing under delegate reuse, and
    // the settings manager announcing a write — exactly as RoomDelegate does.
    property int notificationMode: 0
    readonly property bool muted: notificationMode === 2
    function refreshNotificationMode() {
        // Guarded like Avatar/PresenceDot's bridge lookups: a delegate created
        // synchronously from inside a property-change handler can see `app`
        // undefined on its first context lookup, and an unmuted-by-accident
        // row is a lie about a setting the user changed on purpose.
        if (typeof app === "undefined" || !app || !app.settings)
            return;
        if (root.roomId.length === 0)
            return;
        root.notificationMode = app.settings.roomNotificationMode(root.roomId);
    }
    onRoomIdChanged: root.refreshNotificationMode()
    Component.onCompleted: root.refreshNotificationMode()
    Connections {
        target: (typeof app !== "undefined" && app) ? app.settings : null
        function onRoomNotificationModeChanged() {
            root.refreshNotificationMode();
        }
    }

    // A muted channel keeps its unread WEIGHT but loses its pill: the user
    // asked not to be counted at, not to be lied to about whether anything
    // happened.
    readonly property bool showsPill: root.highlightCount > 0 && !root.muted
    readonly property bool readsUnread: (root.isInvite || root.hasUnread || root.unreadCount > 0 || root.highlightCount > 0)

    // A Loader-hosted row: the Channels presenter picks between five row
    // kinds, so this is loaded rather than declared inline. The Loader takes
    // its height from this value (measured, Qt 6.11: loader implicitHeight
    // 32 for a Control declaring height 32), which is what makes the rows lay
    // out one below another instead of stacking at y=0.
    height: 32
    padding: 0
    hoverEnabled: true

    Accessible.role: Accessible.Button
    Accessible.name: {
        var base = root.channelName;
        if (root.isInvite)
            return qsTr("%1, invitation").arg(base);
        if (root.muted)
            base = qsTr("%1, muted").arg(base);
        else if (root.highlightCount > 0)
            base = qsTr("%1, %2 mentions").arg(base).arg(root.highlightCount);
        else if (root.readsUnread)
            base = qsTr("%1, unread").arg(base);
        return base;
    }

    background: Rectangle {
        radius: AppTheme.radiusSm
        // 8px inset on each side so the pill does not touch the column edge,
        // which is what makes a channel list read as a list rather than as
        // full-width bands.
        anchors.fill: parent
        anchors.leftMargin: 8 + root.depth * 10
        anchors.rightMargin: 8
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        color: root.active ? AppTheme.channelSelected : (root.hovered || root.activeFocus ? AppTheme.channelHover : "transparent")
        border.width: root.activeFocus ? 2 : 0
        border.color: AppTheme.focusRing
        Behavior on color {
            ColorAnimation {
                duration: 90
            }
        }
    }

    contentItem: Item {
        anchors.fill: parent

        // The unread rail: a short bar at the left edge, Sable's cue. Only
        // for a channel that is NOT the active one — the active row already
        // says where you are, and two markers on one row read as a bug.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            x: 2 + root.depth * 10
            width: 3
            height: 16
            radius: 1.5
            color: AppTheme.channelUnreadMark
            visible: root.readsUnread && !root.active && !root.muted
        }

        // The room's picture. A DM gets a circle and a room a rounded square,
        // exactly as the Classic row does, so the same room does not change
        // shape between layouts.
        Avatar {
            id: roomAvatar
            anchors.verticalCenter: parent.verticalCenter
            x: 12 + root.depth * 10
            width: 20
            height: 20
            size: 20
            circle: root.isDirect
            squareRadius: 6
            labelSize: 9
            name: root.channelName
            colorKey: root.identityColorKey.length > 0 ? root.identityColorKey : root.roomId
            mxc: root.avatarUrl
        }

        // The two things a picture cannot say. Drawn as a small ringed badge
        // on the avatar's corner rather than in place of it — an invite needs
        // to be identifiable at a glance, and a lock is a CLAIM that has to be
        // visible wherever the room appears.
        Loader {
            active: root.isInvite || root.encrypted
            visible: active
            anchors.right: roomAvatar.right
            anchors.bottom: roomAvatar.bottom
            anchors.rightMargin: -3
            anchors.bottomMargin: -3
            sourceComponent: Rectangle {
                width: 12
                height: 12
                radius: 6
                color: AppTheme.sidebar
                Icon {
                    anchors.centerIn: parent
                    name: root.isInvite ? "person_add" : "lock"
                    size: 9
                    color: root.isInvite ? AppTheme.accent : AppTheme.channelCategoryText
                }
            }
        }

        // Behind a Loader: the name is empty for a room whose state has not
        // resolved yet, which IS the state this delegate is created in.
        Loader {
            active: root.channelName.length > 0
            anchors.left: roomAvatar.right
            anchors.leftMargin: 8
            anchors.right: callGlyph.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                text: root.channelName
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pixelSize: AppTheme.textBody
                font.weight: root.readsUnread && !root.muted ? AppTheme.weightMedium : AppTheme.weightBody
                color: root.active ? AppTheme.channelSelectedText : (root.muted ? AppTheme.channelCategoryText : (root.readsUnread ? AppTheme.channelTextUnread : AppTheme.channelText))
            }
        }

        // "There is a call in this room", between the name and the unread
        // pill. It collapses to zero width when there is no call, so a row
        // without one is pixel-identical to what it was.
        RoomCallGlyph {
            id: callGlyph
            roomId: root.roomId
            glyphSize: 14
            color: root.active ? AppTheme.channelSelectedText
                               : AppTheme.channelCategoryText
            anchors.right: pillLoader.active ? pillLoader.left : parent.right
            anchors.rightMargin: pillLoader.active ? 6 : 14
            anchors.verticalCenter: parent.verticalCenter
        }

        Loader {
            id: pillLoader
            active: root.showsPill
            visible: active
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: UnreadBadge {
                count: root.highlightCount
                mention: true
            }
        }

        // Only when muted, so it does not become permanent furniture.
        Loader {
            active: root.muted && !root.showsPill && !callGlyph.live
            visible: active
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Icon {
                name: "notifications_off"
                size: 14
                color: AppTheme.channelCategoryText
            }
        }
    }

    // Right-click and the Menu key, matching the Classic row. Without this
    // the whole action set — favourite, mark read/unread, notification mode,
    // copy link, leave — was unreachable in this layout.
    //
    // Behind a Loader, and that is a PERFORMANCE contract, not tidiness. The
    // menu is a Popup with a submenu and ten items; declaring it inline built
    // all of that for EVERY row, and a channel row is 32px so a screen holds
    // three times as many rows as the Classic list plus a cache buffer. The
    // first version of this shipped the menu inline and switching the filter
    // — which resets the model and rebuilds every delegate — went from
    // instant to visibly laggy. The menu is created by the first right-click
    // on the row and kept after that.
    function openContextMenu() {
        if (root.roomId.length === 0)
            return;
        menuLoader.active = true;
        if (menuLoader.item)
            menuLoader.item.popup();
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: root.roomId.length > 0
        onTapped: root.openContextMenu()
    }
    Keys.onPressed: event => {
        if (root.roomId.length > 0
            && (event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10
                    && (event.modifiers & Qt.ShiftModifier)))) {
            root.openContextMenu();
            event.accepted = true;
        }
    }
    Loader {
        id: menuLoader
        objectName: "channelContextMenuLoader"
        active: false
        sourceComponent: RoomActionsMenu {
            objectName: "channelContextMenu"
            roomId: root.roomId
            roomName: root.channelName
            isDirect: root.isDirect
            isFavourite: root.isFavourite
            onMarkRead: root.markRead()
            onMarkUnread: root.markUnread()
            onSetFavourite: on => root.setFavourite(on)
            onSetNotificationMode: mode => root.setNotificationMode(mode)
            onCopyRoomLink: root.copyRoomLink()
            onLeaveRoomRequested: root.leaveRoomRequested()
        }
    }
}
