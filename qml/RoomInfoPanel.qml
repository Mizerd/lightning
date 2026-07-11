import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// v0.5.9: right-side Room Information panel (Phase 6 surface for the
// Phase 10 invite entry point). Overview: identity, encryption state,
// permission-gated name/topic editing, Leave room with confirmation.
// People: member search, joined/invited state, roles, Invite button when
// the SDK says the user may invite. Member data is a bounded in-memory
// snapshot from RoomInfoController; nothing here is persisted.
Rectangle {
    id: root
    color: AppTheme.sidebar
    visible: width > 0

    property var roomData: ({})
    signal closeRequested()
    // v0.5.9: Media & Files entries delegate viewing/saving to the pane
    // that owns the viewer and the save dialog (TimelinePane).
    signal openImageRequested(string mediaKey, var httpUrl)
    signal saveMediaRequested(string mediaKey, string filename)

    // "overview" | "people" | "media"
    property string section: "overview"
    property string memberFilter: ""

    function openForRoom(roomId, data) {
        roomData = data || {}
        section = "overview"
        memberFilter = ""
        memberSearch.text = ""
        app.roomInfo.roomId = roomId
    }

    InvitePeopleDialog {
        id: inviteDialog
        parent: Overlay.overlay
    }

    MemberProfilePopover {
        id: memberProfile
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    FileDialog {
        id: avatarDialog
        title: qsTr("Choose room avatar")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)") ]
        onAccepted: app.roomInfo.setRoomAvatar(selectedFile)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headerRow.implicitHeight + AppTheme.spacing12 * 2
            color: AppTheme.surface
            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Room information")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.fontSizeRoom
                    font.weight: Font.DemiBold
                }
                ToolButton {
                    text: "✕"
                    Accessible.name: qsTr("Close room information")
                    onClicked: root.closeRequested()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // ── Section tabs ─────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: AppTheme.spacing8
            spacing: AppTheme.spacing8
            Button {
                text: qsTr("Overview")
                checkable: true
                checked: root.section === "overview"
                onClicked: root.section = "overview"
            }
            Button {
                text: qsTr("People")
                checkable: true
                checked: root.section === "people"
                onClicked: root.section = "people"
            }
            Button {
                text: qsTr("Media")
                checkable: true
                checked: root.section === "media"
                onClicked: root.section = "media"
            }
            Item { Layout.fillWidth: true }
        }

        // ── Overview ─────────────────────────────────────────────────────
        ScrollView {
            visible: root.section === "overview"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: parent.width
                spacing: AppTheme.spacing12

                // Identity block
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    RowLayout {
                        spacing: AppTheme.spacing12
                        Avatar {
                            size: 56
                            name: root.roomData.name || ""
                            mxc: root.roomData.avatarUrl || ""
                            circle: !(root.roomData.isSpace === true)
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: root.roomData.name || qsTr("(unnamed room)")
                                color: AppTheme.textPrimary
                                font.pixelSize: AppTheme.fontSizeRoom
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                            }
                            Label {
                                visible: root.roomData.encrypted === true
                                text: qsTr("🔒 End-to-end encrypted")
                                color: AppTheme.success
                                font.pixelSize: AppTheme.fontSizeS
                            }
                            Label {
                                visible: root.roomData.encrypted !== true
                                text: qsTr("Not encrypted")
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.fontSizeS
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: (root.roomData.topic || "").length > 0
                        text: root.roomData.topic || ""
                        color: AppTheme.textSecondary
                        wrapMode: Text.Wrap
                        font.pixelSize: AppTheme.fontSizeS
                    }

                    Label {
                        text: qsTr("%1 members (%2 invited)")
                              .arg(app.roomInfo.joinedCount)
                              .arg(app.roomInfo.invitedCount)
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.fontSizeS
                    }

                    Button {
                        text: qsTr("Copy room ID")
                        onClicked: {
                            copyHelper.text = app.roomInfo.roomId
                            copyHelper.selectAll()
                            copyHelper.copy()
                        }
                        ToolTip.text: app.roomInfo.roomId
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                    }
                    // Hidden helper for clipboard copy without C++ additions.
                    TextEdit {
                        id: copyHelper
                        visible: false
                        width: 0; height: 0
                    }
                }

                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

                // Permission-gated editing (name / topic).
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    visible: app.roomInfo.canEditName || app.roomInfo.canEditTopic
                             || app.roomInfo.canEditAvatar

                    Label {
                        text: qsTr("Edit room")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.fontSizeS
                        font.weight: Font.DemiBold
                    }
                    RowLayout {
                        visible: app.roomInfo.canEditAvatar
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Button {
                            text: qsTr("Change avatar…")
                            enabled: !app.roomInfo.editPending
                            onClicked: avatarDialog.open()
                        }
                        Button {
                            text: qsTr("Remove avatar")
                            enabled: !app.roomInfo.editPending
                            onClicked: app.roomInfo.removeRoomAvatar()
                        }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        visible: app.roomInfo.canEditName
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        TextField {
                            id: editName
                            Layout.fillWidth: true
                            placeholderText: qsTr("Room name")
                            text: root.roomData.name || ""
                            font.pixelSize: AppTheme.fontSizeM
                        }
                        Button {
                            text: qsTr("Save")
                            enabled: !app.roomInfo.editPending
                                     && editName.text.trim().length > 0
                                     && editName.text !== (root.roomData.name || "")
                            onClicked: app.roomInfo.setRoomName(editName.text)
                        }
                    }
                    RowLayout {
                        visible: app.roomInfo.canEditTopic
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        TextField {
                            id: editTopic
                            Layout.fillWidth: true
                            placeholderText: qsTr("Topic")
                            text: root.roomData.topic || ""
                            font.pixelSize: AppTheme.fontSizeM
                        }
                        Button {
                            text: qsTr("Save")
                            enabled: !app.roomInfo.editPending
                                     && editTopic.text !== (root.roomData.topic || "")
                            onClicked: app.roomInfo.setRoomTopic(editTopic.text)
                        }
                    }
                    Label {
                        visible: app.roomInfo.editError.length > 0
                        Layout.fillWidth: true
                        text: app.roomInfo.editError
                        color: AppTheme.danger
                        wrapMode: Text.WordWrap
                        font.pixelSize: AppTheme.fontSizeS
                    }
                    BusyIndicator {
                        visible: app.roomInfo.editPending
                        running: visible
                        implicitWidth: 18; implicitHeight: 18
                    }
                }

                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

                // Leave room.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    Button {
                        text: qsTr("Leave room")
                        enabled: !app.roomInfo.leavePending
                        onClicked: leaveConfirm.open()
                        contentItem: Label {
                            text: parent.text
                            color: AppTheme.danger
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                    Label {
                        visible: app.roomInfo.leaveError.length > 0
                        Layout.fillWidth: true
                        text: app.roomInfo.leaveError
                        color: AppTheme.danger
                        wrapMode: Text.WordWrap
                        font.pixelSize: AppTheme.fontSizeS
                    }
                }
                Item { Layout.preferredHeight: AppTheme.spacing16 }
            }
        }

        // ── People ───────────────────────────────────────────────────────
        ColumnLayout {
            visible: root.section === "people"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: AppTheme.spacing8
                spacing: AppTheme.spacing8
                TextField {
                    id: memberSearch
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search members…")
                    font.pixelSize: AppTheme.fontSizeS
                    onTextChanged: root.memberFilter = text
                }
                Button {
                    visible: app.roomInfo.canInvite
                    text: qsTr("Invite")
                    highlighted: true
                    Accessible.name: qsTr("Invite people to this room")
                    onClicked: inviteDialog.openFor(app.roomInfo.roomId)
                }
            }

            Label {
                visible: app.roomInfo.loading
                Layout.leftMargin: AppTheme.spacing12
                text: qsTr("Loading members…")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeS
            }
            Label {
                visible: app.roomInfo.truncated
                Layout.leftMargin: AppTheme.spacing12
                Layout.fillWidth: true
                text: qsTr("Showing the first %1 members of %2.")
                      .arg(app.roomInfo.members.length)
                      .arg(app.roomInfo.joinedCount + app.roomInfo.invitedCount)
                color: AppTheme.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.fontSizeXS
            }

            ListView {
                id: memberList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                // Reading `members` makes this binding re-evaluate when the
                // snapshot updates (invites landing, refreshes), not only
                // when the filter text changes.
                model: {
                    var snapshot = app.roomInfo.members // dependency only
                    return root.memberFilter.length > 0
                            ? app.roomInfo.filterMembers(root.memberFilter)
                            : snapshot
                }
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: ItemDelegate {
                    width: ListView.view.width
                    Accessible.name: modelData.displayName.length > 0
                                     ? qsTr("%1 (%2)").arg(modelData.displayName)
                                                      .arg(modelData.userId)
                                     : modelData.userId
                    onClicked: memberProfile.openFor(modelData)
                    contentItem: RowLayout {
                        spacing: AppTheme.spacing8
                        Rectangle {
                            width: 32; height: 32
                            radius: AppTheme.radiusPill
                            color: AppTheme.cardElevated
                            Label {
                                anchors.centerIn: parent
                                text: {
                                    var n = modelData.displayName.length > 0
                                            ? modelData.displayName
                                            : modelData.userId.slice(1)
                                    return n.length > 0 ? n[0].toUpperCase() : "?"
                                }
                                color: AppTheme.textSecondary
                                font.pixelSize: AppTheme.fontSizeM
                                font.weight: Font.DemiBold
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing4
                                Label {
                                    text: modelData.displayName.length > 0
                                          ? modelData.displayName
                                          : modelData.userId
                                    color: AppTheme.textPrimary
                                    font.pixelSize: AppTheme.fontSizeM
                                    elide: Label.ElideRight
                                }
                                Label {
                                    visible: modelData.ambiguous === true
                                             && modelData.displayName.length > 0
                                    text: modelData.userId
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.fontSizeXS
                                    elide: Label.ElideMiddle
                                    Layout.fillWidth: true
                                }
                            }
                            Label {
                                visible: modelData.displayName.length > 0
                                         && modelData.ambiguous !== true
                                text: modelData.userId
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.fontSizeXS
                                elide: Label.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                        Label {
                            visible: modelData.membership === "invited"
                            text: qsTr("Invited")
                            color: AppTheme.warning
                            font.pixelSize: AppTheme.fontSizeXS
                        }
                        Label {
                            visible: modelData.role === "administrator"
                                     || modelData.role === "creator"
                            text: qsTr("Admin")
                            color: AppTheme.accent
                            font.pixelSize: AppTheme.fontSizeXS
                        }
                        Label {
                            visible: modelData.role === "moderator"
                            text: qsTr("Mod")
                            color: AppTheme.accent
                            font.pixelSize: AppTheme.fontSizeXS
                        }
                    }
                    ToolTip.text: modelData.userId
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                }
            }
        }

        // ── Media & Files ────────────────────────────────────────────────
        ColumnLayout {
            visible: root.section === "media"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            Label {
                Layout.margins: AppTheme.spacing12
                Layout.fillWidth: true
                text: qsTr("Media and files shared in the loaded part of "
                           + "this conversation. Scroll the timeline up to "
                           + "load more history.")
                color: AppTheme.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.fontCaption
            }

            ListView {
                id: mediaList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                // Newest first; bound to the loaded timeline (count keeps
                // the binding fresh as history paginates in).
                model: {
                    var deps = app.timeline.count
                    return app.timeline.mediaEntries().slice().reverse()
                }
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                Label {
                    anchors.centerIn: parent
                    visible: mediaList.count === 0
                    text: qsTr("No media in the loaded history.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSecondary
                }

                delegate: ItemDelegate {
                    width: ListView.view.width
                    Accessible.name: modelData.filename
                    onClicked: {
                        if (modelData.isImage)
                            root.openImageRequested(modelData.mediaKey || "",
                                                    modelData.httpUrl)
                    }
                    contentItem: RowLayout {
                        spacing: AppTheme.spacing8
                        Label {
                            text: modelData.isImage ? "🖼" : "📎"
                            font.pixelSize: AppTheme.fontRoomTitle
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                text: modelData.filename || qsTr("(unnamed)")
                                color: AppTheme.textPrimary
                                font.pixelSize: AppTheme.fontSecondary
                                elide: Label.ElideMiddle
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%1 · %2")
                                      .arg(modelData.sender)
                                      .arg(Qt.formatDateTime(modelData.timestamp,
                                                             "d MMM hh:mm"))
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.fontCaption
                                elide: Label.ElideRight
                            }
                        }
                        ToolButton {
                            visible: (modelData.mediaKey || "").length > 0
                                     && app.mediaBridge.supported
                            text: qsTr("Save")
                            Accessible.name: qsTr("Save %1 as…").arg(modelData.filename)
                            onClicked: root.saveMediaRequested(modelData.mediaKey,
                                                               modelData.filename || "download")
                        }
                    }
                }
            }
        }
    }

    // Leave confirmation — Cancel is the default safe action.
    Dialog {
        id: leaveConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Leave room?")
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusLg
        }

        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                Layout.maximumWidth: 360
                text: qsTr("You will stop receiving messages from this room. "
                           + "Server history is not deleted, and you can be "
                           + "invited again later.")
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: leaveConfirm.close()
                }
                Button {
                    text: qsTr("Leave room")
                    contentItem: Label {
                        text: parent.text
                        color: AppTheme.danger
                        horizontalAlignment: Text.AlignHCenter
                    }
                    onClicked: {
                        leaveConfirm.close()
                        app.roomInfo.leaveRoom()
                    }
                }
            }
        }
    }
}
