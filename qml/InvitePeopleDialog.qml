import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.9 (Phase 10): invite users into an existing joined room. Opened from
// the Room Information People tab, and only when the SDK-derived
// permission (RoomMember::can_invite) allows it. Selected users are
// deduplicated; per-user pending/ok/failed state is shown; a failure for
// one user never discards the others' results.
Dialog {
    id: root
    modal: true
    title: qsTr("Invite people")
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(480, parent ? parent.width - AppTheme.spacing24 * 2 : 480)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property string roomId: ""
    property var selectedUsers: []
    property bool batchDone: false

    function openFor(targetRoomId) {
        roomId = targetRoomId
        selectedUsers = []
        batchDone = false
        picker.clear()
        app.conversations.reset()
        open()
        picker.focusSearch()
    }

    onClosed: {
        selectedUsers = []
        picker.clear()
        app.conversations.reset()
    }

    Connections {
        target: app.conversations
        function onInviteBatchCompleted(okCount, failCount) {
            root.batchDone = true
            // Membership updates arrive via authoritative sync; the Room
            // Information panel refreshes from membersChanged.
        }
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusLg
    }

    // True while the invitee is already joined/invited per the loaded
    // member snapshot (best-effort pre-check; the server remains the
    // authority).
    function membershipOf(userId) {
        var members = app.roomInfo.members
        for (var i = 0; i < members.length; ++i) {
            if (members[i].userId === userId)
                return members[i].membership
        }
        return ""
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            visible: app.conversations.errorMessage.length > 0
            Layout.fillWidth: true
            text: app.conversations.errorMessage
            color: AppTheme.danger
            wrapMode: Text.WordWrap
            font.pixelSize: AppTheme.fontSizeS
        }

        UserPicker {
            id: picker
            Layout.fillWidth: true
            visible: !root.batchDone
            onUserSelected: (userId, displayName) => {
                var membership = root.membershipOf(userId)
                if (membership === "joined" || membership === "invited") {
                    alreadyLabel.userId = userId
                    alreadyLabel.membership = membership
                    picker.clear()
                    return
                }
                alreadyLabel.userId = ""
                if (root.selectedUsers.indexOf(userId) === -1) {
                    var next = root.selectedUsers.slice()
                    next.push(userId)
                    root.selectedUsers = next
                }
                picker.clear()
            }
        }

        Label {
            id: alreadyLabel
            property string userId: ""
            property string membership: ""
            visible: userId.length > 0
            Layout.fillWidth: true
            text: membership === "joined"
                  ? qsTr("%1 is already in this room.").arg(userId)
                  : qsTr("%1 has already been invited.").arg(userId)
            color: AppTheme.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: AppTheme.fontSizeS
        }

        // Selected users before dispatch.
        Flow {
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            visible: root.selectedUsers.length > 0 && !app.conversations.busy
                     && !root.batchDone
            Repeater {
                model: root.selectedUsers
                Rectangle {
                    radius: AppTheme.radiusPill
                    color: AppTheme.cardElevated
                    implicitWidth: chipRow.implicitWidth + AppTheme.spacing12
                    implicitHeight: chipRow.implicitHeight + AppTheme.spacing4
                    RowLayout {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: AppTheme.spacing4
                        Label {
                            text: modelData
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSizeS
                        }
                        ToolButton {
                            implicitWidth: 18; implicitHeight: 18
                            contentItem: Icon { name: "close"; size: 14 }
                            Accessible.name: qsTr("Remove %1").arg(modelData)
                            onClicked: {
                                var next = root.selectedUsers.slice()
                                next.splice(index, 1)
                                root.selectedUsers = next
                            }
                        }
                    }
                }
            }
        }

        // Per-user progress/result once dispatched.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing4
            visible: app.conversations.inviteResults.length > 0
            Repeater {
                model: app.conversations.inviteResults
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Label {
                        Layout.fillWidth: true
                        text: modelData.userId
                        color: AppTheme.textPrimary
                        elide: Label.ElideMiddle
                        font.pixelSize: AppTheme.fontSizeS
                    }
                    Label {
                        text: {
                            if (modelData.state === "ok") return qsTr("Invited ✓")
                            if (modelData.state === "failed") {
                                if (modelData.category === "forbidden")
                                    return qsTr("Not permitted")
                                if (modelData.category === "rate_limited")
                                    return qsTr("Rate limited")
                                return qsTr("Failed")
                            }
                            return qsTr("Pending…")
                        }
                        color: modelData.state === "ok" ? AppTheme.success
                             : modelData.state === "failed" ? AppTheme.danger
                             : AppTheme.textMuted
                        font.pixelSize: AppTheme.fontSizeS
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            BusyIndicator {
                visible: app.conversations.busy
                running: visible
                implicitWidth: 20; implicitHeight: 20
            }
            Item { Layout.fillWidth: true }
            Button {
                visible: !root.batchDone
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            Button {
                visible: !root.batchDone
                highlighted: true
                enabled: root.selectedUsers.length > 0 && !app.conversations.busy
                text: root.selectedUsers.length > 1
                      ? qsTr("Invite %n people", "", root.selectedUsers.length)
                      : qsTr("Invite")
                onClicked: app.conversations.inviteUsers(root.roomId,
                                                         root.selectedUsers)
            }
            Button {
                visible: root.batchDone
                highlighted: true
                text: qsTr("Done")
                onClicked: root.close()
            }
        }
    }
}
