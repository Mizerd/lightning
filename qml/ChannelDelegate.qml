import QtQuick
import QtQuick.Controls
import MatrixClient

// One channel row in the Channels navigation layout: the room's avatar and
// name, nothing else at rest. No preview or timestamp, so the list stays
// scannable and still (Classic is where previews live). A small badge on the
// avatar marks the two things a picture can't: an invite, or encryption.
//
// Weight is the unread signal (`channelText` vs `channelTextUnread`); the only
// count pill is for unread/mentions. Nothing moves when a message arrives.
//
// Per-row delegate: every Label whose text can be empty lives behind a Loader.
// An empty, never laid-out Text keeps ItemObservesViewport and makes Qt walk
// the whole tree on every scroll frame.
ItemDelegate {
    id: root

    property string roomId: ""
    property string channelName: ""
    /// The room's avatar (mxc uri); empty until it resolves, when Avatar shows
    /// palette initials like every other room surface.
    property string avatarUrl: ""
    /// Colour key for that fallback, matching the room list's policy.
    property string identityColorKey: ""
    property bool isDirect: false
    /// An invited, unjoined room. It gets its own glyph and always reads as
    /// unread, since it's waiting on the user.
    property bool isInvite: false
    property bool encrypted: false
    property int unreadCount: 0
    property int highlightCount: 0
    property bool hasUnread: false
    property bool active: false
    /// Indentation level: 0 at the top of the column, 1 inside a folder.
    property int depth: 0
    /// Favourite flag, for the context menu toggle and the star.
    property bool isFavourite: false

    // The same actions as the Classic row. Signal-only: the presenter performs
    // every mutation, as with RoomDelegate.
    signal markRead()
    signal markUnread()
    signal setFavourite(bool on)
    signal setNotificationMode(int mode)
    signal copyRoomLink()
    signal leaveRoomRequested()

    // roomNotificationMode is Q_INVOKABLE, not bindable, so it's re-queried
    // when the id changes (delegate reuse) and when settings announce a write,
    // as in RoomDelegate.
    property int notificationMode: 0
    readonly property bool muted: notificationMode === 2
    function refreshNotificationMode() {
        // Guarded: a delegate created synchronously from a property-change
        // handler can see `app` undefined on its first lookup.
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

    // A muted channel keeps its unread weight but loses its pill. Plain unread
    // gets a pill too, not only mentions; the count can be 0 on an unread room
    // (Matrix computes notification_count only where push rules say to), so
    // the pill falls back to a dot.
    readonly property bool showsPill:
        (root.highlightCount > 0 || root.unreadCount > 0
         || root.hasUnread || root.isInvite) && !root.muted
    readonly property bool readsUnread: (root.isInvite || root.hasUnread || root.unreadCount > 0 || root.highlightCount > 0)

    // Loaded by the Channels presenter (five row kinds). The Loader takes its
    // height from this explicit value, so rows stack instead of all sitting at
    // y=0.
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
        // Inset from the column edges so it reads as a list, not full-width
        // bands.
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

        // The unread bar at the left edge, except on the active row, which is
        // already marked.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            x: 2 + root.depth * 10
            // Wide enough to notice: surfacing unread is this column's job.
            width: 4
            height: 20
            radius: 2
            color: AppTheme.channelUnreadMark
            visible: root.readsUnread && !root.active && !root.muted
        }

        // A DM is a circle and a room a rounded square, as in the Classic row.
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

        // A small ringed badge on the avatar's corner: invite or lock. The lock
        // is a claim that must be visible wherever the room appears.
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

        // Behind a Loader: the name is empty until the room's state resolves.
        Loader {
            active: root.channelName.length > 0
            anchors.left: roomAvatar.right
            anchors.leftMargin: 8
            anchors.right: favouriteStar.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                text: root.channelName
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pixelSize: AppTheme.textBody
                font.weight: root.readsUnread && !root.muted ? AppTheme.weightMedium : AppTheme.weightBody
                color: root.active ? AppTheme.channelSelectedText : (root.muted ? AppTheme.channelCategoryText : (root.readsUnread ? AppTheme.channelTextUnread : AppTheme.channelText))
            }
        }

        // Favourite star: filled when favourited, an outline on hover, and a
        // click toggles the same m.favourite tag as the menu. No width when
        // neither, so the unread pill never moves. Drawn rather than a glyph
        // (the icon subset has no filled star).
        //
        // It anchors to the mute glyph when that is shown, not only to the call
        // glyph: both sit 14px from the right edge, so anchoring to one alone
        // drew the star through the other.
        Loader {
            id: favouriteStar
            objectName: "channelFavouriteStar"
            readonly property bool shown:
                app.roomList.roomFavouritesSupported === true && !root.isInvite
                && (root.isFavourite || root.hovered)
            active: shown
            visible: active
            width: active ? 16 : 0
            height: 16
            anchors.right: mutedGlyph.active ? mutedGlyph.left : callGlyph.left
            anchors.rightMargin:
                (mutedGlyph.active || callGlyph.width > 0) ? 6 : 0
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Item {
                Canvas {
                    id: starCanvas
                    anchors.fill: parent
                    antialiasing: true
                    readonly property bool filled: root.isFavourite
                    readonly property color ink: root.isFavourite
                        ? AppTheme.accent
                        : (root.active ? AppTheme.channelSelectedText
                                       : AppTheme.channelCategoryText)
                    onFilledChanged: requestPaint()
                    onInkChanged: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset()
                        var cx = width / 2, cy = height / 2
                        var outer = width / 2 - 1.5, inner = outer * 0.47
                        ctx.beginPath()
                        for (var i = 0; i < 10; ++i) {
                            var r = (i % 2 === 0) ? outer : inner
                            var a = -Math.PI / 2 + i * Math.PI / 5
                            var x = cx + r * Math.cos(a), y = cy + r * Math.sin(a)
                            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                        }
                        ctx.closePath()
                        ctx.lineJoin = "round"
                        if (filled) {
                            ctx.fillStyle = ink
                            ctx.fill()
                        } else {
                            ctx.lineWidth = 1.4
                            ctx.strokeStyle = ink
                            ctx.stroke()
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.setFavourite(!root.isFavourite)
                }
                Accessible.role: Accessible.Button
                Accessible.name: root.isFavourite ? qsTr("Remove from favourites")
                                                  : qsTr("Add to favourites")
            }
        }

        // Call indicator, between the name and the unread pill; zero width when
        // there's no call.
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
                // Mentions win the colour and number; an unread room with no
                // count shows a dot.
                count: root.highlightCount > 0 ? root.highlightCount
                                               : root.unreadCount
                mention: root.highlightCount > 0
                // Unread with no honest number falls back to the dot.
                dot: root.hasUnread || root.isInvite
            }
        }

        // Mute bell, only when muted and nothing else holds the rightmost slot.
        // Explicit zero width when inactive so the star doesn't inherit a stale
        // offset.
        Loader {
            id: mutedGlyph
            objectName: "channelMutedGlyph"
            active: root.muted && !root.showsPill && !callGlyph.live
            visible: active
            width: active ? 14 : 0
            height: 14
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

    // Right-click and the Menu key open the same actions as the Classic row.
    // The menu is loaded on first use, not declared inline: building a Popup
    // with a submenu per 32px row made filter switches (which rebuild every
    // delegate) laggy.
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
