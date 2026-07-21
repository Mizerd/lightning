import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects
import QtQuick.Layouts
import MatrixClient

// Creation UX: one Lightning modal card for starting a Direct Message,
// creating a room, or creating a Space.
//
// DM flow: search → select user → existing joined DMs (from the SDK's
// m.direct projection) are offered for reuse FIRST; starting a new encrypted
// DM is an explicit, clearly-secondary action when one already exists.
// Room flow: name/topic, Private/Public visibility, encryption (default ON
// for private, forced off and disabled for public), optional alias for
// public rooms, invites, optional placement into the active Space, and an
// OPTIONAL picture applied after creation (a failed picture upload is a
// warning, never a failed create). Space flow: name/topic/visibility only —
// no encryption control is instantiated at all.
//
// Tabs are Loaders: only the active tab's controls exist, form state lives
// on the dialog root so switching tabs never loses input, and the shared
// UserSearchModel is cleared on every switch (one visible picker at a time).
// All server operations run through ConversationController; duplicate
// submissions are blocked by its single-flight `busy` state.
Dialog {
    id: root
    objectName: "newConversationDialog"
    modal: true
    focus: true
    standardButtons: Dialog.NoButton
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(560, parent ? parent.width - AppTheme.spacing24 * 2 : 560)
    height: Math.min(implicitHeight,
                     parent ? parent.height - AppTheme.spacing24 * 2 : implicitHeight)
    anchors.centerIn: parent
    padding: AppTheme.spacing16

    // "dm" | "room" | "space"
    property string mode: "dm"

    // DM selection state.
    property string selectedUserId: ""
    property string selectedDisplayName: ""
    property string selectedAvatarUrl: ""

    // Room form state (root-owned so the tab Loaders can unload freely).
    property string roomNameText: ""
    property bool roomNameEdited: false
    property string roomTopicText: ""
    property string roomAliasText: ""
    property bool roomIsPublic: false
    property bool roomEncrypted: true
    property bool addToSpaceChecked: false
    property var roomInvites: []
    // Local file URL chosen for the optional room picture ("" = none).
    property string roomAvatarPath: ""

    // Space form state.
    property string spaceNameText: ""
    property bool spaceNameEdited: false
    property string spaceTopicText: ""
    property bool spaceIsPublic: false

    // Set while a Space create is in flight so the new Space is selected in
    // the rail (not opened as a timeline) when it arrives.
    property bool pendingIsSpace: false

    readonly property bool busy: app.conversations.busy
    readonly property bool canCreateRoom: !busy && roomNameText.trim().length > 0
    readonly property bool canCreateSpace: !busy && spaceNameText.trim().length > 0
    readonly property bool spacePlacementAvailable:
        app.spaces && app.spaces.activeSpaceId
        && app.spaces.activeSpaceId.startsWith("!")

    // startMode ("dm" | "room" | "space") lets callers (e.g. the Home
    // surface) open straight into a tab; defaults to a DM search.
    function openDialog(startMode) {
        resetAll()
        if (startMode === "room" || startMode === "dm" || startMode === "space")
            mode = startMode
        open()
    }

    function resetAll() {
        mode = "dm"
        selectedUserId = ""
        selectedDisplayName = ""
        selectedAvatarUrl = ""
        roomNameText = ""
        roomNameEdited = false
        roomTopicText = ""
        roomAliasText = ""
        roomIsPublic = false
        roomEncrypted = true
        addToSpaceChecked = false
        roomInvites = []
        roomAvatarPath = ""
        spaceNameText = ""
        spaceNameEdited = false
        spaceTopicText = ""
        spaceIsPublic = false
        pendingIsSpace = false
        app.conversations.reset()
        app.conversations.userSearch.clear()
    }

    // Every tab switch clears the SHARED search model state and hands focus
    // to the new tab's first field (via the Loader reload below).
    function switchMode(newMode) {
        if (newMode !== "dm" && newMode !== "room" && newMode !== "space")
            return
        if (newMode === mode)
            return
        app.conversations.userSearch.clear()
        app.conversations.clearError()
        mode = newMode
    }

    function activeTabItem() {
        if (mode === "room") return roomTabLoader.item
        if (mode === "space") return spaceTabLoader.item
        return dmTabLoader.item
    }
    function focusCurrentTab() {
        var tab = activeTabItem()
        if (tab)
            tab.focusFirst()
    }

    function submitRoom() {
        if (!canCreateRoom)
            return
        pendingIsSpace = false
        app.conversations.createRoom({
            name: roomNameText,
            topic: roomTopicText,
            "public": roomIsPublic,
            encrypted: roomEncrypted && !roomIsPublic,
            alias: roomIsPublic ? roomAliasText : "",
            invites: roomInvites,
            spaceId: (spacePlacementAvailable && addToSpaceChecked)
                     ? app.spaces.activeSpaceId : "",
            avatarPath: roomAvatarPath
        })
    }

    function submitSpace() {
        if (!canCreateSpace)
            return
        pendingIsSpace = true
        app.conversations.createRoom({
            name: spaceNameText,
            topic: spaceTopicText,
            "public": spaceIsPublic,
            encrypted: false,
            isSpace: true
        })
    }

    onClosed: resetAll()
    onOpened: Qt.callLater(focusCurrentTab)

    Connections {
        target: app.conversations
        function onConversationReady(roomId) {
            // A newly created Space is selected in the rail rather than opened
            // as a timeline (it has no message timeline of its own).
            if (root.pendingIsSpace && roomId && app.spaces)
                app.spaces.activeSpaceId = roomId
            if (root.opened)
                root.close()
        }
    }

    FileDialog {
        id: avatarFileDialog
        title: qsTr("Choose a room picture")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)") ]
        onAccepted: root.roomAvatarPath = selectedFile.toString()
    }

    background: Rectangle {
        color: AppTheme.surface
        border.color: AppTheme.border
        border.width: 1
        radius: AppTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: AppTheme.spacing12

        // ── Header: mode title + bare close ───────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                objectName: "creationTitle"
                Layout.fillWidth: true
                text: root.mode === "room" ? qsTr("Create a room")
                    : root.mode === "space" ? qsTr("Create a Space")
                    : qsTr("New conversation")
                color: AppTheme.textPrimary
                font.family: AppTheme.uiFont
                font.pixelSize: AppTheme.fontSizeHeader
                font.weight: Font.ExtraBold
                elide: Label.ElideRight
            }
            IconButton {
                objectName: "creationCloseButton"
                implicitWidth: 30; implicitHeight: 30
                radius: AppTheme.radiusMd
                iconName: "close"
                iconSize: 18
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }

        SegmentedControl {
            objectName: "creationModeTabs"
            model: [
                { label: qsTr("Direct message"), value: "dm" },
                { label: qsTr("Room"), value: "room" },
                { label: qsTr("Space"), value: "space" }
            ]
            current: root.mode
            onActivated: (value) => root.switchMode(value)
        }

        // ── Single error banner ───────────────────────────────────────────
        Rectangle {
            objectName: "creationErrorBanner"
            visible: app.conversations.errorMessage.length > 0
            Layout.fillWidth: true
            radius: AppTheme.radiusMd
            color: Qt.alpha(AppTheme.danger, 0.12)
            border.width: 1
            border.color: Qt.alpha(AppTheme.danger, 0.45)
            implicitHeight: errorLabel.implicitHeight + AppTheme.spacing8 * 2
            Label {
                id: errorLabel
                anchors.fill: parent
                anchors.margins: AppTheme.spacing8
                text: app.conversations.errorMessage
                color: AppTheme.danger
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.fontSizeS
                verticalAlignment: Text.AlignVCenter
            }
        }

        // ── Active tab (scrolls when height-constrained) ──────────────────
        Flickable {
            id: tabFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: tabColumn.implicitHeight
            contentWidth: width
            contentHeight: tabColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: tabColumn
                width: tabFlick.width
                spacing: AppTheme.spacing12

                Loader {
                    id: dmTabLoader
                    Layout.fillWidth: true
                    active: root.visible && root.mode === "dm"
                    sourceComponent: dmTabComponent
                    onLoaded: Qt.callLater(function() {
                        if (dmTabLoader.item && root.visible)
                            dmTabLoader.item.focusFirst()
                    })
                }
                Loader {
                    id: roomTabLoader
                    Layout.fillWidth: true
                    active: root.visible && root.mode === "room"
                    sourceComponent: roomTabComponent
                    onLoaded: Qt.callLater(function() {
                        if (roomTabLoader.item && root.visible)
                            roomTabLoader.item.focusFirst()
                    })
                }
                Loader {
                    id: spaceTabLoader
                    Layout.fillWidth: true
                    active: root.visible && root.mode === "space"
                    sourceComponent: spaceTabComponent
                    onLoaded: Qt.callLater(function() {
                        if (spaceTabLoader.item && root.visible)
                            spaceTabLoader.item.focusFirst()
                    })
                }
            }
        }

        // ── Footer: busy state + Cancel (always enabled) ──────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: AppTheme.spacing8
            Label {
                objectName: "creationBusyLabel"
                visible: root.busy
                text: qsTr("Working…")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeS
                onVisibleChanged: if (!visible) opacity = 1.0
                SequentialAnimation on opacity {
                    running: root.busy && !AppTheme.reducedMotion
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation { from: 1.0; to: 0.45; duration: 600 }
                    NumberAnimation { from: 0.45; to: 1.0; duration: 600 }
                }
            }
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "creationCancelButton"
                text: qsTr("Cancel")
                Accessible.name: qsTr("Cancel")
                onClicked: root.close()
            }
        }
    }

    // ── Direct Message tab ────────────────────────────────────────────────
    Component {
        id: dmTabComponent
        ColumnLayout {
            objectName: "dmTab"
            spacing: AppTheme.spacing12

            function focusFirst() {
                if (root.selectedUserId === "")
                    dmPicker.focusSearch()
            }

            UserPicker {
                id: dmPicker
                objectName: "dmUserPicker"
                Layout.fillWidth: true
                visible: root.selectedUserId === ""
                onUserSelected: (userId, displayName, avatarUrl) => {
                    root.selectedUserId = userId
                    root.selectedDisplayName = displayName
                    root.selectedAvatarUrl = avatarUrl || ""
                    app.conversations.checkExistingDm(userId)
                }
            }

            // Selected-user card.
            Rectangle {
                objectName: "dmSelectedCard"
                visible: root.selectedUserId !== ""
                Layout.fillWidth: true
                radius: AppTheme.radiusMd
                color: AppTheme.cardElevated
                border.width: 1
                border.color: AppTheme.border
                implicitHeight: selectedRow.implicitHeight + AppTheme.spacing12 * 2
                RowLayout {
                    id: selectedRow
                    anchors.fill: parent
                    anchors.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing12
                    Avatar {
                        size: 40
                        circle: true
                        mxc: root.selectedAvatarUrl
                        name: root.selectedDisplayName.length > 0
                              ? root.selectedDisplayName : root.selectedUserId
                        colorKey: root.selectedUserId
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing2
                        Label {
                            Layout.fillWidth: true
                            text: root.selectedDisplayName.length > 0
                                  ? root.selectedDisplayName : root.selectedUserId
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSizeM
                            font.weight: Font.Bold
                            elide: Label.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: root.selectedDisplayName.length > 0
                            text: root.selectedUserId
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.fontSizeS
                            elide: Label.ElideMiddle
                        }
                    }
                    IconButton {
                        objectName: "dmChangeUserButton"
                        implicitWidth: 30; implicitHeight: 30
                        radius: AppTheme.radiusMd
                        iconName: "close"
                        iconSize: 16
                        Accessible.name: qsTr("Choose a different user")
                        ToolTip.text: Accessible.name
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: {
                            root.selectedUserId = ""
                            root.selectedDisplayName = ""
                            root.selectedAvatarUrl = ""
                            app.conversations.userSearch.clear()
                            Qt.callLater(function() { dmPicker.focusSearch() })
                        }
                    }
                }
            }

            // Existing-DM reuse comes FIRST.
            Label {
                visible: root.selectedUserId !== ""
                         && app.conversations.existingDms.length > 0
                text: qsTr("You already share a direct message with this user")
                color: AppTheme.textSecondary
                font.pixelSize: AppTheme.fontSizeS
            }
            Repeater {
                model: root.selectedUserId !== ""
                       ? app.conversations.existingDms : []
                Rectangle {
                    id: existingRow
                    required property var modelData
                    required property int index
                    objectName: "existingDmRow_" + index
                    Layout.fillWidth: true
                    radius: AppTheme.radiusMd
                    color: AppTheme.cardElevated
                    border.width: 1
                    border.color: AppTheme.border
                    implicitHeight: existingRowContent.implicitHeight
                                    + AppTheme.spacing8 * 2
                    readonly property string dmName:
                        modelData.name && modelData.name.length > 0
                        ? modelData.name : qsTr("Existing conversation")
                    RowLayout {
                        id: existingRowContent
                        anchors.fill: parent
                        anchors.margins: AppTheme.spacing8
                        spacing: AppTheme.spacing8
                        Avatar {
                            size: 32
                            circle: true
                            name: existingRow.dmName
                            colorKey: modelData.roomId
                        }
                        Label {
                            Layout.fillWidth: true
                            text: existingRow.dmName
                            color: AppTheme.textPrimary
                            font.pixelSize: AppTheme.fontSizeM
                            elide: Label.ElideRight
                        }
                        AppButton {
                            objectName: "existingDmOpen_" + existingRow.index
                            text: qsTr("Open")
                            enabled: !root.busy
                            Accessible.name: qsTr("Open %1").arg(existingRow.dmName)
                            onClicked: {
                                app.openRoom(existingRow.modelData.roomId)
                                root.close()
                            }
                        }
                    }
                }
            }

            // Primary action when there is nothing to reuse…
            AppButton {
                objectName: "dmStartButton"
                visible: root.selectedUserId !== ""
                         && app.conversations.existingDms.length === 0
                kind: "primary"
                Layout.fillWidth: true
                enabled: !root.busy
                text: qsTr("Start encrypted direct message")
                Accessible.name: text
                onClicked: app.conversations.startDirectMessage(root.selectedUserId)
            }
            // …clearly-secondary escape hatch when there is.
            AppButton {
                objectName: "dmStartAnywayButton"
                visible: root.selectedUserId !== ""
                         && app.conversations.existingDms.length > 0
                Layout.fillWidth: true
                enabled: !root.busy
                text: qsTr("Start a new conversation anyway")
                Accessible.name: text
                onClicked: app.conversations.startDirectMessage(root.selectedUserId)
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Direct messages are end-to-end encrypted.")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeXS
                wrapMode: Text.WordWrap
            }
        }
    }

    // ── Room tab ──────────────────────────────────────────────────────────
    Component {
        id: roomTabComponent
        ColumnLayout {
            objectName: "roomTab"
            spacing: AppTheme.spacing8

            function focusFirst() {
                roomNameField.forceActiveFocus()
            }

            Label {
                text: qsTr("Room name")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
            }
            AppTextField {
                id: roomNameField
                objectName: "roomNameField"
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. Design team")
                Accessible.name: qsTr("Room name")
                Component.onCompleted: text = root.roomNameText
                onTextChanged: root.roomNameText = text
                onTextEdited: root.roomNameEdited = true
                onAccepted: root.submitRoom()
                KeyNavigation.tab: roomTopicField
            }
            Label {
                objectName: "roomNameHint"
                visible: root.roomNameEdited
                         && root.roomNameText.trim().length === 0
                Layout.fillWidth: true
                text: qsTr("The room needs a name.")
                color: AppTheme.danger
                font.pixelSize: AppTheme.fontSizeXS
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.topMargin: AppTheme.spacing4
                text: qsTr("Topic (optional)")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
            }
            AppTextField {
                id: roomTopicField
                objectName: "roomTopicField"
                Layout.fillWidth: true
                placeholderText: qsTr("What is this room about?")
                Accessible.name: qsTr("Topic")
                Component.onCompleted: text = root.roomTopicText
                onTextChanged: root.roomTopicText = text
                onAccepted: root.submitRoom()
            }

            Label {
                Layout.topMargin: AppTheme.spacing4
                text: qsTr("Visibility")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
            }
            SegmentedControl {
                objectName: "roomVisibility"
                model: [
                    { label: qsTr("Private"), value: "private" },
                    { label: qsTr("Public"), value: "public" }
                ]
                current: root.roomIsPublic ? "public" : "private"
                onActivated: (value) => root.roomIsPublic = (value === "public")
            }
            Label {
                objectName: "roomVisibilityCaption"
                Layout.fillWidth: true
                text: root.roomIsPublic
                      ? qsTr("Anyone can find and join this room.")
                      : qsTr("Only people you invite can join this room.")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeXS
                wrapMode: Text.WordWrap
            }

            // Encryption switch-row (labels toggle it too).
            RowLayout {
                objectName: "roomEncryptRow"
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacing4
                spacing: AppTheme.spacing12
                AppSwitch {
                    id: roomEncryptSwitch
                    objectName: "roomEncryptSwitch"
                    checked: root.roomIsPublic ? false : root.roomEncrypted
                    enabled: !root.roomIsPublic && !root.busy
                    Accessible.name: qsTr("End-to-end encryption")
                    onToggled: root.roomEncrypted = !root.roomEncrypted
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacing2
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("End-to-end encryption")
                        color: root.roomIsPublic ? AppTheme.textDisabled
                                                 : AppTheme.textPrimary
                        font.pixelSize: AppTheme.fontSizeS
                        font.weight: Font.Bold
                    }
                    Label {
                        objectName: "roomEncryptCaption"
                        Layout.fillWidth: true
                        text: root.roomIsPublic
                              ? qsTr("Public rooms are not end-to-end encrypted.")
                              : root.roomEncrypted
                                ? qsTr("Only invited members can read messages.")
                                : qsTr("Warning: this private room will NOT be "
                                       + "encrypted. The server can read every "
                                       + "message.")
                        color: !root.roomIsPublic && !root.roomEncrypted
                               ? AppTheme.warning : AppTheme.textMuted
                        font.pixelSize: AppTheme.fontSizeXS
                        wrapMode: Text.WordWrap
                    }
                    TapHandler {
                        enabled: roomEncryptSwitch.enabled
                        onTapped: roomEncryptSwitch.toggled()
                    }
                }
            }

            Label {
                visible: root.roomIsPublic
                Layout.topMargin: AppTheme.spacing4
                text: qsTr("Address (optional)")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
            }
            AppTextField {
                id: roomAliasField
                objectName: "roomAliasField"
                visible: root.roomIsPublic
                Layout.fillWidth: true
                placeholderText: qsTr("Local address, e.g. my-room")
                Accessible.name: qsTr("Room address")
                Component.onCompleted: text = root.roomAliasText
                onTextChanged: root.roomAliasText = text
                onAccepted: root.submitRoom()
            }

            // Optional placement into the current Space.
            RowLayout {
                objectName: "addToSpaceRow"
                visible: root.spacePlacementAvailable
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacing4
                spacing: AppTheme.spacing12
                AppSwitch {
                    id: addToSpaceSwitch
                    objectName: "addToSpaceSwitch"
                    checked: root.addToSpaceChecked
                    enabled: !root.busy
                    Accessible.name: qsTr("Add to the current Space")
                    onToggled: root.addToSpaceChecked = !root.addToSpaceChecked
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Add to the current Space")
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.fontSizeS
                    TapHandler {
                        enabled: addToSpaceSwitch.enabled
                        onTapped: addToSpaceSwitch.toggled()
                    }
                }
            }

            Label {
                Layout.topMargin: AppTheme.spacing4
                text: qsTr("Invite people (optional)")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
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
                        border.width: 1
                        border.color: AppTheme.border
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
                            IconButton {
                                objectName: "inviteChipRemove_" + index
                                implicitWidth: 18; implicitHeight: 18
                                radius: AppTheme.radiusPill
                                iconName: "close"
                                iconSize: 12
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
                objectName: "roomInvitePicker"
                Layout.fillWidth: true
                onUserSelected: (userId, displayName, avatarUrl) => {
                    if (root.roomInvites.indexOf(userId) === -1) {
                        var next = root.roomInvites.slice()
                        next.push(userId)
                        root.roomInvites = next
                    }
                    roomPicker.clear()
                }
            }

            // Optional room picture (applied AFTER creation; a failed upload
            // is a warning toast, never a failed create).
            Label {
                Layout.topMargin: AppTheme.spacing4
                text: qsTr("Room picture (optional)")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Item {
                    objectName: "roomAvatarPreview"
                    visible: root.roomAvatarPath !== ""
                    implicitWidth: 40
                    implicitHeight: 40
                    Image {
                        id: avatarPreviewImage
                        anchors.fill: parent
                        source: root.roomAvatarPath
                        sourceSize.width: 80
                        sourceSize.height: 80
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        visible: false
                    }
                    MultiEffect {
                        anchors.fill: parent
                        source: avatarPreviewImage
                        maskEnabled: true
                        maskSource: avatarPreviewMask
                    }
                    Item {
                        id: avatarPreviewMask
                        anchors.fill: parent
                        layer.enabled: true
                        visible: false
                        Rectangle {
                            anchors.fill: parent
                            radius: width / 2
                            color: AppTheme.textPrimary
                        }
                    }
                }
                AppButton {
                    objectName: "roomAvatarPickButton"
                    text: root.roomAvatarPath === "" ? qsTr("Choose picture")
                                                     : qsTr("Change picture")
                    enabled: !root.busy
                    Accessible.name: text
                    onClicked: avatarFileDialog.open()
                }
                IconButton {
                    objectName: "roomAvatarRemoveButton"
                    visible: root.roomAvatarPath !== ""
                    implicitWidth: 30; implicitHeight: 30
                    radius: AppTheme.radiusMd
                    iconName: "delete"
                    iconSize: 16
                    Accessible.name: qsTr("Remove picture")
                    onClicked: root.roomAvatarPath = ""
                }
                Item { Layout.fillWidth: true }
            }

            AppButton {
                objectName: "roomCreateButton"
                kind: "primary"
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacing4
                enabled: root.canCreateRoom
                text: root.roomIsPublic ? qsTr("Create public room")
                      : (root.roomEncrypted ? qsTr("Create encrypted room")
                                            : qsTr("Create unencrypted room"))
                Accessible.name: text
                onClicked: root.submitRoom()
            }
        }
    }

    // ── Space tab (no encryption control instantiated at all) ─────────────
    Component {
        id: spaceTabComponent
        ColumnLayout {
            objectName: "spaceTab"
            spacing: AppTheme.spacing8

            function focusFirst() {
                spaceNameField.forceActiveFocus()
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("A Space groups related rooms together in the "
                           + "left rail. It is a real Matrix Space, not a "
                           + "local folder.")
                color: AppTheme.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: AppTheme.fontSizeXS
            }

            Label {
                text: qsTr("Space name")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
            }
            AppTextField {
                id: spaceNameField
                objectName: "spaceNameField"
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. My community")
                Accessible.name: qsTr("Space name")
                Component.onCompleted: text = root.spaceNameText
                onTextChanged: root.spaceNameText = text
                onTextEdited: root.spaceNameEdited = true
                onAccepted: root.submitSpace()
                KeyNavigation.tab: spaceTopicField
            }
            Label {
                objectName: "spaceNameHint"
                visible: root.spaceNameEdited
                         && root.spaceNameText.trim().length === 0
                Layout.fillWidth: true
                text: qsTr("The Space needs a name.")
                color: AppTheme.danger
                font.pixelSize: AppTheme.fontSizeXS
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.topMargin: AppTheme.spacing4
                text: qsTr("Description (optional)")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
            }
            AppTextField {
                id: spaceTopicField
                objectName: "spaceTopicField"
                Layout.fillWidth: true
                placeholderText: qsTr("What is this Space for?")
                Accessible.name: qsTr("Description")
                Component.onCompleted: text = root.spaceTopicText
                onTextChanged: root.spaceTopicText = text
                onAccepted: root.submitSpace()
            }

            Label {
                Layout.topMargin: AppTheme.spacing4
                text: qsTr("Visibility")
                color: AppTheme.textMuted
                font.family: AppTheme.uiFont
                font.pixelSize: 12
            }
            SegmentedControl {
                objectName: "spaceVisibility"
                model: [
                    { label: qsTr("Private"), value: "private" },
                    { label: qsTr("Public"), value: "public" }
                ]
                current: root.spaceIsPublic ? "public" : "private"
                onActivated: (value) => root.spaceIsPublic = (value === "public")
            }
            Label {
                objectName: "spaceVisibilityCaption"
                Layout.fillWidth: true
                text: root.spaceIsPublic
                      ? qsTr("Anyone can find and join this Space.")
                      : qsTr("Only people you invite can see this Space.")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeXS
                wrapMode: Text.WordWrap
            }

            AppButton {
                objectName: "spaceCreateButton"
                kind: "primary"
                Layout.fillWidth: true
                Layout.topMargin: AppTheme.spacing4
                enabled: root.canCreateSpace
                text: qsTr("Create Space")
                Accessible.name: text
                onClicked: root.submitSpace()
            }
        }
    }
}
