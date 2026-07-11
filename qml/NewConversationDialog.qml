import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.5.9: Start a Direct Message (Phase 8) or create a room (Phase 9).
//
// DM flow: search → select user → existing joined DMs (from the SDK's
// m.direct projection) are offered for reuse before a new encrypted DM can
// be created. Room flow: name/topic, private/public, encryption (default ON
// for private, with an explicit warning when disabled), optional alias for
// public rooms, initial invites, optional placement into the active Space.
// All server operations run through ConversationController; duplicate
// submissions are blocked by its single-flight `busy` state.
Dialog {
    id: root
    modal: true
    title: qsTr("New conversation")
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(520, parent ? parent.width - AppTheme.spacing24 * 2 : 520)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    // "dm" | "room"
    property string mode: "dm"
    // DM selection state.
    property string selectedUserId: ""
    property string selectedDisplayName: ""
    // Room invite selection state.
    property var roomInvites: []

    function openDialog() {
        resetAll()
        open()
        dmPicker.focusSearch()
    }

    function resetAll() {
        mode = "dm"
        selectedUserId = ""
        selectedDisplayName = ""
        roomInvites = []
        roomName.text = ""
        roomTopic.text = ""
        roomAlias.text = ""
        publicRoom.checked = false
        encryptRoom.checked = true
        addToSpace.checked = false
        dmPicker.clear()
        roomPicker.clear()
        app.conversations.reset()
    }

    onClosed: resetAll()

    Connections {
        target: app.conversations
        function onConversationReady(roomId) {
            if (root.opened)
                root.close()
        }
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        radius: AppTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        // Mode switch
        RowLayout {
            spacing: AppTheme.spacing8
            Button {
                text: qsTr("Direct Message")
                checkable: true
                checked: root.mode === "dm"
                onClicked: root.mode = "dm"
                Accessible.name: qsTr("Start a direct message")
            }
            Button {
                text: qsTr("New Room")
                checkable: true
                checked: root.mode === "room"
                onClicked: root.mode = "room"
                Accessible.name: qsTr("Create a new room")
            }
            Item { Layout.fillWidth: true }
        }

        Label {
            visible: app.conversations.errorMessage.length > 0
            Layout.fillWidth: true
            text: app.conversations.errorMessage
            color: AppTheme.danger
            wrapMode: Text.WordWrap
            font.pixelSize: AppTheme.fontSizeS
        }

        // ── Direct Message ────────────────────────────────────────────────
        ColumnLayout {
            visible: root.mode === "dm"
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            UserPicker {
                id: dmPicker
                Layout.fillWidth: true
                visible: root.selectedUserId === ""
                onUserSelected: (userId, displayName) => {
                    root.selectedUserId = userId
                    root.selectedDisplayName = displayName
                    app.conversations.checkExistingDm(userId)
                }
            }

            // Selected user + existing-DM reuse offer.
            ColumnLayout {
                visible: root.selectedUserId !== ""
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing8
                    Label {
                        Layout.fillWidth: true
                        text: root.selectedDisplayName.length > 0
                              ? qsTr("%1 (%2)").arg(root.selectedDisplayName)
                                               .arg(root.selectedUserId)
                              : root.selectedUserId
                        color: AppTheme.textPrimary
                        elide: Label.ElideMiddle
                        font.pixelSize: AppTheme.fontSizeM
                    }
                    ToolButton {
                        text: "✕"
                        Accessible.name: qsTr("Choose a different user")
                        onClicked: {
                            root.selectedUserId = ""
                            root.selectedDisplayName = ""
                            dmPicker.clear()
                            dmPicker.focusSearch()
                        }
                    }
                }

                Label {
                    visible: app.conversations.existingDms.length > 0
                    text: qsTr("You already share a direct message with this user:")
                    color: AppTheme.textSecondary
                    font.pixelSize: AppTheme.fontSizeS
                }
                Repeater {
                    model: app.conversations.existingDms
                    Button {
                        Layout.fillWidth: true
                        text: modelData.name && modelData.name.length > 0
                              ? qsTr("Open “%1”").arg(modelData.name)
                              : qsTr("Open existing conversation")
                        enabled: !app.conversations.busy
                        onClicked: {
                            app.openRoom(modelData.roomId)
                            root.close()
                        }
                    }
                }

                Button {
                    Layout.fillWidth: true
                    highlighted: true
                    enabled: !app.conversations.busy
                    text: app.conversations.existingDms.length > 0
                          ? qsTr("Start a new encrypted conversation anyway")
                          : qsTr("Start encrypted direct message")
                    onClicked: app.conversations.startDirectMessage(root.selectedUserId)
                }
                Label {
                    text: qsTr("Direct messages are end-to-end encrypted.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSizeXS
                }
            }
        }

        // ── New Room ──────────────────────────────────────────────────────
        ColumnLayout {
            visible: root.mode === "room"
            Layout.fillWidth: true
            spacing: AppTheme.spacing8

            TextField {
                id: roomName
                Layout.fillWidth: true
                placeholderText: qsTr("Room name (required)")
                font.pixelSize: AppTheme.fontSizeM
            }
            TextField {
                id: roomTopic
                Layout.fillWidth: true
                placeholderText: qsTr("Topic (optional)")
                font.pixelSize: AppTheme.fontSizeM
            }
            CheckBox {
                id: publicRoom
                text: qsTr("Public room (anyone can find and join)")
                onCheckedChanged: if (checked) encryptRoom.checked = false
            }
            TextField {
                id: roomAlias
                visible: publicRoom.checked
                Layout.fillWidth: true
                placeholderText: qsTr("Local address, e.g. my-room (optional)")
                font.pixelSize: AppTheme.fontSizeM
            }
            CheckBox {
                id: encryptRoom
                enabled: !publicRoom.checked
                text: qsTr("Enable end-to-end encryption")
            }
            Label {
                visible: !publicRoom.checked && !encryptRoom.checked
                Layout.fillWidth: true
                text: qsTr("Warning: this private room will NOT be encrypted. "
                           + "The server can read every message.")
                color: AppTheme.warning
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.fontSizeS
            }
            Label {
                visible: publicRoom.checked
                Layout.fillWidth: true
                text: qsTr("Public rooms are not end-to-end encrypted.")
                color: AppTheme.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.fontSizeXS
            }

            CheckBox {
                id: addToSpace
                visible: app.spaces && app.spaces.activeSpaceId
                         && app.spaces.activeSpaceId.startsWith("!")
                text: qsTr("Add to the current Space")
            }

            Label {
                text: qsTr("Invite people (optional)")
                color: AppTheme.textSecondary
                font.pixelSize: AppTheme.fontSizeS
            }
            Flow {
                Layout.fillWidth: true
                spacing: AppTheme.spacing4
                visible: root.roomInvites.length > 0
                Repeater {
                    model: root.roomInvites
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
                                text: "✕"
                                Accessible.name: qsTr("Remove %1").arg(modelData)
                                onClicked: {
                                    var next = root.roomInvites.slice()
                                    next.splice(index, 1)
                                    root.roomInvites = next
                                }
                            }
                        }
                    }
                }
            }
            UserPicker {
                id: roomPicker
                Layout.fillWidth: true
                onUserSelected: (userId, displayName) => {
                    if (root.roomInvites.indexOf(userId) === -1) {
                        var next = root.roomInvites.slice()
                        next.push(userId)
                        root.roomInvites = next
                    }
                    roomPicker.clear()
                }
            }

            Button {
                Layout.fillWidth: true
                highlighted: true
                enabled: !app.conversations.busy && roomName.text.trim().length > 0
                text: publicRoom.checked ? qsTr("Create public room")
                                         : (encryptRoom.checked
                                            ? qsTr("Create encrypted room")
                                            : qsTr("Create unencrypted room"))
                onClicked: {
                    app.conversations.createRoom({
                        name: roomName.text,
                        topic: roomTopic.text,
                        "public": publicRoom.checked,
                        encrypted: encryptRoom.checked && !publicRoom.checked,
                        alias: publicRoom.checked ? roomAlias.text : "",
                        invites: root.roomInvites,
                        spaceId: (addToSpace.visible && addToSpace.checked)
                                 ? app.spaces.activeSpaceId : ""
                    })
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
            Label {
                visible: app.conversations.busy
                text: qsTr("Working…")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeS
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Cancel")
                onClicked: root.close()
            }
        }
    }
}
