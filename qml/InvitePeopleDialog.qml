import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Dialog {
    id: root
    objectName: "invitePeopleDialog"
    modal: true
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(480, parent ? parent.width - AppTheme.spacing24 * 2 : 480)
    height: Math.min(implicitHeight,
                     parent ? parent.height - AppTheme.spacing24 * 2 : implicitHeight)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property string roomId: ""
    property var selectedUsers: []
    property bool batchDone: false
    property var recentSnapshot: []
    readonly property var suggestedPeople: {
        var out = []
        var ignored = app.moderation ? app.moderation.ignoredUsers : []
        for (var i = 0; i < root.recentSnapshot.length; ++i) {
            var p = root.recentSnapshot[i]
            if (root.selectedIndexOf(p.userId) !== -1)
                continue
            var m = root.membershipOf(p.userId)
            if (m === "joined" || m === "invited" || m === "banned")
                continue
            if (ignored.indexOf(p.userId) !== -1)
                continue
            out.push(p)
        }
        return out
    }

    readonly property var roomRecord: roomId !== ""
                                      ? app.roomList.findRoom(roomId) : ({})
    readonly property string roomDisplayName:
        roomRecord && roomRecord.name !== undefined
        ? (roomRecord.name || "") : ""
    readonly property string roomAddress:
        roomRecord && (roomRecord.canonicalAlias || "") !== ""
        ? roomRecord.canonicalAlias : roomId

    function openFor(targetRoomId) {
        roomId = targetRoomId
        selectedUsers = []
        batchDone = false
        recentSnapshot = app.conversations.recentPeople(12)
        picker.clear()
        app.conversations.reset()
        open()
        picker.focusSearch()
    }

    onClosed: {
        selectedUsers = []
        recentSnapshot = []
        picker.clear()
        app.conversations.reset()
    }

    Connections {
        target: app.conversations
        function onInviteBatchCompleted(okCount, failCount) {
            root.batchDone = true
        }
    }

    background: Rectangle {
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        radius: AppTheme.radiusLg
    }

    function membershipOf(userId) {
        if (!app.roomInfo || app.roomInfo.roomId !== root.roomId)
            return ""
        var members = app.roomInfo.members
        for (var i = 0; i < members.length; ++i) {
            if (members[i].userId === userId)
                return members[i].membership
        }
        return ""
    }

    function selectedIndexOf(userId) {
        for (var i = 0; i < selectedUsers.length; ++i) {
            if (selectedUsers[i].id === userId) return i
        }
        return -1
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        Label {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: root.roomDisplayName.length > 0
                  ? qsTr("Invite to %1").arg(root.roomDisplayName)
                  : qsTr("Invite people")
            color: AppTheme.stormText
            font.family: AppTheme.menuFont
            font.pixelSize: AppTheme.textTitle
            font.weight: AppTheme.weightBold
            elide: Label.ElideRight
        }

        Label {
            textFormat: Text.PlainText
            visible: root.roomAddress.length > 0
            Layout.fillWidth: true
            text: root.roomAddress
            color: AppTheme.stormTextMuted
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.fontMonoXS
            elide: Label.ElideRight
        }

        Label {
            visible: app.conversations.errorMessage.length > 0
            Layout.fillWidth: true
            text: app.conversations.errorMessage
            color: AppTheme.stormDanger
            wrapMode: Text.WordWrap
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
        }

        UserPicker {
            id: picker
            objectName: "invitePeoplePicker"
            Layout.fillWidth: true
            visible: !root.batchDone
            suggestions: root.suggestedPeople
            onUserSelected: (userId, displayName, avatarUrl) => {
                var membership = root.membershipOf(userId)
                // The server refuses invites to banned users, so point at the
                // remedy rather than letting it fail as "Not permitted".
                if (membership === "joined" || membership === "invited"
                        || membership === "banned") {
                    alreadyLabel.userId = userId
                    alreadyLabel.membership = membership
                    picker.clear()
                    return
                }
                alreadyLabel.userId = ""
                if (root.selectedIndexOf(userId) === -1) {
                    var next = root.selectedUsers.slice()
                    next.push({ id: userId, name: displayName || "",
                                avatar: avatarUrl || "" })
                    root.selectedUsers = next
                }
                picker.clear()
            }
        }

        Label {
            textFormat: Text.PlainText
            id: alreadyLabel
            property string userId: ""
            property string membership: ""
            visible: userId.length > 0
            Layout.fillWidth: true
            text: membership === "joined"
                  ? qsTr("%1 is already in this room.").arg(userId)
                  : membership === "banned"
                    ? qsTr("%1 is banned from this room. Unban them from "
                           + "the People tab first.").arg(userId)
                    : qsTr("%1 has already been invited.").arg(userId)
            color: AppTheme.stormTextMuted
            wrapMode: Text.WordWrap
            font.pixelSize: AppTheme.textBody
            lineHeight: AppTheme.lineHeightBody
            lineHeightMode: Text.ProportionalHeight
        }

        Flickable {
            id: inviteBodyFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: inviteBodyColumn.implicitHeight
            contentWidth: width
            contentHeight: inviteBodyColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            SmoothWheelArea {}

            ColumnLayout {
                id: inviteBodyColumn
                width: inviteBodyFlick.width
                spacing: AppTheme.spacing12

                Flow {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing6
                    visible: root.selectedUsers.length > 0 && !app.conversations.busy
                             && !root.batchDone
                    Repeater {
                        model: root.selectedUsers
                        Rectangle {
                            id: chip
                            required property var modelData
                            required property int index
                            objectName: "inviteChip_" + index
                            readonly property string label:
                                chip.modelData.name.length > 0
                                    ? chip.modelData.name : chip.modelData.id
                            radius: AppTheme.radiusPill
                            color: AppTheme.stormSelection
                            border.width: 1
                            border.color: AppTheme.stormBorderStrong
                            implicitWidth: chipRow.implicitWidth + AppTheme.spacing12
                            implicitHeight: chipRow.implicitHeight + AppTheme.spacing6
                            RowLayout {
                                id: chipRow
                                anchors.centerIn: parent
                                spacing: AppTheme.spacing6
                                Avatar {
                                    size: 20
                                    circle: true
                                    mxc: chip.modelData.avatar
                                    name: chip.label
                                    colorKey: chip.modelData.id
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    text: chip.label
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                    Layout.maximumWidth: 280
                                    elide: Label.ElideMiddle
                                }
                                IconButton {
                                    storm: true
                                    objectName: "inviteChipRemove_" + chip.index
                                    implicitWidth: 18; implicitHeight: 18
                                    radius: AppTheme.radiusPill
                                    iconName: "close"
                                    iconSize: 12
                                    Accessible.name: qsTr("Remove %1").arg(chip.label)
                                    onClicked: {
                                        var next = root.selectedUsers.slice()
                                        next.splice(chip.index, 1)
                                        root.selectedUsers = next
                                    }
                                }
                            }
                        }
                    }
                }

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
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: modelData.userId
                                color: AppTheme.userColor(modelData.userId)
                                elide: Label.ElideMiddle
                                font.pixelSize: AppTheme.textBody
                            }
                            Icon {
                                visible: modelData.state === "ok"
                                name: "check"
                                size: 14
                                color: AppTheme.stormSuccess
                            }
                            Label {
                                text: {
                                    if (modelData.state === "ok") return qsTr("Invited")
                                    if (modelData.state === "failed") {
                                        if (modelData.category === "forbidden")
                                            return qsTr("Not permitted")
                                        if (modelData.category === "rate_limited")
                                            return qsTr("Rate limited")
                                        if (modelData.category === "timeout")
                                            return qsTr("Not confirmed")
                                        return qsTr("Failed")
                                    }
                                    return qsTr("Pending…")
                                }
                                color: modelData.state === "ok" ? AppTheme.stormSuccess
                                     : modelData.state === "failed" ? AppTheme.stormDanger
                                     : AppTheme.stormTextMuted
                                font.pixelSize: AppTheme.textBody
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Item { Layout.fillWidth: true }
            AppButton {
                storm: true
                objectName: "invitePeopleCancelButton"
                visible: !root.batchDone
                text: qsTr("Cancel")
                Accessible.name: qsTr("Cancel")
                onClicked: root.close()
            }
            AppButton {
                objectName: "invitePeopleSubmitButton"
                storm: true
                kind: "primary"
                iconName: "send"
                busy: app.conversations.busy
                visible: !root.batchDone
                enabled: root.selectedUsers.length > 0 && !app.conversations.busy
                text: root.selectedUsers.length > 1
                      ? qsTr("Invite %n people", "", root.selectedUsers.length)
                      : qsTr("Invite")
                onClicked: app.conversations.inviteUsers(
                    root.roomId, root.selectedUsers.map(function(u) { return u.id }))
            }
            AppButton {
                storm: true
                objectName: "invitePeopleDoneButton"
                kind: "primary"
                visible: root.batchDone
                text: qsTr("Done")
                Accessible.name: qsTr("Done")
                onClicked: root.close()
            }
        }
    }
}
