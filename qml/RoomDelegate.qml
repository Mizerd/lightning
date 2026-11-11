import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    readonly property bool compactRows: app.settings.compactRoomList
    implicitHeight: content.implicitHeight + AppTheme.roomRowPaddingV * 2
    Accessible.role: Accessible.ListItem
    Accessible.name: {
        if (model.membership === "invited")
            return qsTr("Invitation to %1").arg(model.name)
        var base = root.threadOnly
                   ? (model.threadUnreadKind === 2
                      ? qsTr("%1, mention in a thread", "room row")
                      : qsTr("%1, unread in a thread", "room row")).arg(model.name)
                   : (model.highlightCount > 0 && !root.muted)
                   ? qsTr("%1, %n mention(s)", "", model.highlightCount)
                         .arg(model.name)
                   : (model.unreadCount > 0 && !root.muted)
                   ? qsTr("%1, %n unread message(s)", "", model.unreadCount)
                         .arg(model.name)
                   : (model.hasUnread && !root.muted)
                   ? qsTr("%1, unread", "room row").arg(model.name)
                   : (model.markedUnread && !root.muted)
                   ? qsTr("%1, marked as unread", "room row").arg(model.name)
                   : model.name
        return root.muted ? qsTr("%1, muted").arg(base) : base
    }
    activeFocusOnTab: true

    property bool selected: false
    property bool showGroupDivider: false
    signal clicked()
    signal acceptInvite()
    signal rejectInvite()
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
    signal inviterProfileRequested(var member, Item anchor)

    readonly property color metaInk: selected ? AppTheme.selectedText
                                              : AppTheme.textMuted

    readonly property bool isUnread: model.hasUnread || model.markedUnread
    readonly property bool canMarkRead: model.rawUnread === true
        || isUnread || model.unreadCount > 0 || model.highlightCount > 0

    readonly property string roomId: model.roomId || ""
    property int notificationMode: 0
    readonly property bool muted: notificationMode === 2
    function refreshNotificationMode() {
        if (typeof app === "undefined" || !app || !app.settings)
            return
        notificationMode = roomId.length > 0
                           ? app.settings.roomNotificationMode(roomId) : 0
    }
    onRoomIdChanged: refreshNotificationMode()
    Component.onCompleted: refreshNotificationMode()
    Connections {
        target: app.settings
        function onRoomNotificationModeChanged(changedRoomId) {
            if (changedRoomId === root.roomId)
                root.refreshNotificationMode()
        }
    }

    readonly property bool needsAttention:
        model.membership === "invited" || model.membership === "knocked"

    readonly property bool threadOnly:
        !root.muted && (model.threadUnreadKind || 0) > 0
    readonly property bool hasCountBadge:
        !root.muted && !root.threadOnly
        && (model.unreadCount > 0 || model.highlightCount > 0)

    function activityLabel(when) {
        if (!when || isNaN(when.getTime()) || when.getTime() <= 0)
            return ""
        var now = new Date()
        var days = Math.floor((now - when) / 86400000)
        if (when.toDateString() === now.toDateString())
            return Qt.formatTime(when, app.settings.clockTimeFormat)
        if (days < 2) return qsTr("Yesterday")
        if (days < 7) return Qt.locale(AppLocale.name).toString(when, "ddd")
        if (when.getFullYear() === now.getFullYear())
            return Qt.locale(AppLocale.name).toString(when, "d MMM")
        return Qt.locale(AppLocale.name).toString(when, "MMM yyyy")
    }

    Rectangle {
        objectName: "roomRowChip"
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing4
        anchors.rightMargin: AppTheme.spacing4
        anchors.bottomMargin: root.showGroupDivider ? 1 : 0
        radius: AppTheme.radiusMd
        color: selected ? (hover.hovered ? AppTheme.roomSelectedHover : AppTheme.roomSelected)
             : hover.hovered ? AppTheme.roomHover
             : root.needsAttention ? AppTheme.chipAccentFill
             : "transparent"
        TapHandler { onTapped: root.clicked() }
    }
    HoverHandler { id: hover }

    Rectangle {
        visible: selected
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: Math.max(16, parent.height - AppTheme.spacing12)
        radius: width / 2
        color: AppTheme.bolt
    }

    Rectangle {
        objectName: "roomGroupDivider"
        z: 1
        visible: root.showGroupDivider
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing8
        height: 1
        color: AppTheme.borderStrong
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing8
        anchors.topMargin: AppTheme.roomRowPaddingV
        anchors.bottomMargin: AppTheme.roomRowPaddingV
        spacing: AppTheme.spacing10

        Avatar {
            size: 32
            name: model.name || ""
            // A stranger's invite picture loads only when the account allows
            // it (MSC4278 invite_avatars); the letter tile stands in.
            mxc: (model.membership === "invited" && !(app.accountPrefs
                  && app.accountPrefs.inviteAvatarsAllowed))
                 ? "" : (model.avatarUrl || "")
            colorKey: model.identityColorKey || model.roomId || ""
            circle: model.isDirect === true
            opacity: root.muted && !selected ? 0.55 : 1.0
            Layout.alignment: model.membership === "invited"
                              ? Qt.AlignTop : Qt.AlignVCenter
            Layout.topMargin: model.membership === "invited" ? 2 : 0

            PresenceDot {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: -1
                dotSize: 10
                ring: selected ? AppTheme.selected : AppTheme.roomList
                userId: model.isDirect === true
                        && (model.identityColorKey || "").charAt(0) === "@"
                        ? model.identityColorKey : ""
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                id: nameRow
                Layout.fillWidth: true
                spacing: AppTheme.spacing6
                Label {
                    id: nameLabel
                    objectName: "roomNameLabel"
                    text: model.name
                    textFormat: Text.PlainText
                    color: selected ? AppTheme.roomSelectedName
                         : (root.isUnread && !root.muted) ? AppTheme.roomNameUnread
                                                          : AppTheme.roomName
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    font.weight: (selected || (root.isUnread && !root.muted))
                                 ? AppTheme.weightBold : AppTheme.weightMedium
                    elide: Label.ElideRight
                    // Explicit, not the implicit default: an implicit left follows
                    // the TEXT's direction, so an English name floated mid-row
                    // in an RTL UI while Arabic hugged the avatar. An explicit
                    // left is mirrored with the layout.
                    horizontalAlignment: Text.AlignLeft
                    Layout.fillWidth: true
                    Layout.minimumWidth: Math.min(implicitWidth, 72)
                    ToolTip.visible: nameHover.hovered && nameLabel.truncated
                    ToolTip.delay: 600
                    ToolTip.text: model.name
                    HoverHandler { id: nameHover }
                }
                // Bridged-network badge; empty for native rooms. A Loader
                // rather than an empty Label: a Text born with "" never clears
                // ItemObservesViewport, and Qt would walk the subtree on every
                // scroll frame. Label and selection are passed on the Loader
                // rather than read as `model.` inside the component.
                Loader {
                    id: networkTagLoader
                    objectName: "roomNetworkTagLoader"
                    readonly property string label: model.networkLabel || ""
                    readonly property bool tagSelected: selected
                    active: label !== "" && app.settings.showBridgeBadges
                    visible: active
                    Layout.maximumWidth: Math.max(0, nameRow.width / 3)
                    Layout.alignment: Qt.AlignVCenter
                    sourceComponent: Label {
                        objectName: "roomNetworkTag"
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        text: networkTagLoader.label
                        textFormat: Text.PlainText
                        color: networkTagLoader.tagSelected
                               ? AppTheme.selectedText : AppTheme.textMuted
                        font.pixelSize: AppTheme.fontCaption
                        font.weight: Font.DemiBold
                        leftPadding: 5; rightPadding: 5
                        background: Rectangle {
                            color: AppTheme.hover
                            border.color: AppTheme.border
                            border.width: 1
                            radius: AppTheme.radiusSm
                        }
                    }
                }
                Icon {
                    visible: model.encrypted === true
                    name: "lock"
                    size: 12
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                RoomCallGlyph {
                    objectName: "roomCallGlyph"
                    roomId: model.roomId
                    glyphSize: 13
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                Icon {
                    objectName: "roomMutedGlyph"
                    visible: root.muted
                    name: "notifications_off"
                    size: 13
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                    Accessible.ignored: true
                }
                Label {
                    objectName: "roomActivityLabel"
                    visible: !!model.lastActivity
                             && model.lastActivity.toString() !== ""
                             && text.length > 0
                    text: root.activityLabel(model.lastActivity)
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.weight: (root.isUnread && !root.muted)
                                 ? AppTheme.weightStrong : AppTheme.weightBody
                    color: selected ? AppTheme.selectedText
                         : (root.isUnread && !root.muted)
                           ? AppTheme.textSecondary : AppTheme.textMuted
                }
            }

            RowLayout {
                id: previewRow
                visible: !root.compactRows || model.membership === "invited"
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Label {
                    objectName: "roomPreviewLabel"
                    visible: !root.compactRows && model.membership !== "invited"
                    text: model.lastMessagePreview
                    color: selected ? AppTheme.roomSelectedPreview : AppTheme.roomPreview
                    opacity: selected ? 0.9 : 1.0
                    font: app.textFontWithEmoji(
                              AppTheme.uiFont,
                              AppTheme.scaled(AppTheme.textMeta))
                    elide: Label.ElideRight
                    horizontalAlignment: Text.AlignLeft
                    maximumLineCount: 1
                    wrapMode: Text.NoWrap
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                }

                RowLayout {
                    id: unreadIndicators
                    objectName: "roomUnreadIndicators"
                    spacing: AppTheme.spacing6
                    parent: root.compactRows ? nameRow : previewRow
                    Rectangle {
                        id: roomUnreadDot
                        objectName: "roomUnreadDot"
                        visible: root.isUnread && !root.hasCountBadge && !root.muted
                                 && !root.threadOnly
                        implicitWidth: 8
                        implicitHeight: 8
                        radius: 4
                        Layout.alignment: Qt.AlignVCenter
                        color: root.muted ? root.metaInk : AppTheme.unreadDot
                        border.width: root.muted ? 0 : AppTheme.badgeDotRingWidth
                        border.color: AppTheme.unreadDotRing
                    }
                    ThreadUnreadBadge {
                        kind: root.threadOnly ? model.threadUnreadKind : 0
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Label {
                        objectName: "roomUnreadBadge"
                        visible: root.hasCountBadge
                        text: model.highlightCount > 0 ? "@" + model.highlightCount
                                                       : model.unreadCount
                        Accessible.name: String(model.highlightCount > 0 ? model.highlightCount
                                                                         : model.unreadCount)
                        color: model.highlightCount > 0 ? AppTheme.mentionBadgeInk
                             : root.muted ? root.metaInk
                                          : AppTheme.unreadCountInk
                        background: Rectangle {
                            color: model.highlightCount > 0 ? AppTheme.roomMentionBadge
                                 : root.muted ? "transparent"
                                              : AppTheme.unreadCountFill
                            border.width: root.muted && model.highlightCount === 0
                                          ? 1 : 0
                            border.color: AppTheme.chipNeutralBorder
                            radius: AppTheme.radiusPill
                        }
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: AppTheme.spacing6
                        rightPadding: AppTheme.spacing6
                        font.pixelSize: AppTheme.textMicro
                        font.weight: AppTheme.weightBold
                        Layout.preferredHeight: 18
                        Layout.minimumWidth: 18
                    }

                    Rectangle {
                        objectName: "roomMarkedUnreadDot"
                        visible: model.markedUnread === true && !root.hasCountBadge
                                 && !roomUnreadDot.visible
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        Layout.alignment: Qt.AlignVCenter
                        radius: 4
                        color: root.muted ? root.metaInk : AppTheme.unreadDot
                        border.width: root.muted ? 0 : AppTheme.badgeDotRingWidth
                        border.color: AppTheme.unreadDotRing
                    }
                }
            }

            ColumnLayout {
                visible: model.membership === "invited"
                Layout.fillWidth: true
                spacing: AppTheme.spacing4
                Label {
                    objectName: "roomInviteLabel"
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    readonly property string inviterName:
                        String(model.inviter || "").replace(/\s+/g, " ").trim()
                    text: inviterName
                          ? (model.isSpace
                             ? qsTr("%1 invited you to a Space").arg(inviterName)
                             : (model.isDirect
                                ? qsTr("%1 invited you to chat").arg(inviterName)
                                : qsTr("%1 invited you to a room").arg(inviterName)))
                          : (model.isSpace ? qsTr("Space invitation")
                                           : (model.isDirect
                                              ? qsTr("Direct message invitation")
                                              : qsTr("Room invitation")))
                    color: root.metaInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.underline: inviterMouse.containsMouse
                    elide: Label.ElideRight
                    MouseArea {
                        id: inviterMouse
                        objectName: "roomInviterProfileArea"
                        anchors.fill: parent
                        enabled: root.inviterUserId !== ""
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("View %1's profile")
                            .arg(parent.inviterName || root.inviterUserId)
                        onClicked: root.openInviterProfile(parent)
                    }
                }
                Label {
                    objectName: "roomInviteCommonRooms"
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: {
                        if (!root.isInviteRow || root.inviterUserId === ""
                                || !app.roomInfo || !app.roomInfo.mutualRoomHint)
                            return ""
                        var _rev = app.roomInfo.mutualSummaryRevision
                        return app.roomInfo.mutualRoomHint(root.inviterUserId)
                    }
                    visible: text.length > 0
                    color: root.metaInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideRight
                }
                RowLayout {
                    objectName: "roomInviteButtons"
                    spacing: AppTheme.spacingS
                    AppButton {
                        kind: "primary"
                        implicitHeight: AppTheme.buttonHeightSm
                        leftPadding: AppTheme.buttonPaddingHSm
                        rightPadding: AppTheme.buttonPaddingHSm
                        text: qsTr("Accept")
                        enabled: !model.invitePending
                        Accessible.name: qsTr("Accept room invitation")
                        onClicked: root.acceptInvite()
                    }
                    AppButton {
                        kind: "danger"
                        implicitHeight: AppTheme.buttonHeightSm
                        leftPadding: AppTheme.buttonPaddingHSm
                        rightPadding: AppTheme.buttonPaddingHSm
                        text: qsTr("Reject")
                        enabled: !model.invitePending
                        Accessible.name: qsTr("Reject room invitation")
                        onClicked: root.rejectInvite()
                    }
                }
            }
            Label {
                textFormat: Text.PlainText
                visible: model.inviteError && model.inviteError.length > 0
                text: model.inviteError || ""
                color: AppTheme.error
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                wrapMode: Text.WordWrap
                lineHeight: AppTheme.lineHeightTight
                lineHeightMode: Text.ProportionalHeight
                Layout.fillWidth: true
            }

            StatusChip {
                objectName: "roomUpgradedChip"
                visible: model.supersededByAccessibleSuccessor === true
                label: qsTr("Upgraded")
                tone: "neutral"
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("This room has been upgraded")
            }

            RowLayout {
                visible: model.membership === "knocked"
                spacing: AppTheme.spacingS
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Join request pending")
                    color: root.metaInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideRight
                }
                AppButton {
                    implicitHeight: AppTheme.buttonHeightSm
                    leftPadding: AppTheme.buttonPaddingHSm
                    rightPadding: AppTheme.buttonPaddingHSm
                    text: qsTr("Withdraw")
                    Accessible.name: qsTr("Withdraw the join request")
                    onClicked: app.discovery.cancelKnock(model.roomId)
                }
            }
        }
    }

    readonly property bool isInviteRow: model.membership === "invited"
    readonly property string inviterUserId: model.inviterUserId || ""
    property Item sharedRowMenu: null
    readonly property string rowRoomId: model.roomId !== undefined ? model.roomId : ""
    onRowRoomIdChanged: {
        if (inviteMenuLoader.item)
            inviteMenuLoader.item.close()
        if (roomMenuLoader.item)
            roomMenuLoader.item.close()
    }
    function rowMenuState() {
        return {
            roomId: model.roomId,
            roomName: model.name || "",
            canonicalAlias: model.canonicalAlias || "",
            isDirect: model.isDirect === true,
            isFavourite: model.isFavourite === true,
            isLowPriority: model.isLowPriority === true,
            isUnread: root.canMarkRead
        }
    }
    function openRowMenu() {
        if (!root.isInviteRow && root.sharedRowMenu
            && root.sharedRowMenu.openFor(root.rowMenuState()))
            return
        const host = root.isInviteRow ? inviteMenuLoader : roomMenuLoader
        host.active = true
        if (host.item)
            host.item.popup()
    }
    function openInviterProfile(anchor) {
        if (root.inviterUserId === "")
            return
        const name = String(model.inviter || "")
        root.inviterProfileRequested({
            userId: root.inviterUserId,
            displayName: name !== root.inviterUserId ? name : "",
            avatarUrl: "",
            roomScoped: false,
            withholdMedia: !(app.accountPrefs
                             && app.accountPrefs.inviteAvatarsAllowed)
        }, anchor)
    }
    Loader {
        id: inviteMenuLoader
        active: false
        sourceComponent: AppMenu {
            objectName: "inviteContextMenu"
            AppMenuItem {
                objectName: "inviteAcceptItem"
                iconName: "person_add"
                text: qsTr("Accept invitation")
                onTriggered: root.acceptInvite()
            }
            AppMenuItem {
                objectName: "inviteRejectItem"
                iconName: "close"
                text: qsTr("Reject invitation")
                danger: true
                onTriggered: root.rejectInvite()
            }
            AppMenuItem {
                objectName: "inviteViewInviterItem"
                visible: root.inviterUserId !== ""
                enabled: visible
                iconName: "person"
                text: qsTr("View inviter's profile")
                onTriggered: root.openInviterProfile(null)
            }
            AppMenuItem {
                objectName: "inviteIgnoreUserItem"
                visible: root.inviterUserId !== ""
                enabled: visible
                iconName: "block"
                text: qsTr("Ignore user")
                onTriggered: app.moderation.ignoreUser(root.inviterUserId)
            }
        }
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        enabled: model.membership === "joined" || root.isInviteRow
        onTapped: root.openRowMenu()
    }
    TapHandler {
        acceptedDevices: PointerDevice.TouchScreen
        enabled: model.membership === "joined" || root.isInviteRow
        onLongPressed: root.openRowMenu()
    }
    Keys.onPressed: (event) => {
        if (model.membership === "joined"
            && (event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10
                    && (event.modifiers & Qt.ShiftModifier)))) {
            root.openRowMenu()
            event.accepted = true
        }
    }
    Rectangle {
        objectName: "roomRowFocusRing"
        anchors.fill: parent
        anchors.margins: 1
        visible: root.activeFocus
        radius: AppTheme.radiusMd
        color: "transparent"
        border.width: 2
        border.color: AppTheme.focusRing
        z: 50
    }
    Loader {
        id: roomMenuLoader
        active: false
        sourceComponent: RoomActionsMenu {
            objectName: "roomContextMenu"
            roomId: model.roomId
            roomName: model.name || ""
            canonicalAlias: model.canonicalAlias || ""
            isDirect: model.isDirect === true
            isFavourite: model.isFavourite === true
            isLowPriority: model.isLowPriority === true
            isUnread: root.canMarkRead
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
