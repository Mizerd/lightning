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

    // Looks up avatar/name/topic itself (rather than taking a caller-passed
    // snapshot) so it stays live: an avatar that arrives asynchronously
    // after resolveMissingDirectAvatars() completes — or any other room
    // change — refreshes here exactly like the timeline header does,
    // instead of freezing on whatever was known at the moment the panel
    // opened.
    function openForRoom(roomId) {
        app.roomInfo.roomId = roomId
        section = "overview"
        memberFilter = ""
        memberSearch.text = ""
        refreshRoomData()
    }
    function refreshRoomData() {
        roomData = app.roomInfo.roomId !== ""
                   ? app.roomList.findRoom(app.roomInfo.roomId) : ({})
    }
    Connections {
        target: app.roomList
        function onDataChanged() { root.refreshRoomData() }
        function onModelReset() { root.refreshRoomData() }
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

    // Development-only: locate a descendant by objectName across both the
    // visual children (Item-derived) and the default-property data list.
    function findDemoDescendant(obj, name) {
        if (!obj) return null
        if (obj.objectName === name) return obj
        // Dialogs/Popups are not Items: their subtree hangs off contentItem,
        // never children/data — without this branch a Dialog descendant is
        // silently unreachable.
        if (obj.contentItem) {
            var viaContent = findDemoDescendant(obj.contentItem, name)
            if (viaContent) return viaContent
        }
        var kids = obj.children || []
        for (var i = 0; i < kids.length; ++i) {
            var found = findDemoDescendant(kids[i], name)
            if (found) return found
        }
        var data = obj.data || []
        for (var j = 0; j < data.length; ++j) {
            var found2 = findDemoDescendant(data[j], name)
            if (found2) return found2
        }
        return null
    }

    // Development-only: screenshot-demo popup hooks (see
    // ScreenshotDemoController and SpacesRail.qml:accountSwitcherRequested
    // for the pattern this mirrors). Null target / disabled in a non-demo
    // build makes this an inert no-op.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenInvitePeople() {
            inviteDialog.openFor(app.currentRoomId)
            // Seed the token field so real search results (matching Maya
            // Chen) render instead of an empty starting state.
            Qt.callLater(function() {
                var picker = root.findDemoDescendant(inviteDialog, "invitePeoplePicker")
                if (picker)
                    picker.searchText = "ma"
            })
        }
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
                    font.pixelSize: 15
                    font.weight: Font.ExtraBold
                }
                IconButton {
                    implicitWidth: 30; implicitHeight: 30
                    radius: 7
                    iconName: "close"
                    iconSize: 18
                    Accessible.name: qsTr("Close room information")
                    onClicked: root.closeRequested()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // ── Section tabs: one coherent segmented row ─────────────────────
        SegmentedControl {
            objectName: "roomInfoTabs"
            Layout.margins: AppTheme.spacing8
            model: [
                { label: qsTr("Overview"), value: "overview" },
                { label: qsTr("People"), value: "people" },
                { label: qsTr("Media"), value: "media" },
            ]
            current: root.section
            onActivated: (value) => root.section = value
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

                // v0.6.0 checkpoint 11: LOCAL per-room notification mode.
                // Explicitly this-device-only — never presented as a server
                // push rule.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12
                    Layout.rightMargin: AppTheme.spacing12
                    Layout.topMargin: AppTheme.spacing12
                    spacing: 4
                    Label {
                        text: qsTr("Notifications (this device)")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.fontSecondary
                        font.weight: Font.DemiBold
                    }
                    AppComboBox {
                        objectName: "roomNotificationModeCombo"
                        Layout.fillWidth: true
                        model: [
                            qsTr("All messages"),
                            qsTr("Mentions only"),
                            qsTr("Mute")
                        ]
                        currentIndex: app.roomInfo.roomId !== ""
                            ? app.settings.roomNotificationMode(
                                  app.roomInfo.roomId)
                            : 0
                        onActivated: (index) => {
                            if (app.roomInfo.roomId !== "")
                                app.settings.setRoomNotificationMode(
                                    app.roomInfo.roomId, index)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.fontCaption
                        text: qsTr("Local setting: it does not change this "
                                   + "room's server push rules.")
                    }
                }

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
                            circle: root.roomData.isDirect === true
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
                            RowLayout {
                                visible: root.roomData.encrypted === true
                                spacing: 4
                                Icon {
                                    name: "verified_user"
                                    size: 14
                                    color: AppTheme.accent
                                }
                                Label {
                                    text: qsTr("End-to-end encrypted")
                                    color: AppTheme.textSecondary
                                    font.pixelSize: AppTheme.fontSizeS
                                    font.weight: Font.DemiBold
                                }
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

                    AppButton {
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
                        AppButton {
                            text: qsTr("Change avatar…")
                            enabled: !app.roomInfo.editPending
                            onClicked: avatarDialog.open()
                        }
                        AppButton {
                            kind: "danger"
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
                        AppTextField {
                            id: editName
                            Layout.fillWidth: true
                            placeholderText: qsTr("Room name")
                            text: root.roomData.name || ""
                        }
                        AppButton {
                            kind: "primary"
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
                        AppTextField {
                            id: editTopic
                            Layout.fillWidth: true
                            placeholderText: qsTr("Topic")
                            text: root.roomData.topic || ""
                        }
                        AppButton {
                            kind: "primary"
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

                    AppButton {
                        kind: "danger"
                        text: qsTr("Leave room")
                        enabled: !app.roomInfo.leavePending
                        onClicked: leaveConfirm.open()
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
                AppTextField {
                    id: memberSearch
                    Layout.fillWidth: true
                    searchIcon: true
                    clearButton: true
                    placeholderText: qsTr("Search members…")
                    onTextChanged: root.memberFilter = text
                }
                AppButton {
                    kind: "primary"
                    visible: app.roomInfo.canInvite
                    text: qsTr("Invite")
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
                        Avatar {
                            width: 32; height: 32
                            size: 32
                            name: modelData.displayName.length > 0
                                  ? modelData.displayName : modelData.userId
                            mxc: modelData.avatarUrl || ""
                            colorKey: modelData.userId || ""
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
                        Icon {
                            name: modelData.isImage ? "image" : "attach_file"
                            size: 16
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
                        AppButton {
                            visible: (modelData.mediaKey || "").length > 0
                                     && app.mediaBridge.supported
                            implicitHeight: 26
                            leftPadding: 10
                            rightPadding: 10
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
        // An explicit viewport-bounded width keeps Dialog implicit sizing
        // independent from the wrapping label/layout inside it.
        width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))
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
                text: qsTr("You will stop receiving messages from this room. "
                           + "Server history is not deleted, and you can be "
                           + "invited again later.")
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Cancel")
                    focus: true
                    onClicked: leaveConfirm.close()
                }
                AppButton {
                    kind: "danger"
                    text: qsTr("Leave room")
                    onClicked: {
                        leaveConfirm.close()
                        app.roomInfo.leaveRoom()
                    }
                }
            }
        }
    }
}
