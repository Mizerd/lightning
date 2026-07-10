import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    implicitHeight: content.implicitHeight + AppTheme.spacingM * 2
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

    Rectangle {
        anchors.fill: parent
        color: selected ? AppTheme.accent
             : hover.hovered ? AppTheme.surfaceAlt
             : "transparent"
        HoverHandler { id: hover }
        TapHandler { onTapped: root.clicked() }
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: AppTheme.spacingM
        spacing: AppTheme.spacingM

        Rectangle {
            width: 40; height: 40
            radius: 20
            color: selected ? AppTheme.accentText : AppTheme.surfaceAlt
            Label {
                anchors.centerIn: parent
                text: (model.name && model.name.length > 0) ? model.name.charAt(0).toUpperCase() : "?"
                color: selected ? AppTheme.accent : AppTheme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: model.name
                    color: selected ? AppTheme.accentText : AppTheme.text
                    font.pixelSize: 14
                    font.weight: model.hasUnread || model.markedUnread
                                 ? Font.Bold : Font.Medium
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    visible: model.encrypted === true
                    text: "\u{1F512}"
                    font.pixelSize: 12
                    color: selected ? AppTheme.accentText : AppTheme.textMuted
                }
                Label {
                    visible: model.lastActivity && model.lastActivity.toString() !== ""
                    text: visible ? Qt.formatDateTime(model.lastActivity, "hh:mm") : ""
                    font.pixelSize: AppTheme.fontSizeXS
                    color: selected ? AppTheme.accentText : AppTheme.textMuted
                }
                Label {
                    visible: model.unreadCount > 0 || model.highlightCount > 0
                    text: model.highlightCount > 0 ? model.highlightCount : model.unreadCount
                    color: selected ? AppTheme.accent : AppTheme.accentText
                    background: Rectangle {
                        color: selected ? AppTheme.accentText
                              : (model.highlightCount > 0 ? AppTheme.error : AppTheme.accent)
                        radius: 8
                    }
                    leftPadding: 6; rightPadding: 6; topPadding: 1; bottomPadding: 1
                    font.pixelSize: 11
                }
            }
            Label {
                text: model.lastMessagePreview
                color: selected ? Qt.rgba(1, 1, 1, 0.85) : AppTheme.textMuted
                font.pixelSize: 12
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
                Button {
                    text: qsTr("Accept")
                    enabled: !model.invitePending
                    Accessible.name: qsTr("Accept room invitation")
                    onClicked: root.acceptInvite()
                }
                Button {
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
    Menu {
        id: roomMenu
        MenuItem { text: qsTr("Mark as read"); onTriggered: root.markRead() }
        MenuItem { text: qsTr("Mark as unread"); onTriggered: root.markUnread() }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: AppTheme.border
        opacity: 0.5
    }
}
