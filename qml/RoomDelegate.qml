import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    implicitHeight: content.implicitHeight + AppTheme.spacing6 * 2
    Accessible.role: Accessible.ListItem
    Accessible.name: {
        if (model.membership === "invited")
            return qsTr("Invitation to %1").arg(model.name)
        var base = model.highlightCount > 0
                   ? qsTr("%1, %2 mentions").arg(model.name)
                                            .arg(model.highlightCount)
                   : model.name
        // The mute glyph is decorative, so the accessible name says it.
        return root.muted ? qsTr("%1, muted").arg(base) : base
    }
    // Every row is a tab stop so the keyboard context menu (Menu key /
    // Shift+F10) reaches any room, not just the selected one. Joined rooms
    // only, like right-click.
    activeFocusOnTab: true

    property bool selected: false
    // Rule under the last favourite, set by RoomsPanel (only it can see the
    // section boundary). Pinned section labels have no fill, so a short
    // favourites block would otherwise merge with the next group.
    property bool showGroupDivider: false
    signal clicked()
    signal acceptInvite()
    signal rejectInvite()
    signal markRead()
    signal markUnread()
    // Carries the value to write, not a toggle; the row never flips it locally,
    // so a refused write cannot desynchronize it.
    signal setFavourite(bool on)
    // Signal-only: the host (RoomsPanel) performs the mutations.
    signal setNotificationMode(int mode)
    signal copyRoomLink()
    signal leaveRoomRequested()

    // Meta ink for this row: textMuted is tuned for the list surface, not the
    // selected chip, where it fails contrast.
    readonly property color metaInk: selected ? AppTheme.selectedText
                                              : AppTheme.textMuted

    // Read rows are dimmed and lighter; unread or selected rows keep full ink.
    readonly property bool isUnread: model.hasUnread || model.markedUnread

    // Mute state. roomNotificationMode is a Q_INVOKABLE, so it is re-queried
    // when the id changes (rows are recycled) and when settings announce a
    // write. SettingsManager caches the account slug for this hot path.
    readonly property string roomId: model.roomId || ""
    property int notificationMode: 0
    readonly property bool muted: notificationMode === 2
    function refreshNotificationMode() {
        // Guarded like Avatar's `bridge` lookup: a delegate created inside a
        // property-change handler can see `app` undefined. Keep the last known
        // mode rather than asserting "not muted".
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

    // Invitations and pending knocks get a tinted chip, without spending the
    // accent.
    readonly property bool needsAttention:
        model.membership === "invited" || model.membership === "knocked"

    // One predicate for whether there is a count, shared by the pill and the
    // dots so they never both render.
    readonly property bool hasCountBadge:
        model.unreadCount > 0 || model.highlightCount > 0

    // A clock time only for today, as Element does. Identical to
    // HomePane.activityLabel().
    function activityLabel(when) {
        if (!when || isNaN(when.getTime()) || when.getTime() <= 0)
            return ""
        var now = new Date()
        var days = Math.floor((now - when) / 86400000)
        if (when.toDateString() === now.toDateString())
            // The app-wide clock format, resolved in C++ to a Qt format string.
            // Being inside a function, it updates when the caller's binding
            // next re-evaluates.
            return Qt.formatTime(when, app.settings.clockTimeFormat)
        if (days < 2) return qsTr("Yesterday")
        if (days < 7) return Qt.formatDate(when, "ddd")
        if (when.getFullYear() === now.getFullYear())
            return Qt.formatDate(when, "d MMM")
        return Qt.formatDate(when, "MMM yyyy")
    }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing4
        anchors.rightMargin: AppTheme.spacing4
        // The group rule uses the row's last pixel line, kept off the selection
        // chip where it would be invisible.
        anchors.bottomMargin: root.showGroupDivider ? 1 : 0
        // An 8px rounded highlight chip from the semantic tokens.
        radius: AppTheme.radiusMd
        color: selected ? (hover.hovered ? AppTheme.selectedHover : AppTheme.selected)
             : hover.hovered ? AppTheme.hover
             : root.needsAttention ? AppTheme.chipAccentFill
             : "transparent"
        HoverHandler { id: hover }
        TapHandler { onTapped: root.clicked() }
    }

    // Selected-room edge bar in the gutter left of the chip, rounded and inset
    // so it reads as a marker rather than a table border.
    Rectangle {
        visible: selected
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: Math.max(16, parent.height - AppTheme.spacing12)
        radius: width / 2
        color: AppTheme.bolt
    }

    // Drawn inside the row's bounds so it never shifts the rows below; inset to
    // line up with the avatar column.
    Rectangle {
        objectName: "roomGroupDivider"
        // Above every other layer in the row.
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
        anchors.topMargin: AppTheme.spacing6
        anchors.bottomMargin: AppTheme.spacing6
        spacing: AppTheme.spacing10

        Avatar {
            size: 32
            name: model.name || ""
            mxc: model.avatarUrl || ""
            // One fallback-colour policy: an unambiguous DM is coloured as the
            // person.
            colorKey: model.identityColorKey || model.roomId || ""
            // People are circles; rooms and Spaces are rounded squares with a
            // "#" until the avatar loads.
            circle: model.isDirect === true
            // A muted room recedes as a whole, avatar included.
            opacity: root.muted && !selected ? 0.55 : 1.0
            // Invite rows have a third line; keep the avatar with the name at
            // the top.
            Layout.alignment: model.membership === "invited"
                              ? Qt.AlignTop : Qt.AlignVCenter
            Layout.topMargin: model.membership === "invited" ? 2 : 0

            // Presence on unambiguous 1:1 DMs only: identityColorKey is the
            // partner's MXID exactly then. Unknown presence renders nothing.
            PresenceDot {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: -1
                dotSize: 10
                // The ring must be the colour behind the dot (the sidebar, or
                // the selected chip).
                ring: selected ? AppTheme.selected : AppTheme.sidebar
                userId: model.isDirect === true
                        && (model.identityColorKey || "").charAt(0) === "@"
                        ? model.identityColorKey : ""
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6
                Label {
                    text: model.name
                    textFormat: Text.PlainText
                    // Read rows dim; unread and selected keep full ink. A muted
                    // room never claims unread emphasis.
                    color: selected ? AppTheme.selectedText
                         : (root.isUnread && !root.muted) ? AppTheme.textPrimary
                                                          : AppTheme.textSecondary
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    font.weight: (selected || (root.isUnread && !root.muted))
                                 ? AppTheme.weightBold : AppTheme.weightMedium
                    elide: Label.ElideRight
                    Layout.fillWidth: true
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
                    active: label !== ""
                    visible: active
                    sourceComponent: Label {
                        objectName: "roomNetworkTag"
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
                // Call-in-progress mark on the title line; zero width without a
                // call.
                RoomCallGlyph {
                    objectName: "roomCallGlyph"
                    roomId: model.roomId
                    glyphSize: 13
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                // The visible half of the mute state; the accessible name
                // carries the word.
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
                    visible: model.lastActivity
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
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                Label {
                    objectName: "roomPreviewLabel"
                    text: model.lastMessagePreview
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                    opacity: selected ? 0.9 : 1.0
                    // The same emoji-capable font as the composer and message
                    // body: a preview is message text and hits the same Qt 6.8
                    // monochrome fallback.
                    font: app.textFontWithEmoji(
                              AppTheme.uiFont,
                              AppTheme.scaled(AppTheme.textMeta))
                    elide: Label.ElideRight
                    // One line, always: explicit newlines would break lines
                    // even with elide. Plain text: a body must never
                    // rich-format the room list.
                    maximumLineCount: 1
                    wrapMode: Text.NoWrap
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                }

                // The count pill sits on the preview line (as Element does), so
                // the timestamp does not shift as counts change. An unread room
                // with no count still says so: servers often report 0 for a
                // genuinely unread room (counts follow push rules), so show a
                // dot.
                Rectangle {
                    objectName: "roomUnreadDot"
                    visible: root.isUnread && !root.hasCountBadge
                    implicitWidth: 8
                    implicitHeight: 8
                    radius: 4
                    Layout.alignment: Qt.AlignVCenter
                    // Muted rooms keep the dot in muted ink.
                    color: root.muted ? root.metaInk : AppTheme.unreadBadge
                }
                Label {
                    objectName: "roomUnreadBadge"
                    visible: root.hasCountBadge
                    text: model.highlightCount > 0 ? model.highlightCount
                                                   : model.unreadCount
                    // Mentions keep their colour even when muted; a muted plain
                    // count becomes an outline pill in muted ink.
                    color: model.highlightCount > 0 ? AppTheme.dangerText
                         : root.muted ? root.metaInk
                                      : AppTheme.accentText
                    background: Rectangle {
                        color: model.highlightCount > 0 ? AppTheme.mentionBadge
                             : root.muted ? "transparent"
                                          : AppTheme.unreadBadge
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
                    // A single digit renders as a circle.
                    Layout.preferredHeight: 18
                    Layout.minimumWidth: 18
                }

                // Marked-unread dot, shown only without a numeric badge, in the
                // unreadBadge colour, inline next to where the pill would be.
                Rectangle {
                    visible: model.markedUnread && !root.hasCountBadge
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    Layout.alignment: Qt.AlignVCenter
                    radius: 4
                    color: root.muted ? root.metaInk : AppTheme.unreadBadge
                }
            }

            RowLayout {
                visible: model.membership === "invited"
                spacing: AppTheme.spacingS
                Label {
                    // Untrusted text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: model.inviter
                          ? (model.isSpace
                             ? qsTr("Space invitation from %1").arg(model.inviter)
                             : qsTr("Room invitation from %1").arg(model.inviter))
                          : (model.isSpace ? qsTr("Space invitation")
                                           : qsTr("Room invitation"))
                    color: root.metaInk
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideRight
                }
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
            Label {
                // Untrusted text: never markup.
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

            // This room was replaced and the replacement is reachable (the
            // model requires the successor to be joined or invited and to point
            // back). Demoted and labelled, never hidden: the old room stays
            // readable.
            StatusChip {
                objectName: "roomUpgradedChip"
                visible: model.supersededByAccessibleSuccessor === true
                label: qsTr("Upgraded")
                tone: "neutral"
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("This room has been upgraded")
            }

            // Pending knock: the only actions are waiting and withdrawing.
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

    // The context menu is for joined rooms only; invitations offer
    // Accept/Reject.
    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: model.membership === "joined"
        onTapped: roomMenu.popup()
    }
    // Keyboard open path, joined rooms only like right-click.
    Keys.onPressed: (event) => {
        if (model.membership === "joined"
            && (event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10
                    && (event.modifiers & Qt.ShiftModifier)))) {
            roomMenu.popup()
            event.accepted = true
        }
    }
    // The menu lives in RoomActionsMenu.qml, shared with the Channels row.
    RoomActionsMenu {
        id: roomMenu
        objectName: "roomContextMenu"
        roomId: model.roomId
        roomName: model.name || ""
        canonicalAlias: model.canonicalAlias || ""
        isDirect: model.isDirect === true
        isFavourite: model.isFavourite === true
        onMarkRead: root.markRead()
        onMarkUnread: root.markUnread()
        onSetFavourite: on => root.setFavourite(on)
        onSetNotificationMode: mode => root.setNotificationMode(mode)
        onCopyRoomLink: root.copyRoomLink()
        onLeaveRoomRequested: root.leaveRoomRequested()
    }

    // No per-row hairline: rows separate through spacing and tints.
}
