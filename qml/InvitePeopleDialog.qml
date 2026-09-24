import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Invite users into a joined room. Opened from Room Information's People tab,
// only when the SDK permission (RoomMember::can_invite) allows. Selected users
// are deduplicated and shown as chips ({id, name, avatar}); results are shown
// per user (pending/ok/failed) and one failure never discards the others.
// inviteUsers() dispatches a plain user-id list.
Dialog {
    id: root
    objectName: "invitePeopleDialog"
    modal: true
    // The shared modal scrim, not the Basic style's default dim.
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(480, parent ? parent.width - AppTheme.spacing24 * 2 : 480)
    // Clamp to the window and let the body scroll, so a long invite list can't
    // push the title and footer off screen.
    height: Math.min(implicitHeight,
                     parent ? parent.height - AppTheme.spacing24 * 2 : implicitHeight)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property string roomId: ""
    property var selectedUsers: [] // [{id, name, avatar}]
    property bool batchDone: false

    // The room being invited to, from the live room-list record at open time:
    // name in the title, address below. Falls back to a generic title; nothing
    // is fabricated.
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
            // Membership updates arrive via sync; Room Information refreshes
            // from membersChanged.
        }
    }

    background: Rectangle {
        color: AppTheme.stormPanel
        border.color: AppTheme.stormBorder
        radius: AppTheme.radiusLg
    }

    // True when the invitee is already joined/invited per the loaded member
    // snapshot (best-effort; the server decides).
    function membershipOf(userId) {
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

    // A primary button with a send icon; AppButton has no icon slot.
    component InvitePrimaryButton: AbstractButton {
        id: primaryBtn
        // Real padding, not a widened implicitWidth: the contentItem is
        // stretched to the button width, so extra width alone pins the content
        // to the left edge.
        leftPadding: 14
        rightPadding: 14
        implicitHeight: 32
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.Button
        Accessible.name: primaryBtn.text
        // Bolt fill with boltInk, readable on every theme's accent.
        contentItem: RowLayout {
            id: primaryContent
            spacing: AppTheme.spacing6
            Icon {
                name: "send"
                size: 15
                color: primaryBtn.enabled ? AppTheme.boltInk
                                          : AppTheme.stormTextFaint
            }
            Label {
                text: primaryBtn.text
                color: primaryBtn.enabled ? AppTheme.boltInk
                                          : AppTheme.stormTextFaint
                font.family: AppTheme.menuFont
                font.pixelSize: 13
                font.weight: AppTheme.weightBold
            }
        }
        background: Rectangle {
            radius: AppTheme.radiusMd
            color: !primaryBtn.enabled ? AppTheme.stormInset
                   : primaryBtn.down ? Qt.darker(AppTheme.bolt, 1.12)
                   : primaryBtn.hovered ? Qt.darker(AppTheme.bolt, 1.05)
                   : AppTheme.bolt
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: AppTheme.radiusMd + 4
            color: "transparent"
            border.width: 2
            border.color: AppTheme.bolt
            visible: primaryBtn.visualFocus
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        // Themed header instead of Dialog.title, which Basic renders as its own
        // unthemed bar.
        Label {
            // Remote or externally chosen text: never markup.
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

        // Room address under the title, only when real.
        Label {
            // Remote or externally chosen text: never markup.
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
            // Remote or externally chosen text: never markup.
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

        // ── Token chips + per-user results (scrolls when height-constrained,
        // like NewConversationDialog's tabFlick) ──
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
            // Same wheel/touchpad feel as the room timeline; see
            // qml/SmoothWheelArea.qml.
            SmoothWheelArea {}

            ColumnLayout {
                id: inviteBodyColumn
                width: inviteBodyFlick.width
                spacing: AppTheme.spacing12

                // Selected users before dispatch, as token chips.
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
                                    // Remote or externally chosen text: never
                                    // markup.
                                    textFormat: Text.PlainText
                                    text: chip.label
                                    color: AppTheme.stormText
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                    // A Flow wraps between chips, never inside
                                    // one, so cap a long name/MXID to keep a
                                    // chip within the dialog.
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
                                    // Muted rest-state ink for the remove
                                    // glyph.
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
                                // Remote or externally chosen text: never
                                // markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: modelData.userId
                                // Identity ink, so a specific MXID is easy to
                                // pick out.
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
            AppBusyIndicator {
                visible: app.conversations.busy
                running: visible
                size: 20
                color: AppTheme.bolt
            }
            Item { Layout.fillWidth: true }
            AppButton {
                storm: true
                objectName: "invitePeopleCancelButton"
                visible: !root.batchDone
                text: qsTr("Cancel")
                Accessible.name: qsTr("Cancel")
                onClicked: root.close()
            }
            InvitePrimaryButton {
                objectName: "invitePeopleSubmitButton"
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
