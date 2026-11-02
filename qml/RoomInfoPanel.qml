import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root

    // The section tabs wrap to two rows when they cannot fit the panel (whose
    // floor is 260). The decision comes from tabsProbe, an off-layout copy
    // whose width the result cannot move, so the layout never chases itself.
    // Applied deferred rather than as a direct binding: changing the wrapped
    // Repeater's model from inside the layout pass (overflowing depends on
    // widths that pass is settling) rebuilt delegate trees every frame and hung
    // the GUI thread. Qt.callLater moves the change outside the pass and
    // coalesces bursts. The offscreen harness cannot reproduce the hang.
    property bool tabsWrap: false
    readonly property real tabsUnwrapSlack: 12
    function applyTabsWrap() {
        if (!tabsWrap) {
            tabsWrap = tabsProbe.width > 0
                       && tabsProbe.implicitWidth > tabsProbe.width
        } else if (tabsProbe.width > 0
                   && tabsProbe.implicitWidth
                      <= tabsProbe.width - root.tabsUnwrapSlack) {
            tabsWrap = false
        }
    }
    readonly property var tabModel: {
        const tabs = [{ label: qsTr("Overview"), value: "overview" }]
        if (root.pinnedAvailable)
            tabs.push({ label: qsTr("Pinned"), value: "pinned" })
        tabs.push({ label: qsTr("People"), value: "people" })
        tabs.push({ label: qsTr("Media"), value: "media" })
        if (root.widgetsAvailable)
            tabs.push({ label: qsTr("Widgets"), value: "widgets" })
        return tabs
    }
    color: AppTheme.sidePanel
    ThemedSurface {
        anchors.fill: parent
        role: "sidePanel"
        flatFill: false
    }
    visible: width > 0
    clip: true

    property var roomData: ({})
    signal closeRequested()
    // Media & Files hands viewing/saving to TimelinePane. Pass the list and the
    // index, never a key alone: the picture may be absent from the loaded
    // timeline (see MediaBrowser's signal).
    signal openImagesRequested(var entries, int index)
    signal saveMediaRequested(string mediaKey, string filename)

    property string section: "overview"
    readonly property int nameMaxLines: 3
    readonly property int topicFoldedLines: 6
    property bool topicExpanded: false
    property string memberFilter: ""

    // Escape inside a text field leaves the field instead of closing the
    // whole panel and dropping the unsaved name/topic/profile draft: the host's
    // Escape shortcut would otherwise win (a text field does not claim
    // ShortcutOverride for Escape). The next Escape, with focus out of the
    // field, closes the panel as before.
    function textFieldFocused() {
        var f = Window.activeFocusItem
        return !!f && f !== root && f.hasOwnProperty("cursorPosition")
               && f.hasOwnProperty("text")
    }
    Keys.onShortcutOverride: (event) => {
        if (event.key === Qt.Key_Escape && root.textFieldFocused())
            event.accepted = true
    }
    Keys.onEscapePressed: (event) => {
        if (root.textFieldFocused()) {
            root.forceActiveFocus()
            event.accepted = true
        }
    }

    readonly property bool pinnedAvailable:
        app.pinned && app.pinned.supported
        && app.roomInfo.roomId !== ""
        && app.roomInfo.roomId === app.pinned.roomId
    onPinnedAvailableChanged: {
        if (!pinnedAvailable && section === "pinned")
            section = "overview"
    }
    readonly property bool widgetsAvailable:
        app.widgets && app.widgets.supported
    onWidgetsAvailableChanged: {
        if (!widgetsAvailable && section === "widgets")
            section = "overview"
    }
    signal jumpToEventRequested(string eventId)
    signal exportRoomRequested()

    function openForRoom(roomId) {
        app.roomInfo.roomId = roomId
        if (app.widgets.supported) {
            app.widgets.roomId = roomId
            app.widgets.refresh()
        }
        app.requestRoomBridgeInfo(roomId, true)
        section = "overview"
        memberFilter = ""
        memberSearch.text = ""
        topicExpanded = false
        overviewFlick.contentY = 0
        memberSection.membership = "joined"
        memberSection.alphabetical = false
        refreshRoomData()
        app.requestRoomNotificationMode(roomId)
        if (app.roomClosure)
            app.roomClosure.checkServerAdmin()
    }
    function accessNotes(joinRule, history, listed) {
        var notes = []
        var open = joinRule === "public"
        if (history === "world_readable" && !open)
            notes.push(qsTr("Anyone can read this room's history without "
                            + "joining, even though not everyone can join it."))
        if (listed === 1 && (joinRule === "invite" || joinRule === "private"))
            notes.push(qsTr("This room is listed in the room directory, but "
                            + "only invited people can join it."))
        return notes
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
    }

    function openMemberMenu(member, anchor, x, y) {
        if (!member || !member.userId || !app.roomInfo)
            return
        memberMenu.member = member
        memberMenu.rowItem = anchor
        memberMenu.canKick = app.roomInfo.canModerate(member.userId, "kick")
        memberMenu.canBan = app.roomInfo.canModerate(member.userId, "ban")
        memberMenu.canUnban = app.roomInfo.canModerate(member.userId, "unban")
        memberMenu.popup(anchor, x, y)
    }

    AppMenu {
        id: memberMenu
        objectName: "roomMemberMenu"
        menuWidth: 210
        property var member: ({})
        property Item rowItem: null
        property bool canKick: false
        property bool canBan: false
        property bool canUnban: false
        contextLabel: member && member.userId
                      ? (member.displayName || member.userId) : ""

        AppMenuItem {
            objectName: "roomMemberMenuProfile"
            iconName: "person"
            text: qsTr("View profile")
            onTriggered: memberProfile.openFor(memberMenu.member, memberMenu.rowItem)
        }
        AppMenuSeparator {
            visible: memberMenu.canKick || memberMenu.canBan
                     || memberMenu.canUnban
        }
        AppMenuItem {
            objectName: "roomMemberMenuKick"
            visible: memberMenu.canKick
            danger: true
            iconName: "person_remove"
            text: qsTr("Remove from room…")
            onTriggered: memberProfile.openForAction(memberMenu.member, "kick")
        }
        AppMenuItem {
            objectName: "roomMemberMenuBan"
            visible: memberMenu.canBan
            danger: true
            iconName: "block"
            text: qsTr("Ban from room…")
            onTriggered: memberProfile.openForAction(memberMenu.member, "ban")
        }
        AppMenuItem {
            objectName: "roomMemberMenuUnban"
            visible: memberMenu.canUnban
            iconName: "undo"
            text: qsTr("Unban…")
            onTriggered: memberProfile.openForAction(memberMenu.member, "unban")
        }
    }

    function findDemoDescendant(obj, name) {
        if (!obj) return null
        if (obj.objectName === name) return obj
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

    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenInvitePeople() {
            inviteDialog.openFor(app.currentRoomId)
            Qt.callLater(function() {
                var picker = root.findDemoDescendant(inviteDialog, "invitePeoplePicker")
                if (picker)
                    picker.searchText = "ma"
            })
        }
    }

    NativeFileDialog {
        id: avatarDialog
        purpose: "image"
        title: qsTr("Choose room avatar")
        currentFolder: app.defaultFileDialogFolder()
        fileMode: "open"
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg)") ]
        // The picker only chooses; the crop dialog is the gate: an SVG is converted
        // to a PNG locally first, never previewed or uploaded as SVG (CLAUDE.md §6).
        onAccepted: avatarCrop.openFor(selectedFile)
    }

    NativeFileDialog {
        id: myAvatarDialog
        purpose: "image"
        title: qsTr("Choose your avatar for this room")
        currentFolder: app.defaultFileDialogFolder()
        fileMode: "open"
        nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg)") ]
        onAccepted: myAvatarCrop.openFor(selectedFile)
    }
    ImageCropDialog {
        id: myAvatarCrop
        role: "avatar"
        onCropped: function (file) { app.roomInfo.setMyRoomAvatar(file) }
    }

    ImageCropDialog {
        id: avatarCrop
        role: "avatar"
        onCropped: function (file) { app.roomInfo.setRoomAvatar(file) }
    }

    SegmentedControl {
        id: tabsProbe
        objectName: "roomInfoTabsProbe"
        visible: false
        enabled: false
        storm: true
        dense: true
        fitWidth: false
        model: root.tabModel
        width: Math.max(0, root.width - AppTheme.spacing6 * 2)
        onWidthChanged: Qt.callLater(root.applyTabsWrap)
        onImplicitWidthChanged: Qt.callLater(root.applyTabsWrap)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(AppTheme.headerBandHeight,
                headerRow.implicitHeight + AppTheme.spacing12 * 2)
            color: AppTheme.surface
            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacing12
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    text: {
                        if (root.section !== "people")
                            return qsTr("Room information");
                        var n = app.roomInfo.joinedCount;
                        if (n <= 0)
                            return qsTr("Members");
                        return n === 1 ? qsTr("1 member")
                                       : qsTr("%1 members").arg(n);
                    }
                    color: AppTheme.textPrimary
                    elide: Label.ElideRight
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightDisplay
                }
                IconButton {
                    objectName: "roomPermissionsButton"
                    implicitWidth: 30; implicitHeight: 30
                    radius: AppTheme.radiusControl
                    iconName: "lock"
                    iconSize: 18
                    Accessible.name: qsTr("Room permissions")
                    onClicked: {
                        if (!root.roomPermissionsDialog)
                            root.roomPermissionsDialog =
                                roomPermissionsDialogComponent.createObject(root)
                        root.roomPermissionsDialog.openPermissionsForRoom(
                            app.roomInfo.roomId, root.roomData.name || "")
                    }
                }
                IconButton {
                    implicitWidth: 30; implicitHeight: 30
                    radius: AppTheme.radiusControl
                    iconName: "close"
                    iconSize: 18
                    Accessible.name: qsTr("Close room information")
                    onClicked: root.closeRequested()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.headerDivider }

        SegmentedControl {
            id: roomInfoTabs
            objectName: "roomInfoTabs"
            storm: true
            Layout.leftMargin: AppTheme.spacing6
            Layout.rightMargin: AppTheme.spacing6
            Layout.topMargin: AppTheme.spacing6
            Layout.bottomMargin: AppTheme.spacing6
            // Plain `visible`: keeping it in the layout at zero height bound to
            // its own implicit height made the ColumnLayout re-polish
            // endlessly. Safe because the wrap is decided by tabsProbe.
            visible: !root.tabsWrap
            // Zero minimum: otherwise the RowLayout's minimum is the sum of its
            // segments, which propagates up and pushes the panel off the window
            // instead of letting the strip overflow and wrap.
            Layout.minimumWidth: 0
            clip: true
            Layout.fillWidth: true
            dense: true
            fitWidth: true
            model: root.tabModel
            current: root.section
            onActivated: (value) => root.section = value
        }
        Repeater {
            model: root.tabsWrap ? 2 : 0
            SegmentedControl {
                required property int index
                objectName: "roomInfoTabsRow" + index
                storm: true
                dense: true
                Layout.leftMargin: AppTheme.spacing6
                Layout.rightMargin: AppTheme.spacing6
                Layout.topMargin: index === 0 ? AppTheme.spacing6 : 0
                Layout.bottomMargin: index === 1 ? AppTheme.spacing6 : 2
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: {
                    const all = root.tabModel
                    const cut = Math.ceil(all.length / 2)
                    return index === 0 ? all.slice(0, cut) : all.slice(cut)
                }
                current: root.section
                onActivated: (value) => root.section = value
            }
        }

        Flickable {
            id: overviewFlick
            visible: root.section === "overview"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: overviewColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            readonly property bool comboOpen:
                notificationModeCombo.popup.visible
                || joinRuleCombo.popup.visible || historyCombo.popup.visible
            interactive: !comboOpen
            objectName: "roomInfoOverviewFlick"
            SmoothWheelArea { enabled: !overviewFlick.comboOpen }

            ColumnLayout {
                id: overviewColumn
                width: parent.width
                spacing: AppTheme.spacing12

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: AppTheme.spacing12
                    Layout.rightMargin: AppTheme.spacing12
                    Layout.topMargin: AppTheme.spacing12
                    spacing: 4
                    Label {
                        text: app.serverRoomNotificationModes
                              ? qsTr("Notifications")
                              : qsTr("Notifications (this device)")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }
                    AppComboBox {
                        id: notificationModeCombo
                        objectName: "roomNotificationModeCombo"
                        Layout.fillWidth: true
                        model: app.serverRoomNotificationModes
                            ? [
                                qsTr("All messages"),
                                qsTr("Mentions & keywords"),
                                qsTr("Mute"),
                                qsTr("Follow account default")
                              ]
                            : [
                                qsTr("All messages"),
                                qsTr("Mentions & keywords"),
                                qsTr("Mute")
                              ]
                        property int displayedMode: 0
                        property bool syncFailed: false
                        function refreshMode() {
                            displayedMode = app.roomInfo.roomId !== ""
                                ? app.settings.roomNotificationMode(
                                      app.roomInfo.roomId)
                                : 0
                            syncFailed = app.roomInfo.roomId !== ""
                                && app.roomNotificationModeSyncFailed(
                                       app.roomInfo.roomId)
                        }
                        Component.onCompleted: refreshMode()
                        currentIndex: (displayedMode >= 0
                                       && displayedMode < count)
                                      ? displayedMode : 0
                        Connections {
                            target: app.settings
                            function onRoomNotificationModeChanged(roomId) {
                                if (roomId === app.roomInfo.roomId)
                                    notificationModeCombo.refreshMode()
                            }
                        }
                        Connections {
                            target: app
                            function onRoomNotificationModeSyncStateChanged(roomId) {
                                if (roomId === app.roomInfo.roomId)
                                    notificationModeCombo.refreshMode()
                            }
                        }
                        Connections {
                            target: app.roomInfo
                            function onRoomIdChanged() {
                                notificationModeCombo.refreshMode()
                            }
                        }
                        onActivated: (index) => {
                            if (app.roomInfo.roomId !== "")
                                app.setRoomNotificationMode(
                                    app.roomInfo.roomId, index)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                        text: app.serverRoomNotificationModes
                              ? (notificationModeCombo.syncFailed
                                 ? qsTr("Couldn't save to the server — "
                                        + "kept on this device. "
                                        + "Retried when you reconnect.")
                                 : (notificationModeCombo.displayedMode === 3
                                    ? qsTr("This room has no override — your "
                                           + "account's notification settings "
                                           + "apply.")
                                    : qsTr("Saved to your account's notification "
                                           + "settings (server push rules).")))
                              : qsTr("Local setting: it does not change this "
                                     + "room's server push rules.")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    RowLayout {
                        spacing: AppTheme.spacing12
                        Avatar {
                            objectName: "roomInfoAvatar"
                            size: 56
                            name: root.roomData.name || ""
                            mxc: root.roomData.avatarUrl || ""
                            colorKey: root.roomData.identityColorKey || ""
                            circle: root.roomData.isDirect === true

                            PresenceDot {
                                objectName: "roomInfoPresenceDot"
                                anchors.bottom: parent.bottom
                                anchors.right: parent.right
                                dotSize: 16
                                ring: root.color
                                hoverStatus: true
                                userId: root.visible
                                        && root.section === "overview"
                                        && root.roomData.isDirect === true
                                        && (root.roomData.identityColorKey || "")
                                               .charAt(0) === "@"
                                        ? root.roomData.identityColorKey : ""
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                id: roomInfoName
                                objectName: "roomInfoName"
                                Layout.fillWidth: true
                                text: root.roomData.name || qsTr("(unnamed room)")
                                textFormat: Text.PlainText
                                color: AppTheme.textPrimary
                                font.pixelSize: AppTheme.textTitle
                                font.weight: AppTheme.weightStrong
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                maximumLineCount: root.nameMaxLines
                                elide: Text.ElideRight
                                Accessible.name: text
                                HoverHandler { id: roomInfoNameHover }
                                ToolTip.visible: roomInfoName.truncated
                                                 && roomInfoNameHover.hovered
                                ToolTip.delay: 500
                                ToolTip.text: roomInfoName.text
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
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                            }
                            Label {
                                visible: root.roomData.encrypted !== true
                                text: qsTr("Not encrypted")
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textBody
                            }
                        }
                    }

                    Item {
                        id: topicFold
                        objectName: "roomInfoTopicFold"
                        Layout.fillWidth: true
                        visible: (root.roomData.topic || "").length > 0
                        readonly property real foldedHeight:
                            topicMetrics.lineSpacing * root.topicFoldedLines
                        readonly property bool foldable:
                            roomInfoTopic.implicitHeight > foldedHeight + 1
                        implicitHeight: foldable && !root.topicExpanded
                                        ? foldedHeight
                                        : roomInfoTopic.implicitHeight
                        clip: true
                        FontMetrics {
                            id: topicMetrics
                            font: roomInfoTopic.font
                        }
                        TopicText {
                            id: roomInfoTopic
                            objectName: "roomInfoTopic"
                            width: parent.width
                            plainText: root.roomData.topic || ""
                        }
                    }
                    AppButton {
                        objectName: "roomInfoTopicToggle"
                        visible: topicFold.visible && topicFold.foldable
                        kind: "ghost"
                        size: "sm"
                        text: root.topicExpanded ? qsTr("Show less")
                                                 : qsTr("Show more")
                        Accessible.name: root.topicExpanded
                                         ? qsTr("Show less of the topic")
                                         : qsTr("Show the whole topic")
                        onClicked: root.topicExpanded = !root.topicExpanded
                    }

                    Label {
                        objectName: "roomOverviewMemberCount"
                        visible: !app.roomInfo.loading || app.roomInfo.joinedCount > 0
                        text: app.roomInfo.invitedCount > 0
                              ? qsTr("%n member(s) (%1 invited)", "",
                                     app.roomInfo.joinedCount)
                                  .arg(app.roomInfo.invitedCount)
                              : qsTr("%n member(s)", "",
                                     app.roomInfo.joinedCount)
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textBody
                    }

                    RowLayout {
                        objectName: "roomInfoBridgeRow"
                        visible: (root.roomData.bridgeLabel || "").length > 0
                        spacing: 4
                        Icon {
                            name: "link"
                            size: 14
                            color: AppTheme.textMuted
                        }
                        Label {
                            objectName: "roomInfoBridgeLabel"
                            Layout.fillWidth: true
                            text: qsTr("Bridged via %1")
                                  .arg(root.roomData.bridgeLabel || "")
                            textFormat: Text.PlainText
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textBody
                            elide: Text.ElideRight
                        }
                    }

                    AppButton {
                        objectName: "roomInfoPolicyButton"
                        visible: app.policy && app.policy.available
                        text: qsTr("Moderation rules…")
                        onClicked: policyListDialog.openFor(
                            app.roomInfo.roomId, root.roomData.name || "")
                        ToolTip.text: qsTr("Ban lists this room publishes")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                    }
                    AppButton {
                        objectName: "roomInfoExportButton"
                        text: qsTr("Export room…")
                        onClicked: root.exportRoomRequested()
                        ToolTip.text: qsTr("Save this room's loaded messages "
                                           + "to a file")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
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
                    TextEdit {
                        id: copyHelper
                        visible: false
                        width: 0; height: 0
                    }
                }

                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

                ColumnLayout {
                    id: chatBackgroundSection
                    objectName: "roomChatBackgroundSection"
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    visible: !!app.backdrops && app.currentRoomId !== ""

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing6
                        Icon {
                            name: "image"
                            size: 16
                            color: AppTheme.textSecondary
                        }
                        Label {
                            objectName: "roomChatBackgroundTitle"
                            Layout.fillWidth: true
                            text: qsTr("Chat background")
                            color: AppTheme.textSecondary
                            font.pixelSize: AppTheme.textBody
                            font.weight: AppTheme.weightStrong
                            elide: Label.ElideRight
                        }
                    }
                    ChatBackgroundEditor {
                        objectName: "roomChatBackgroundEditor"
                        Layout.fillWidth: true
                        showTitle: false
                        scopeKind: "room"
                        scopeId: app.currentRoomId
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: AppTheme.border
                    visible: chatBackgroundSection.visible
                             && editRoomGroup.visible
                }

                ColumnLayout {
                    id: editRoomGroup
                    objectName: "roomEditSection"
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    visible: app.roomInfo.canEditName || app.roomInfo.canEditTopic
                             || app.roomInfo.canEditAvatar

                    Label {
                        text: qsTr("Edit room")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }
                    Flow {
                        visible: app.roomInfo.canEditAvatar
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppButton {
                            text: qsTr("Change avatar…")
                            enabled: !app.roomInfo.editPending
                            onClicked: avatarDialog.open()
                        }
                        AppButton {
                            objectName: "roomRemoveAvatarButton"
                            visible: (root.roomData.avatarUrl || "").length > 0
                            kind: "danger"
                            text: qsTr("Remove avatar")
                            enabled: !app.roomInfo.editPending
                            onClicked: app.roomInfo.removeRoomAvatar()
                        }
                    }
                    RowLayout {
                        visible: app.roomInfo.canEditName
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppTextField {
                            id: editName
                            Layout.minimumWidth: 60
                            Layout.fillWidth: true
                            placeholderText: qsTr("Room name")
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                            Component.onCompleted: cursorPosition = 0
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
                            Layout.minimumWidth: 60
                            Layout.fillWidth: true
                            placeholderText: qsTr("Topic")
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                            Component.onCompleted: cursorPosition = 0
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
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.WordWrap
                        font.pixelSize: AppTheme.textBody
                    }
                    AppBusyIndicator {
                        visible: app.roomInfo.editPending
                        running: visible
                        size: 18
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: AppTheme.border
                    visible: roomAdminBlock.visible
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: AppTheme.border
                    visible: app.roomInfo.roomProfilesSupported
                }
                ColumnLayout {
                    objectName: "roomProfileSection"
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    visible: app.roomInfo.roomProfilesSupported

                    Label {
                        text: qsTr("Your profile in this room")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: qsTr("Only affects this room. Everyone here sees "
                                   + "it; other rooms keep your usual name and "
                                   + "picture.")
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.textMeta
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppTextField {
                            id: myRoomNameField
                            objectName: "roomProfileNameField"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            enabled: !app.roomInfo.roomProfilePending
                            placeholderText: qsTr("Your name in this room")
                            onTextChanged: if (!activeFocus) cursorPosition = 0
                            Component.onCompleted: cursorPosition = 0
                        }
                        AppButton {
                            text: qsTr("Save")
                            enabled: !app.roomInfo.roomProfilePending
                                     && myRoomNameField.text.length > 0
                            onClicked: app.roomInfo.setMyRoomDisplayName(
                                myRoomNameField.text)
                        }
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        AppButton {
                            text: qsTr("Change picture…")
                            kind: "ghost"
                            size: "sm"
                            enabled: !app.roomInfo.roomProfilePending
                            onClicked: myAvatarDialog.open()
                        }
                        AppButton {
                            objectName: "roomProfileResetButton"
                            text: qsTr("Reset to global")
                            kind: "ghost"
                            size: "sm"
                            enabled: !app.roomInfo.roomProfilePending
                            onClicked: {
                                myRoomNameField.text = ""
                                app.roomInfo.setMyRoomDisplayName("")
                                app.roomInfo.clearMyRoomAvatar()
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: app.roomInfo.roomProfileError.length > 0
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        text: app.roomInfo.roomProfileError
                        color: AppTheme.danger
                        font.pixelSize: AppTheme.textMeta
                    }
                }

                ColumnLayout {
                    id: roomAdminBlock
                    objectName: "roomAdminBlock"
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8
                    visible: app.roomInfo.canChangeJoinRule
                             || app.roomInfo.canChangeAlias
                             || app.roomInfo.canChangeHistoryVisibility
                             || app.roomInfo.canChangeGuestAccess
                    onVisibleChanged: if (visible) app.roomInfo.requestDirectoryVisibility()
                    Connections {
                        target: app.roomInfo
                        function onRoomIdChanged() {
                            if (roomAdminBlock.visible)
                                app.roomInfo.requestDirectoryVisibility()
                        }
                    }

                    Label {
                        text: qsTr("Access")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.textBody
                        font.weight: AppTheme.weightStrong
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: app.roomInfo.canChangeJoinRule
                        readonly property bool restricted:
                            app.roomInfo.joinRule === "restricted"
                            || app.roomInfo.joinRule === "knock_restricted"
                        Label {
                            text: qsTr("Who can join")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        AppComboBox {
                            id: joinRuleCombo
                            objectName: "roomJoinRuleCombo"
                            Layout.fillWidth: true
                            visible: !parent.restricted
                            model: [
                                qsTr("Invited people only"),
                                qsTr("Anyone with the link"),
                                qsTr("Ask to join (knock)")
                            ]
                            readonly property var ruleValues:
                                ["invite", "public", "knock"]
                            property int displayedIndex: 0
                            function refreshRule() {
                                var idx = ruleValues.indexOf(
                                    app.roomInfo.joinRule)
                                displayedIndex = idx >= 0 ? idx : 0
                            }
                            Component.onCompleted: refreshRule()
                            currentIndex: displayedIndex
                            enabled: !app.roomInfo.editPending
                            Connections {
                                target: app.roomInfo
                                function onMembersChanged() {
                                    joinRuleCombo.refreshRule()
                                }
                            }
                            onActivated: (index) => {
                                app.roomInfo.setJoinRule(
                                    joinRuleCombo.ruleValues[index])
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Members of selected spaces can join")
                                color: AppTheme.text
                                font.pixelSize: AppTheme.textBody
                                wrapMode: Text.WordWrap
                            }
                            AppSwitch {
                                id: restrictedSwitch
                                objectName: "roomRestrictedSwitch"
                                checked: restrictedSwitch.parent.parent.restricted
                                enabled: !app.roomInfo.editPending
                                onToggled: {
                                    if (!checked) {
                                        var ids = restrictedPicker.selectedIds()
                                        if (ids.length === 0) {
                                            restrictedPicker.expanded = true
                                            return
                                        }
                                        app.roomInfo.setRestrictedJoinRule(
                                            "restricted", ids)
                                    } else {
                                        app.roomInfo.setJoinRule("invite")
                                    }
                                }
                            }
                        }
                        ColumnLayout {
                            id: restrictedPicker
                            objectName: "roomRestrictedPicker"
                            Layout.fillWidth: true
                            spacing: 2
                            property bool expanded: false
                            visible: parent.restricted || expanded
                            function selectedIds() {
                                var ids = []
                                for (var i = 0; i < spaceRepeater.count; ++i) {
                                    var row = spaceRepeater.itemAt(i)
                                    if (row && row.checked)
                                        ids.push(row.spaceId)
                                }
                                return ids
                            }
                            function apply(kind) {
                                var ids = selectedIds()
                                if (ids.length === 0)
                                    return
                                app.roomInfo.setRestrictedJoinRule(kind, ids)
                            }
                            Label {
                                text: qsTr("Spaces whose members may join")
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                            }
                            Repeater {
                                id: spaceRepeater
                                model: app.roomInfo.joinedSpaces
                                delegate: RowLayout {
                                    id: spaceRow
                                    required property var modelData
                                    readonly property string spaceId: modelData.roomId
                                    property bool checked:
                                        app.roomInfo.restrictedAllowedRooms
                                            .indexOf(modelData.roomId) >= 0
                                    Connections {
                                        target: app.roomInfo
                                        function onMembersChanged() {
                                            spaceRow.checked =
                                                app.roomInfo.restrictedAllowedRooms
                                                    .indexOf(spaceRow.spaceId) >= 0
                                        }
                                    }
                                    Layout.fillWidth: true
                                    spacing: AppTheme.spacing8
                                    Label {
                                        Layout.fillWidth: true
                                        text: spaceRow.modelData.name
                                        textFormat: Text.PlainText
                                        color: AppTheme.text
                                        font.pixelSize: AppTheme.textBody
                                        elide: Label.ElideRight
                                    }
                                    AppSwitch {
                                        checked: spaceRow.checked
                                        enabled: !app.roomInfo.editPending
                                        onToggled: {
                                            spaceRow.checked = !spaceRow.checked
                                            if (restrictedPicker.parent.restricted)
                                                restrictedPicker.apply(
                                                    app.roomInfo.joinRule)
                                        }
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: app.roomInfo.joinedSpaces.length === 0
                                wrapMode: Text.WordWrap
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                                text: qsTr("Join a space first; only spaces "
                                           + "you are in can be chosen.")
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: app.roomInfo.restrictedHasUnknownRules
                                wrapMode: Text.WordWrap
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                                text: qsTr("This room also allows joins by a "
                                           + "rule Lightning can't show. It "
                                           + "is kept when you save.")
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                visible: restrictedPicker.parent.restricted
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Others may ask to join (knock)")
                                    color: AppTheme.text
                                    font.pixelSize: AppTheme.textBody
                                    wrapMode: Text.WordWrap
                                }
                                AppSwitch {
                                    objectName: "roomKnockRestrictedSwitch"
                                    checked: app.roomInfo.joinRule === "knock_restricted"
                                    enabled: !app.roomInfo.editPending
                                    onToggled: restrictedPicker.apply(
                                                   !checked ? "knock_restricted"
                                                            : "restricted")
                                }
                            }
                            AppButton {
                                visible: !restrictedPicker.parent.restricted
                                kind: "primary"
                                size: "sm"
                                text: qsTr("Restrict to these spaces")
                                enabled: !app.roomInfo.editPending
                                         && restrictedPicker.selectedIds().length > 0
                                onClicked: restrictedPicker.apply("restricted")
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: app.roomInfo.canChangeHistoryVisibility
                        Label {
                            text: qsTr("Who can read history")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        AppComboBox {
                            id: historyCombo
                            objectName: "roomHistoryVisibilityCombo"
                            Layout.fillWidth: true
                            model: [
                                qsTr("Members, from when they were invited"),
                                qsTr("Members, from when they joined"),
                                qsTr("Members, everything"),
                                qsTr("Anyone, even without joining")
                            ]
                            readonly property var values:
                                ["invited", "joined", "shared", "world_readable"]
                            property int displayedIndex: 2
                            function refresh() {
                                var idx = values.indexOf(
                                    app.roomInfo.historyVisibility)
                                displayedIndex = idx >= 0 ? idx : 2
                            }
                            Component.onCompleted: refresh()
                            currentIndex: displayedIndex
                            enabled: !app.roomInfo.editPending
                            Connections {
                                target: app.roomInfo
                                function onMembersChanged() { historyCombo.refresh() }
                            }
                            onActivated: (index) =>
                                app.roomInfo.setHistoryVisibility(
                                    historyCombo.values[index])
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: app.roomInfo.canChangeGuestAccess
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Guests can join")
                            color: AppTheme.text
                            font.pixelSize: AppTheme.textBody
                        }
                        AppSwitch {
                            objectName: "roomGuestAccessSwitch"
                            checked: app.roomInfo.guestAccess === "can_join"
                            enabled: !app.roomInfo.editPending
                            onToggled: app.roomInfo.setGuestAccess(
                                           !checked ? "can_join" : "forbidden")
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: app.roomInfo.canChangeAlias
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            text: app.roomInfo.directoryPublished < 0
                                  ? qsTr("Listed in the room directory (checking…)")
                                  : qsTr("Listed in the room directory")
                            color: AppTheme.text
                            font.pixelSize: AppTheme.textBody
                            wrapMode: Text.WordWrap
                        }
                        AppSwitch {
                            objectName: "roomDirectorySwitch"
                            checked: app.roomInfo.directoryPublished === 1
                            enabled: !app.roomInfo.editPending
                                     && app.roomInfo.directoryPublished >= 0
                            onToggled: app.roomInfo.setDirectoryPublished(!checked)
                        }
                    }

                    Repeater {
                        model: root.accessNotes(app.roomInfo.joinRule,
                                                app.roomInfo.historyVisibility,
                                                app.roomInfo.directoryPublished)
                        delegate: RowLayout {
                            required property string modelData
                            objectName: "roomAccessNote"
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing6
                            Icon {
                                Layout.alignment: Qt.AlignTop
                                name: "info"
                                size: 14
                                color: AppTheme.textMuted
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData
                                textFormat: Text.PlainText
                                color: AppTheme.textMuted
                                font.pixelSize: AppTheme.textMeta
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: app.roomInfo.roomVersion.length > 0
                        spacing: AppTheme.spacing8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Room version %1").arg(app.roomInfo.roomVersion)
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        AppButton {
                            objectName: "roomUpgradeButton"
                            visible: app.roomInfo.canUpgradeRoom
                                     && !app.roomUpgrade.upgraded
                            kind: "secondary"
                            size: "sm"
                            text: qsTr("Upgrade room…")
                            onClicked: {
                                roomUpgradeDialog.kind = "room"
                                roomUpgradeDialog.openFor(app.roomInfo.roomId)
                            }
                        }
                    }
                    RoomUpgradeDialog { id: roomUpgradeDialog }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        visible: app.roomInfo.canChangeAlias
                        Label {
                            text: qsTr("Published address")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            AppTextField {
                                id: editAlias
                                Layout.minimumWidth: 60
                                objectName: "roomAliasField"
                                Layout.fillWidth: true
                                placeholderText: qsTr("#room-name")
                                readonly property string problem:
                                    text.trim() === app.roomInfo.canonicalAlias
                                    ? "" : app.roomInfo.aliasProblem(text)
                                // An explicit mirror, not a binding to the
                                // controller: the first keystroke would break
                                // the binding, and this panel outlives a room
                                // change, so text typed for one room could be
                                // saved to another.
                                property string authoritative: ""
                                function resetForRoom() {
                                    authoritative =
                                        app.roomInfo.canonicalAlias
                                    text = authoritative
                                }
                                function refreshAlias() {
                                    var next = app.roomInfo.canonicalAlias
                                    if (text === authoritative)
                                        text = next
                                    authoritative = next
                                }
                                Component.onCompleted: resetForRoom()
                                Connections {
                                    target: app.roomInfo
                                    function onRoomIdChanged() {
                                        editAlias.resetForRoom()
                                    }
                                    function onMembersChanged() {
                                        editAlias.refreshAlias()
                                    }
                                }
                            }
                            AppButton {
                                objectName: "roomAliasSaveButton"
                                kind: "primary"
                                text: qsTr("Save")
                                enabled: !app.roomInfo.editPending
                                         && editAlias.text.trim()
                                            !== app.roomInfo.canonicalAlias
                                         && editAlias.problem.length === 0
                                onClicked:
                                    app.roomInfo.setCanonicalAlias(
                                        editAlias.text)
                            }
                        }
                        Label {
                            objectName: "roomAliasHint"
                            visible: editAlias.problem.length > 0
                            Layout.fillWidth: true
                            text: editAlias.problem
                            textFormat: Text.PlainText
                            color: AppTheme.danger
                            font.pixelSize: AppTheme.textMeta
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            Layout.topMargin: AppTheme.spacing4
                            text: qsTr("Alternative addresses")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                        }
                        Repeater {
                            model: app.roomInfo.altAliases
                            delegate: RowLayout {
                                required property string modelData
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing8
                                Label {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: modelData
                                    color: AppTheme.text
                                    font.pixelSize: AppTheme.textBody
                                    elide: Label.ElideMiddle
                                }
                                AppButton {
                                    kind: "ghost"
                                    size: "sm"
                                    text: qsTr("Remove")
                                    enabled: !app.roomInfo.editPending
                                    onClicked: {
                                        var next = app.roomInfo.altAliases
                                                       .filter(function (a) {
                                                           return a !== modelData
                                                       })
                                        app.roomInfo.setAltAliases(next)
                                    }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AppTheme.spacing8
                            AppTextField {
                                id: newAltAlias
                                objectName: "roomAltAliasField"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 60
                                placeholderText: qsTr("#another-name")
                                readonly property string problem:
                                    app.roomInfo.aliasProblem(text)
                                onAccepted: if (addAltAlias.enabled)
                                                addAltAlias.clicked()
                            }
                            AppButton {
                                id: addAltAlias
                                kind: "secondary"
                                size: "sm"
                                text: qsTr("Add")
                                enabled: !app.roomInfo.editPending
                                         && newAltAlias.text.trim().length > 0
                                         && newAltAlias.problem.length === 0
                                onClicked: {
                                    var next = app.roomInfo.altAliases.slice()
                                    next.push(newAltAlias.text.trim())
                                    app.roomInfo.setAltAliases(next)
                                    newAltAlias.text = ""
                                }
                            }
                        }
                        Label {
                            objectName: "roomAltAliasHint"
                            visible: newAltAlias.problem.length > 0
                            Layout.fillWidth: true
                            text: newAltAlias.problem
                            textFormat: Text.PlainText
                            color: AppTheme.danger
                            font.pixelSize: AppTheme.textMeta
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            Layout.fillWidth: true
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            wrapMode: Text.WordWrap
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.textMeta
                            text: qsTr("Publishing an address lets people "
                                       + "find and join this room by name. "
                                       + "Leave it empty to remove it.")
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: AppTheme.spacing12
                    spacing: AppTheme.spacing8

                    AppButton {
                        objectName: "roomInfoLeaveButton"
                        kind: "danger"
                        text: qsTr("Leave room")
                        enabled: !app.roomInfo.leavePending
                        onClicked: leaveConfirm.openFor(
                                       app.roomInfo.roomId,
                                       root.roomData.name || "",
                                       app.roomInfo.joinedCount,
                                       app.roomInfo.joinRule,
                                       app.roomInfo.invitedCount,
                                       leaveConfirm.soleAdminOf(app.roomInfo,
                                                                  app.roomInfo.roomId))
                    }
                    AppButton {
                        objectName: "roomCloseButton"
                        visible: app.roomInfo.canChangeJoinRule
                                 && !!app.roomClosure
                        kind: "danger"
                        text: qsTr("Close room…")
                        onClicked: roomCloseDialog.openFor(
                                       app.roomInfo.roomId,
                                       root.roomData.name || "",
                                       root.roomData.isSpace === true, "close")
                    }
                    AppButton {
                        objectName: "roomDeleteButton"
                        visible: !!app.roomClosure
                                 && app.roomClosure.serverAdmin === "yes"
                        kind: "danger"
                        text: qsTr("Delete from server…")
                        onClicked: roomCloseDialog.openFor(
                                       app.roomInfo.roomId,
                                       root.roomData.name || "",
                                       root.roomData.isSpace === true, "delete")
                    }
                    RoomCloseDialog { id: roomCloseDialog }
                    Label {
                        visible: app.roomInfo.leaveError.length > 0
                        Layout.fillWidth: true
                        text: app.roomInfo.leaveError
                        color: AppTheme.danger
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        wrapMode: Text.WordWrap
                        font.pixelSize: AppTheme.textBody
                    }
                }
                Item { Layout.preferredHeight: AppTheme.spacing16 }
            }
        }

        ColumnLayout {
            visible: root.section === "pinned" && root.pinnedAvailable
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing8

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: AppTheme.spacing12
                Layout.bottomMargin: 0
                spacing: AppTheme.spacing8
                Label {
                    Layout.fillWidth: true
                    text: app.pinned.total === 0
                          ? qsTr("No pinned messages")
                          : qsTr("%n pinned message(s)", "", app.pinned.total)
                    color: AppTheme.textSecondary
                    font.pixelSize: AppTheme.textBody
                    font.weight: AppTheme.weightStrong
                }
                AppBusyIndicator {
                    visible: app.pinned.loading || app.pinned.pending
                    running: visible
                    size: 16
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                visible: app.pinned.truncated
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.textMeta
                text: qsTr("Showing the most recent pins. This room pins "
                           + "more than Lightning loads at once.")
            }

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                visible: app.pinned.error.length > 0
                lineHeight: AppTheme.lineHeightBody
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.WordWrap
                text: app.pinned.error
                color: AppTheme.danger
                font.pixelSize: AppTheme.textBody
            }

            ListView {
                objectName: "pinnedMessagesList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 2
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                SmoothWheelArea {}
                model: {
                    var out = []
                    var src = app.pinned.entries
                    for (var i = src.length - 1; i >= 0; --i)
                        out.push(src[i])
                    return out
                }
                delegate: Item {
                    id: pinDelegate
                    width: ListView.view.width
                    height: pinRow.implicitHeight + AppTheme.spacing16

                    required property var modelData
                    required property int index

                    readonly property bool resolved:
                        pinDelegate.modelData.available === true
                    readonly property string senderName:
                        pinDelegate.modelData.senderDisplayName
                        || (pinDelegate.modelData.sender || "")
                    readonly property string kind:
                        pinDelegate.modelData.kind || ""

                    objectName: "pinnedMessageRow"
                    activeFocusOnTab: pinDelegate.resolved
                    Accessible.role: Accessible.Button
                    Accessible.name: pinDelegate.resolved
                                     ? ((pinDelegate.modelData.preview || "")
                                            .length > 0
                                        ? qsTr("%1: %2")
                                              .arg(pinDelegate.senderName)
                                              .arg(pinDelegate.modelData.preview)
                                        : pinDelegate.senderName)
                                     : qsTr("Message unavailable")
                    onActiveFocusChanged: {
                        if (activeFocus)
                            ListView.view.positionViewAtIndex(
                                pinDelegate.index, ListView.Contain)
                    }
                    Keys.onReturnPressed: pinDelegate.activate()
                    Keys.onEnterPressed: pinDelegate.activate()
                    Keys.onSpacePressed: pinDelegate.activate()
                    function activate() {
                        if (pinDelegate.resolved)
                            root.jumpToEventRequested(
                                pinDelegate.modelData.eventId)
                    }

                    HoverHandler { id: pinHover }
                    Rectangle {
                        anchors.fill: parent
                        color: AppTheme.hover
                        visible: pinHover.hovered && pinDelegate.resolved
                    }
                    Rectangle {
                        objectName: "pinnedMessageFocusRing"
                        anchors.fill: parent
                        color: "transparent"
                        border.width: 2
                        border.color: AppTheme.focusRing
                        radius: AppTheme.radiusSm
                        visible: pinDelegate.activeFocus
                    }
                    TapHandler {
                        enabled: pinDelegate.resolved
                        onTapped: root.jumpToEventRequested(
                                      pinDelegate.modelData.eventId)
                    }

                    RowLayout {
                        id: pinRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.rightMargin: AppTheme.spacing8
                        spacing: AppTheme.spacing8

                        Avatar {
                            size: 28
                            circle: true
                            visible: pinDelegate.resolved
                            name: pinDelegate.senderName
                            mxc: pinDelegate.modelData.senderAvatarUrl || ""
                            colorKey: pinDelegate.modelData.sender || ""
                        }
                        Icon {
                            visible: !pinDelegate.resolved
                            name: "warning"
                            size: 20
                            color: AppTheme.textMuted
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing8
                                Label {
                                    Layout.fillWidth: true
                                    elide: Label.ElideRight
                                    text: pinDelegate.resolved
                                          ? pinDelegate.senderName
                                          : qsTr("Message unavailable")
                                    textFormat: Text.PlainText
                                    color: pinDelegate.resolved
                                           ? AppTheme.userColor(
                                                 pinDelegate.modelData.sender || "")
                                           : AppTheme.textPrimary
                                    font.pixelSize: AppTheme.textBody
                                    font.weight: AppTheme.weightStrong
                                }
                                Label {
                                    visible: pinDelegate.resolved
                                             && pinDelegate.modelData
                                                .timestampMs > 0
                                    text: new Date(pinDelegate.modelData
                                                   .timestampMs)
                                          .toLocaleDateString(
                                              Qt.locale(AppLocale.name), Locale.ShortFormat)
                                    textFormat: Text.PlainText
                                    color: AppTheme.textMuted
                                    font.pixelSize: AppTheme.textMeta
                                }
                            }
                            Label {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                elide: Label.ElideRight
                                maximumLineCount: 2
                                lineHeight: AppTheme.lineHeightBody
                                lineHeightMode: Text.ProportionalHeight
                                wrapMode: Text.WordWrap
                                color: AppTheme.textSecondary
                                font.pixelSize: AppTheme.textBody
                                text: {
                                    if (!pinDelegate.resolved) {
                                        return qsTr("It may have been deleted, "
                                                    + "or this account cannot "
                                                    + "see it.")
                                    }
                                    var k = pinDelegate.kind
                                    if (k === "redacted")
                                        return qsTr("Message deleted")
                                    if (k === "encrypted")
                                        return qsTr("Can't decrypt this yet")
                                    var p = pinDelegate.modelData.preview || ""
                                    if (k === "image")
                                        return p.length > 0
                                            ? qsTr("Image · %1").arg(p)
                                            : qsTr("Image")
                                    if (k === "video")
                                        return p.length > 0
                                            ? qsTr("Video · %1").arg(p)
                                            : qsTr("Video")
                                    if (k === "audio")
                                        return p.length > 0
                                            ? qsTr("Audio · %1").arg(p)
                                            : qsTr("Audio")
                                    if (k === "file")
                                        return p.length > 0
                                            ? qsTr("File · %1").arg(p)
                                            : qsTr("File")
                                    if (k === "poll")
                                        return p.length > 0
                                            ? qsTr("Poll · %1").arg(p)
                                            : qsTr("Poll")
                                    if (k === "sticker")
                                        return p.length > 0
                                            ? qsTr("Sticker · %1").arg(p)
                                            : qsTr("Sticker")
                                    return p.length > 0 ? p : qsTr("Message")
                                }
                            }
                        }

                        IconButton {
                            iconName: "close"
                            iconSize: 18
                            implicitWidth: 28
                            implicitHeight: 28
                            visible: app.pinned.canPin
                                     && (pinHover.hovered
                                         || !pinDelegate.resolved)
                            enabled: !app.pinned.pending
                            Accessible.name: qsTr("Unpin this message")
                            ToolTip.text: qsTr("Unpin")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: app.pinned.unpin(
                                           pinDelegate.modelData.eventId)
                        }
                    }
                }
            }
        }

        ColumnLayout {
            id: memberSection
            visible: root.section === "people"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: AppTheme.spacing4

            property int rosterTick: 0
            Connections {
                target: app.roomInfo
                function onMembersChanged() { memberSection.rosterTick++ }
            }

            property string membership: "joined"
            property bool alphabetical: false

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: AppTheme.spacing8
                Layout.rightMargin: AppTheme.spacing8
                Layout.topMargin: AppTheme.spacing6
                spacing: AppTheme.spacing4

                AppTextField {
                    id: memberSearch
                    Layout.fillWidth: true
                    Layout.minimumWidth: 40
                    implicitHeight: Math.max(30, AppTheme.scaled(AppTheme.textBody) + 16)
                    searchIcon: true
                    clearButton: true
                    placeholderText: qsTr("Type name…")
                    onTextChanged: root.memberFilter = text
                }
                IconButton {
                    size: "sm"
                    iconName: "person_search"
                    active: memberSection.membership !== "joined"
                    Accessible.name: qsTr("Filter members by membership")
                    ToolTip.text: memberSection.membership === "joined"
                                  ? qsTr("Showing joined members — click for invited")
                                  : memberSection.membership === "invited"
                                    ? qsTr("Showing invited members — click for banned")
                                    : memberSection.membership === "banned"
                                      ? qsTr("Showing banned members — click for everyone")
                                      : qsTr("Showing everyone — click for joined")
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    onClicked: {
                        memberSection.membership =
                            memberSection.membership === "joined" ? "invited"
                            : memberSection.membership === "invited" ? "banned"
                            : memberSection.membership === "banned" ? ""
                            : "joined"
                    }
                }
                IconButton {
                    size: "sm"
                    iconName: "unfold_more"
                    active: memberSection.alphabetical
                    Accessible.name: qsTr("Sort members alphabetically")
                    ToolTip.text: memberSection.alphabetical
                                  ? qsTr("Sorted A to Z — click for role order")
                                  : qsTr("Sorted by role — click for A to Z")
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    onClicked: memberSection.alphabetical = !memberSection.alphabetical
                }
                IconButton {
                    size: "sm"
                    iconName: "person_add"
                    visible: app.roomInfo.canInvite
                    Accessible.name: qsTr("Invite people to this room")
                    ToolTip.text: qsTr("Invite people")
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    onClicked: inviteDialog.openFor(app.roomInfo.roomId)
                }
            }

            Loader {
                active: app.roomInfo.loading
                visible: active
                Layout.leftMargin: AppTheme.spacing12
                sourceComponent: Label {
                    text: qsTr("Loading members…")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    wrapMode: Text.WordWrap
                }
            }
            Loader {
                active: app.roomInfo.truncated
                visible: active
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                Layout.fillWidth: true
                sourceComponent: Label {
                    text: qsTr("Showing the first %1 members of %2.")
                          .arg(app.roomInfo.members.length)
                          .arg(app.roomInfo.joinedCount + app.roomInfo.invitedCount)
                    color: AppTheme.textMuted
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    wrapMode: Text.WordWrap
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                }
            }

            Loader {
                objectName: "memberEmptyState"
                active: app.roomInfo.roomId !== "" && !app.roomInfo.loading
                        && memberList.count === 0
                visible: active
                Layout.leftMargin: AppTheme.spacing12
                Layout.rightMargin: AppTheme.spacing12
                Layout.fillWidth: true
                sourceComponent: Label {
                    text: root.memberFilter.length > 0
                          ? qsTr("No members match this name.")
                          : memberSection.membership === "invited"
                            ? qsTr("No invited members.")
                            : memberSection.membership === "banned"
                              ? qsTr("No banned members.")
                              : qsTr("No members to show.")
                    textFormat: Text.PlainText
                    color: AppTheme.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                }
            }

            ListView {
                id: memberList
                objectName: "memberRoleList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 3
                model: {
                    var _t = memberSection.rosterTick
                    return app.roomInfo.memberRoleRows(root.memberFilter,
                                                       memberSection.membership,
                                                       memberSection.alphabetical)
                }
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
                SmoothWheelArea {}

                delegate: Loader {
                    id: memberLoader
                    width: memberList.width
                    required property var modelData
                    required property int index
                    sourceComponent: modelData.kind === "header"
                                     ? roleHeaderComponent : memberRowComponent

                    Component {
                        id: roleHeaderComponent
                        Item {
                            width: memberList.width
                            height: Math.max(24, AppTheme.scaled(AppTheme.textSubtitle) + 10)
                            MenuSectionLabel {
                                color: AppTheme.sidePanelSection
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: AppTheme.spacing12
                                anchors.rightMargin: AppTheme.spacing12
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 2
                                font.pixelSize: AppTheme.scaled(AppTheme.textSubtitle)
                                text: qsTr("%1 — %2")
                                          .arg(memberLoader.modelData.label)
                                          .arg(memberLoader.modelData.count)
                            }
                        }
                    }

                    Component {
                        id: memberRowComponent
                        ItemDelegate {
                            width: memberList.width
                            height: Math.max(34, AppTheme.scaled(AppTheme.textTitle) + 16)
                            padding: 0
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            readonly property var member: memberLoader.modelData
                            Accessible.name: member.displayName.length > 0
                                             ? qsTr("%1 (%2), %3")
                                                   .arg(member.displayName)
                                                   .arg(member.userId)
                                                   .arg(member.roleLabel)
                                             : member.userId
                            onClicked: memberProfile.openFor(member, parent)
                            onActiveFocusChanged: {
                                if (activeFocus)
                                    memberList.positionViewAtIndex(
                                        memberLoader.index, ListView.Contain)
                            }
                            Keys.onReturnPressed: (event) => {
                                clicked(); event.accepted = true
                            }
                            Keys.onEnterPressed: (event) => {
                                clicked(); event.accepted = true
                            }
                            Keys.onMenuPressed: root.openMemberMenu(
                                member, parent, width / 2, height / 2)
                            Keys.onPressed: (event) => {
                                if (event.key === Qt.Key_F10
                                        && (event.modifiers & Qt.ShiftModifier)) {
                                    root.openMemberMenu(member, parent,
                                                        width / 2, height / 2)
                                    event.accepted = true
                                }
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                onTapped: (eventPoint) => root.openMemberMenu(
                                    member, parent, eventPoint.position.x,
                                    eventPoint.position.y)
                            }
                            background: Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacing8
                                anchors.rightMargin: AppTheme.spacing8
                                radius: AppTheme.radiusSm
                                color: parent.hovered ? AppTheme.memberRowHover
                                                      : "transparent"
                                border.width: parent.visualFocus ? 2 : 0
                                border.color: AppTheme.focusRing
                            }
                            contentItem: RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacing12
                                anchors.rightMargin: AppTheme.spacing12
                                spacing: AppTheme.spacing8
                                Avatar {
                                    readonly property int px:
                                        Math.max(24, AppTheme.scaled(AppTheme.textTitle) + 8)
                                    width: px; height: px
                                    size: px
                                    name: member.displayName.length > 0
                                          ? member.displayName : member.userId
                                    mxc: member.avatarUrl || ""
                                    colorKey: member.userId || ""

                                    PresenceDot {
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        anchors.margins: -1
                                        dotSize: 8
                                        ring: AppTheme.sidePanel
                                        userId: root.visible
                                                && root.section === "people"
                                                && member.membership !== "banned"
                                                ? (member.userId || "") : ""
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: member.displayName.length > 0
                                          ? member.displayName : member.userId
                                    textFormat: Text.PlainText
                                    color: AppTheme.userColor(member.userId || "")
                                    font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                                    elide: Label.ElideRight
                                }
                                StatusChip {
                                    visible: member.membership === "invited"
                                    tone: "warning"
                                    label: qsTr("Invited")
                                }
                                StatusChip {
                                    visible: member.membership === "banned"
                                    tone: "danger"
                                    label: qsTr("Banned")
                                }
                            }
                            PlainToolTip {
                                objectName: "memberUserIdTip"
                                visible: parent.hovered && !memberProfile.opened
                                delay: 600
                                text: member.userId
                                x: parent.width - width - AppTheme.spacing12
                                y: (parent.height - height) / 2
                            }
                        }
                    }
                }
            }
        }

        Flickable {
            visible: root.section === "widgets"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: widgetCol.implicitHeight + AppTheme.spacing16 * 2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {}
            SmoothWheelArea {}
            ColumnLayout {
                id: widgetCol
                x: AppTheme.spacing12
                y: AppTheme.spacing12
                width: parent.width - AppTheme.spacing12 * 2
                spacing: AppTheme.spacing8

                Label {
                    visible: app.widgets.supported && app.widgets.count === 0
                    Layout.fillWidth: true
                    text: qsTr("No widgets in this room.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    wrapMode: Text.Wrap
                }
                Label {
                    objectName: "roomInfoWidgetWriteError"
                    Layout.fillWidth: true
                    visible: app.widgets.lastWriteError.length > 0
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: AppTheme.danger
                    font.pixelSize: AppTheme.textMeta
                    text: app.widgets.lastWriteError === "forbidden"
                          ? qsTr("You do not have permission to change widgets here.")
                          : qsTr("Could not change the widgets (%1).").arg(app.widgets.lastWriteError)
                }
                AppButton {
                    id: addWidgetButton
                    objectName: "roomInfoAddWidgetButton"
                    visible: app.widgets.supported && app.widgets.canManage
                    enabled: !app.widgets.writing
                    text: qsTr("Add widget…")
                    iconName: "add"
                    onClicked: addWidgetDialog.openDialog()
                }
                Repeater {
                    model: app.widgets.supported ? app.widgets : null
                    RowLayout {
                        id: widgetRow
                        required property int index
                        required property string name
                        required property string kind
                        required property string refusal
                        required property bool openable
                        required property bool removable
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing8
                        Icon {
                            name: "explore"
                            size: 18
                            color: widgetRow.openable ? AppTheme.icon
                                                      : AppTheme.textDisabled
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 2
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: widgetRow.name
                                color: AppTheme.text
                                elide: Label.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                text: widgetRow.openable
                                      ? widgetRow.kind
                                      : app.widgets.refusalText(widgetRow.refusal)
                                color: widgetRow.openable
                                       ? AppTheme.textMuted
                                       : AppTheme.danger
                                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                            }
                        }
                        AppButton {
                            objectName: "roomInfoRemoveWidgetButton"
                            visible: app.widgets.canManage && widgetRow.removable
                            enabled: !app.widgets.writing
                            kind: "ghost"
                            size: "sm"
                            text: qsTr("Remove")
                            Layout.alignment: Qt.AlignTop
                            onClicked: {
                                removeWidgetConfirm.returnFocusItem = this
                                removeWidgetConfirm.openFor(widgetRow.index,
                                                            widgetRow.name || "")
                            }
                        }
                        AppButton {
                            objectName: "roomInfoOpenWidget"
                            text: qsTr("Open")
                            enabled: widgetRow.openable
                            onClicked: {
                                widgetSheet.returnFocusItem = this
                                widgetSheet.openFor(
                                    widgetRow.index,
                                    app.widgets.rowAt(widgetRow.index))
                            }
                        }
                    }
                }
                WidgetOpenSheet { id: widgetSheet }
            }
        }

        MediaBrowser {
            objectName: "mediaBrowser"
            visible: root.section === "media"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: app.mediaHistory
            roomId: app.roomInfo.roomId
            onOpenImagesRequested: (entries, index) =>
                root.openImagesRequested(entries, index)
            onJumpToEventRequested: (eventId) =>
                root.jumpToEventRequested(eventId)
        }
    }
    property var roomPermissionsDialog: null
    Component {
        id: roomPermissionsDialogComponent
        SpaceSettingsDialog {
            objectName: "roomPermissionsDialog"
        }
    }

    Dialog {
        id: removeWidgetConfirm
        objectName: "roomInfoRemoveWidgetConfirmDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))
        modal: true
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape
        title: qsTr("Remove widget?")
        property int pendingIndex: -1
        property string pendingName: ""
        property Item returnFocusItem: null
        onClosed: Qt.callLater(function() {
            if (removeWidgetConfirm.returnFocusItem
                    && removeWidgetConfirm.returnFocusItem.visible)
                removeWidgetConfirm.returnFocusItem.forceActiveFocus(Qt.PopupFocusReason)
        })
        function openFor(index, name) {
            pendingIndex = index
            pendingName = name
            open()
        }
        background: Rectangle {
            color: AppTheme.surface
            border.color: AppTheme.border
            radius: AppTheme.radiusLg
        }
        contentItem: ColumnLayout {
            spacing: AppTheme.spacing12
            Label {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                text: removeWidgetConfirm.pendingName.length > 0
                      ? qsTr("Remove \"%1\" from this room? Everyone here loses it, and it cannot be undone.").arg(removeWidgetConfirm.pendingName)
                      : qsTr("Remove this widget from the room? Everyone here loses it, and it cannot be undone.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "roomInfoRemoveWidgetConfirmCancel"
                    text: qsTr("Cancel")
                    onClicked: removeWidgetConfirm.close()
                }
                Button {
                    objectName: "roomInfoRemoveWidgetConfirmAccept"
                    text: qsTr("Remove")
                    onClicked: {
                        removeWidgetConfirm.close()
                        app.widgets.removeWidget(removeWidgetConfirm.pendingIndex)
                    }
                }
            }
        }
    }
    LeaveRoomConfirmDialog {
        id: leaveConfirm
        objectName: "roomInfoLeaveConfirmDialog"
        onConfirmed: app.roomInfo.leaveRoom()
    }

    PolicyListDialog {
        id: policyListDialog
    }
    AddWidgetDialog {
        id: addWidgetDialog
        returnFocusItem: addWidgetButton
    }
}
