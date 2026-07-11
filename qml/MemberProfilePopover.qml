import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.9 (Phase 5): reusable member profile popover. Shows the member's
// avatar initial, display name, full Matrix ID, membership state and role,
// and offers Start/Open Direct Message (existing DMs are reused, never
// silently duplicated). No moderation actions in this release.
Popup {
    id: root
    modal: true
    padding: AppTheme.spacing12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string userId: ""
    property string displayName: ""
    property string membership: ""
    property string role: ""
    property bool isOwn: false

    function openFor(member) {
        userId = member.userId || ""
        displayName = member.displayName || ""
        membership = member.membership || ""
        role = member.role || ""
        isOwn = member.isOwn === true
        open()
    }

    function startOrOpenDm() {
        // Reuse an existing DM when the SDK's m.direct projection has one;
        // otherwise create a new encrypted DM (opened via conversationReady).
        app.conversations.checkExistingDm(userId)
        var existing = app.conversations.existingDms
        if (existing.length > 0)
            app.openRoom(existing[0].roomId)
        else
            app.conversations.startDirectMessage(userId)
        close()
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusMd
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing8
        implicitWidth: 260

        RowLayout {
            spacing: AppTheme.spacing12
            Rectangle {
                width: 48; height: 48
                radius: AppTheme.radiusPill
                color: AppTheme.cardElevated
                Label {
                    anchors.centerIn: parent
                    text: {
                        var n = root.displayName.length > 0
                                ? root.displayName
                                : (root.userId.length > 1 ? root.userId.slice(1) : "?")
                        return n.length > 0 ? n[0].toUpperCase() : "?"
                    }
                    color: AppTheme.textSecondary
                    font.pixelSize: AppTheme.fontSectionTitle
                    font.weight: Font.DemiBold
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    text: root.displayName.length > 0 ? root.displayName
                                                      : root.userId
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.fontBody
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.displayName.length > 0
                    text: root.userId
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontCaption
                    elide: Label.ElideMiddle
                }
            }
        }

        RowLayout {
            spacing: AppTheme.spacing8
            Label {
                text: root.membership === "invited" ? qsTr("Invited")
                    : root.membership === "joined" ? qsTr("Member")
                    : root.membership
                color: root.membership === "invited" ? AppTheme.warning
                                                     : AppTheme.textSecondary
                font.pixelSize: AppTheme.fontSecondary
            }
            Label {
                visible: root.role === "administrator" || root.role === "creator"
                text: qsTr("Administrator")
                color: AppTheme.accent
                font.pixelSize: AppTheme.fontSecondary
            }
            Label {
                visible: root.role === "moderator"
                text: qsTr("Moderator")
                color: AppTheme.accent
                font.pixelSize: AppTheme.fontSecondary
            }
            Label {
                visible: root.isOwn
                text: qsTr("(you)")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSecondary
            }
        }

        Button {
            Layout.fillWidth: true
            visible: !root.isOwn && app.conversations.supported
            highlighted: true
            text: qsTr("Message")
            Accessible.name: qsTr("Start or open a direct message with %1")
                             .arg(root.displayName.length > 0 ? root.displayName
                                                              : root.userId)
            onClicked: root.startOrOpenDm()
        }
    }
}
