import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    implicitHeight: content.implicitHeight + AppTheme.spacing6 * 2
    Accessible.role: Accessible.ListItem
    Accessible.name: model.membership === "invited"
                     ? qsTr("Invitation to %1").arg(model.name)
                     : (model.highlightCount > 0
                        ? qsTr("%1, %2 mentions").arg(model.name).arg(model.highlightCount)
                        : model.name)

    property bool selected: false
    signal clicked()
    signal acceptInvite()
    signal rejectInvite()
    signal markRead()
    signal markUnread()

    // Read rows are dimmed and lighter-weight; unread/selected rows carry
    // full ink (design handoff §2 room-row states).
    readonly property bool isUnread: model.hasUnread || model.markedUnread

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing4
        anchors.rightMargin: AppTheme.spacing4
        // v0.5.9: softer selected state from the semantic tokens — the
        // selected row keeps readable primary/secondary ink in both themes.
        // Design shell: row highlight is an 8px rounded chip, not a full-
        // bleed square.
        radius: AppTheme.radiusMd
        color: selected ? (hover.hovered ? AppTheme.selectedHover : AppTheme.selected)
             : hover.hovered ? AppTheme.hover
             : "transparent"
        HoverHandler { id: hover }
        TapHandler { onTapped: root.clicked() }
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.leftMargin: AppTheme.spacing8
        anchors.rightMargin: AppTheme.spacing8
        anchors.topMargin: AppTheme.spacing6
        anchors.bottomMargin: AppTheme.spacing6
        spacing: AppTheme.spacing8

        Avatar {
            size: 30
            name: model.name || ""
            mxc: model.avatarUrl || ""
            colorKey: model.roomId || ""
            // Design shell: people are circles, rooms and Spaces are
            // rounded squares that show a "#" glyph until the avatar loads.
            circle: model.isDirect === true
            roomGlyph: model.isDirect !== true
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: model.name
                    // Read rows dim to secondary ink; unread/selected keep
                    // full primary ink (handoff §2).
                    color: selected ? AppTheme.selectedText
                         : root.isUnread ? AppTheme.textPrimary
                                         : AppTheme.textSecondary
                    font.pixelSize: AppTheme.scaled(AppTheme.fontBody)
                    font.weight: (root.isUnread || selected)
                                 ? Font.Bold : Font.Medium
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                Icon {
                    visible: model.encrypted === true
                    name: "lock"
                    size: 12
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                Label {
                    visible: model.lastActivity && model.lastActivity.toString() !== ""
                    text: visible ? Qt.formatDateTime(model.lastActivity, "hh:mm") : ""
                    font.pixelSize: AppTheme.fontCaption
                    color: selected ? AppTheme.selectedText : AppTheme.textMuted
                }
                Label {
                    visible: model.unreadCount > 0 || model.highlightCount > 0
                    text: model.highlightCount > 0 ? model.highlightCount : model.unreadCount
                    // Mention pills use white ink on the red badge; plain
                    // unread pills invert to the row background colour on the
                    // accent fill (handoff §2 count pill).
                    color: model.highlightCount > 0 ? AppTheme.dangerText
                                                    : AppTheme.accentText
                    background: Rectangle {
                        color: model.highlightCount > 0 ? AppTheme.mentionBadge
                                                        : AppTheme.unreadBadge
                        radius: AppTheme.radiusPill
                    }
                    leftPadding: 7; rightPadding: 7; topPadding: 1; bottomPadding: 1
                    font.pixelSize: AppTheme.fontCaption
                    font.weight: Font.ExtraBold
                }
            }
            Label {
                text: model.lastMessagePreview
                color: selected ? AppTheme.selectedText : AppTheme.textMuted
                opacity: selected ? 0.9 : 1.0
                font.pixelSize: AppTheme.scaled(AppTheme.fontMessageSender)
                elide: Label.ElideRight
                Layout.fillWidth: true
            }

            RowLayout {
                visible: model.membership === "invited"
                spacing: AppTheme.spacingS
                Label {
                    Layout.fillWidth: true
                    text: (model.isSpace ? qsTr("Space invitation") : qsTr("Room invitation"))
                          + (model.inviter ? qsTr(" from %1").arg(model.inviter) : "")
                    color: AppTheme.textMuted
                    elide: Label.ElideRight
                }
                AppButton {
                    kind: "primary"
                    implicitHeight: 26
                    leftPadding: 10
                    rightPadding: 10
                    text: qsTr("Accept")
                    enabled: !model.invitePending
                    Accessible.name: qsTr("Accept room invitation")
                    onClicked: root.acceptInvite()
                }
                AppButton {
                    kind: "danger"
                    implicitHeight: 26
                    leftPadding: 10
                    rightPadding: 10
                    text: qsTr("Reject")
                    enabled: !model.invitePending
                    Accessible.name: qsTr("Reject room invitation")
                    onClicked: root.rejectInvite()
                }
            }
            Label {
                visible: model.inviteError && model.inviteError.length > 0
                text: model.inviteError || ""
                color: AppTheme.error
                wrapMode: Text.WordWrap
            }
        }
    }

    // Marked-unread dot. Shown only when there is no numeric badge, so it
    // can never overlap the unread/highlight badge.
    Rectangle {
        visible: model.markedUnread && model.unreadCount === 0
                 && model.highlightCount === 0
        width: 8; height: 8; radius: 4
        color: AppTheme.accent
        anchors.right: parent.right
        anchors.rightMargin: AppTheme.spacingS
        anchors.verticalCenter: parent.verticalCenter
    }

    // Read/unread context menu is a joined-room-only affordance; it must not
    // appear for invitations (which offer Accept/Reject instead).
    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: model.membership === "joined"
        onTapped: roomMenu.popup()
    }
    AppMenu {
        id: roomMenu
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
    }

    // Design shell: no per-row hairline — rows separate through spacing
    // and hover/selection tints only.
}
