import QtQuick
import QtQuick.Controls
import MatrixClient

ItemDelegate {
    id: root

    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onEnterPressed: root.clicked()

    property string roomId: ""
    property string channelName: ""
    property string avatarUrl: ""
    property string identityColorKey: ""
    property bool isDirect: false
    property bool isInvite: false
    // An invitation's picture is a stranger's choice: it loads only when the
    // account allows it (MSC4278 invite_avatars, fail-closed until known).
    // A joined room always shows its own picture.
    readonly property bool inviteAvatarWithheld:
        root.isInvite
        && !(typeof app !== "undefined" && app && app.accountPrefs
             && app.accountPrefs.inviteAvatarsAllowed)
    property bool encrypted: false
    property int unreadCount: 0
    property int highlightCount: 0
    property int threadUnreadKind: 0
    readonly property bool threadOnly: threadUnreadKind > 0 && !isInvite && !muted
    property bool hasUnread: false
    property bool rawUnread: false
    property bool active: false
    property bool batchSelected: false
    property int depth: 0
    property bool isFavourite: false
    property bool isLowPriority: false

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
    signal acceptInvite()
    signal rejectInvite()

    property int notificationMode: 0
    readonly property bool muted: notificationMode === 2
    function refreshNotificationMode() {
        if (typeof app === "undefined" || !app || !app.settings)
            return;
        if (root.roomId.length === 0)
            return;
        root.notificationMode = app.settings.roomNotificationMode(root.roomId);
    }
    onRoomIdChanged: {
        root.refreshNotificationMode()
        if (menuLoader.item)
            menuLoader.item.close()
    }
    Component.onCompleted: root.refreshNotificationMode()
    Connections {
        target: (typeof app !== "undefined" && app) ? app.settings : null
        function onRoomNotificationModeChanged() {
            root.refreshNotificationMode();
        }
    }

    readonly property bool showsPill:
        (root.highlightCount > 0 || root.unreadCount > 0
         || root.hasUnread || root.isInvite || root.threadOnly) && !root.muted
    readonly property bool readsUnread: (root.isInvite || root.hasUnread || root.unreadCount > 0 || root.highlightCount > 0)

    height: Math.max(AppTheme.channelRowMinHeight,
                    2 * AppTheme.scaled(AppTheme.textBody))
    padding: 0
    hoverEnabled: true

    Accessible.role: Accessible.Button
    Accessible.name: {
        var base = root.channelName;
        if (root.isInvite)
            return qsTr("%1, invitation").arg(base);
        if (root.muted)
            base = qsTr("%1, muted").arg(base);
        else if (root.threadOnly)
            base = (root.threadUnreadKind === 2
                    ? qsTr("%1, mention in a thread", "channel row")
                    : qsTr("%1, unread in a thread", "channel row")).arg(base);
        else if (root.highlightCount > 0)
            base = qsTr("%1, %n mention(s)", "", root.highlightCount).arg(base);
        else if (root.readsUnread)
            base = qsTr("%1, unread").arg(base);
        if (root.isLowPriority)
            base = qsTr("%1, low priority").arg(base);
        return base;
    }

    ToolTip.text: root.channelName
    ToolTip.visible: root.hovered && !root.contextMenuOpen
                     && nameLoader.item !== null && nameLoader.item.truncated
    ToolTip.delay: 600

    background: Rectangle {
        radius: AppTheme.radiusSm
        anchors.fill: parent
        anchors.leftMargin: 8 + root.depth * 10
        anchors.rightMargin: 8
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        color: root.active ? AppTheme.channelSelected : root.batchSelected ? AppTheme.chipAccentFill : (root.hovered || root.activeFocus ? AppTheme.channelHover : "transparent")
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

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 2 + root.depth * 10
            width: 4
            height: 20
            radius: 2
            color: AppTheme.channelUnreadMark
            visible: root.readsUnread && !root.active && !root.muted
        }

        Avatar {
            id: roomAvatar
            objectName: "channelAvatar"
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 12 + root.depth * 10
            width: 20
            height: 20
            size: 20
            circle: root.isDirect
            squareRadius: 6
            labelSize: 9
            name: root.channelName
            colorKey: root.identityColorKey.length > 0 ? root.identityColorKey : root.roomId
            mxc: root.inviteAvatarWithheld ? "" : root.avatarUrl
        }

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
                color: AppTheme.roomList
                Icon {
                    anchors.centerIn: parent
                    name: root.isInvite ? "person_add" : "lock"
                    size: 9
                    color: root.isInvite ? AppTheme.accent : AppTheme.channelCategoryText
                }
            }
        }

        Loader {
            id: nameLoader
            objectName: "channelNameLoader"
            active: root.channelName.length > 0
            anchors.left: roomAvatar.right
            anchors.leftMargin: 8
            anchors.right: favouriteStar.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Label {
                // Remote or externally chosen text: never markup.
                textFormat: Text.PlainText
                text: root.isLowPriority
                      ? qsTr("%1 · Low priority").arg(root.channelName)
                      : root.channelName
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                font.weight: root.readsUnread && !root.muted ? AppTheme.weightMedium : AppTheme.weightBody
                color: root.active ? AppTheme.channelSelectedText : (root.muted ? AppTheme.channelCategoryText : (root.readsUnread ? AppTheme.channelTextUnread : AppTheme.channelText))
            }
        }

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
            sourceComponent: root.threadOnly ? threadBadgeComponent : countBadgeComponent
        }

        Component {
            id: threadBadgeComponent
            ThreadUnreadBadge { kind: root.threadUnreadKind }
        }
        Component {
            id: countBadgeComponent
            UnreadBadge {
                count: root.highlightCount > 0 ? root.highlightCount
                                               : root.unreadCount
                mention: root.highlightCount > 0
                dot: root.hasUnread || root.isInvite
            }
        }

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
                HoverHandler {
                    id: muteHover
                }
                ToolTip.text: qsTr("Muted")
                ToolTip.visible: muteHover.hovered && !root.contextMenuOpen
                ToolTip.delay: 400
            }
        }
    }

    readonly property bool contextMenuOpen: menuLoader.item !== null && menuLoader.item.visible
    function openContextMenu() {
        if (root.roomId.length === 0)
            return;
        menuLoader.active = true;
        if (menuLoader.item)
            menuLoader.item.popup();
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        enabled: root.roomId.length > 0
        onTapped: root.openContextMenu()
    }
    // Touch has no right button, so a long press is its way to the menu.
    // Unlike a bare TapHandler, ItemDelegate still emits clicked when a held
    // press is released, so `heldForMenu` tells the host to skip that one.
    property bool heldForMenu: false
    TapHandler {
        acceptedDevices: PointerDevice.TouchScreen
        enabled: root.roomId.length > 0
        onLongPressed: {
            root.heldForMenu = true;
            root.openContextMenu();
        }
    }
    property bool pointerActivation: false
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onPressedChanged: if (pressed) {
            root.pointerActivation = true;
            root.heldForMenu = false;
        }
    }
    Keys.onPressed: event => {
        root.pointerActivation = false;
        if (event.key === Qt.Key_Space
            && (app.roomBatch.selecting || (event.modifiers & Qt.ControlModifier))) {
            root.clicked()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Escape && app.roomBatch.selecting) {
            app.roomBatch.exitSelection()
            event.accepted = true
            return
        }
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
            isLowPriority: root.isLowPriority
            isInvite: root.isInvite
            isUnread: root.readsUnread || root.rawUnread
            onAcceptInviteRequested: root.acceptInvite()
            onRejectInviteRequested: root.rejectInvite()
            onMarkRead: root.markRead()
            onMarkUnread: root.markUnread()
            onSetFavourite: on => root.setFavourite(on)
            onSetLowPriority: on => root.setLowPriority(on)
            onSetNotificationMode: mode => root.setNotificationMode(mode)
            onCopyRoomLink: root.copyRoomLink()
            onLeaveRoomRequested: root.leaveRoomRequested()
            onChatBackgroundRequested: root.chatBackgroundRequested()
            onInviteRequested: root.inviteRequested()
            onSelectRoomRequested: root.selectRoomRequested()
        }
    }
}
