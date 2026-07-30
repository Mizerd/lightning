import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.9 (Phase 10): invite users into an existing joined room. Opened from
// the Room Information People tab, and only when the SDK-derived
// permission (RoomMember::can_invite) allows it. Selected users are
// deduplicated; per-user pending/ok/failed state is shown; a failure for
// one user never discards the others' results.
//
// v0.6.5 (SPEC 1t): token-field chip restyle over the exact same flow —
// selection/dedup, the already-member pre-check, and the per-user
// pending/ok/failed results are unchanged. selectedUsers now carries
// {id, name, avatar} so a chip can show a small Avatar; inviteUsers() still
// dispatches a plain user-id list.
Dialog {
    id: root
    objectName: "invitePeopleDialog"
    modal: true
    title: roomDisplayName.length > 0
           ? qsTr("Invite to %1").arg(roomDisplayName)
           : qsTr("Invite people")
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(480, parent ? parent.width - AppTheme.spacing24 * 2 : 480)
    // Clamp to the window and let the chip/result body scroll — an unbounded
    // invite list otherwise grows the dialog past the overlay, pushing the
    // title and the footer buttons off screen.
    height: Math.min(implicitHeight,
                     parent ? parent.height - AppTheme.spacing24 * 2 : implicitHeight)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    property string roomId: ""
    property var selectedUsers: [] // [{id, name, avatar}]
    property bool batchDone: false

    // v0.6.5 (SPEC 1t): the room being invited to, resolved from the live
    // room-list record at open time — name in the title, mono address line
    // below it. Falls back to the plain generic title when the record is
    // unavailable; nothing is fabricated.
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

    function selectedIndexOf(userId) {
        for (var i = 0; i < selectedUsers.length; ++i) {
            if (selectedUsers[i].id === userId) return i
        }
        return -1
    }

    // Local "primary with send icon" button (SPEC 1t footer): AppButton has
    // no icon slot, so this is a one-off inline component rather than an
    // edit to the shared (lead-owned) AppButton.qml.
    component InvitePrimaryButton: AbstractButton {
        id: primaryBtn
        // Real padding, not a widened implicitWidth: an AbstractButton
        // stretches its contentItem to the full control width, so extra
        // width without padding pins the icon+label to the button's left
        // edge instead of centring them on the accent fill.
        leftPadding: 14
        rightPadding: 14
        implicitHeight: 32
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.Button
        Accessible.name: primaryBtn.text
        contentItem: RowLayout {
            id: primaryContent
            spacing: AppTheme.spacing6
            Icon {
                name: "send"
                size: 15
                color: primaryBtn.enabled ? AppTheme.accentText : AppTheme.textDisabled
            }
            Label {
                text: primaryBtn.text
                color: primaryBtn.enabled ? AppTheme.accentText : AppTheme.textDisabled
                font.pixelSize: 13
                font.weight: Font.Bold
            }
        }
        background: Rectangle {
            radius: AppTheme.radiusMd
            color: !primaryBtn.enabled ? AppTheme.cardElevated
                   : primaryBtn.down ? AppTheme.accentPressed
                   : primaryBtn.hovered ? AppTheme.accentHover
                   : AppTheme.accent
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: AppTheme.radiusMd + 4
            color: "transparent"
            border.width: 2
            border.color: AppTheme.focusRing
            visible: primaryBtn.visualFocus
        }
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        // Mono room address under the title (SPEC 1t), only when real.
        Label {
            visible: root.roomAddress.length > 0
            Layout.fillWidth: true
            text: root.roomAddress
            color: AppTheme.monoIdentityColor
            font.family: AppTheme.monoFont
            font.pixelSize: AppTheme.fontMonoXS
            elide: Label.ElideRight
        }

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
            objectName: "invitePeoplePicker"
            Layout.fillWidth: true
            visible: !root.batchDone
            onUserSelected: (userId, displayName, avatarUrl) => {
                var membership = root.membershipOf(userId)
                if (membership === "joined" || membership === "invited") {
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

        // ── Token chips + per-user results (scrolls when height-
        // constrained; mirrors NewConversationDialog's tabFlick so a long
        // invite list can never grow the dialog past the window) ─────────
        Flickable {
            id: inviteBodyFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: inviteBodyColumn.implicitHeight
            contentWidth: width
            contentHeight: inviteBodyColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: inviteBodyColumn
                width: inviteBodyFlick.width
                spacing: AppTheme.spacing12

                // Selected users before dispatch — token chips (SPEC 1t):
                // pill, accentSoft bg, 1px accentBorder, 20px Avatar,
                // textMuted remove.
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
                            color: AppTheme.accentSoft
                            border.width: 1
                            border.color: AppTheme.accentBorder
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
                                    text: chip.label
                                    color: AppTheme.textPrimary
                                    font.pixelSize: AppTheme.fontSizeS
                                    font.weight: Font.DemiBold
                                    // A Flow wraps BETWEEN chips, never
                                    // inside one — cap a single long name/
                                    // MXID so one chip can never outgrow
                                    // the dialog card.
                                    Layout.maximumWidth: 280
                                    elide: Label.ElideMiddle
                                }
                                IconButton {
                                    objectName: "inviteChipRemove_" + chip.index
                                    implicitWidth: 18; implicitHeight: 18
                                    radius: AppTheme.radiusPill
                                    iconName: "close"
                                    iconSize: 12
                                    // Default rest-state ink (AppTheme.icon =
                                    // textMuted) already matches SPEC 1t.
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
                                Layout.fillWidth: true
                                text: modelData.userId
                                color: AppTheme.textPrimary
                                elide: Label.ElideMiddle
                                font.pixelSize: AppTheme.fontSizeS
                            }
                            Icon {
                                visible: modelData.state === "ok"
                                name: "check"
                                size: 14
                                color: AppTheme.success
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
                                color: modelData.state === "ok" ? AppTheme.success
                                     : modelData.state === "failed" ? AppTheme.danger
                                     : AppTheme.textMuted
                                font.pixelSize: AppTheme.fontSizeS
                            }
                        }
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
            AppButton {
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
