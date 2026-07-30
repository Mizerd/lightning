import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.7 design shell: the room-list column. Workspace header (active Space
// name), search with a ⌘K hint, DM / Room sections, room rows. The account
// entry point lives on the SpacesRail; this column has no footer.
Rectangle {
    id: root
    color: AppTheme.sidebar

    // v0.7.1: entry point for the Home surface's and the rail's create
    // actions, routed here by MainScreen so they reuse this column's shared
    // new-conversation dialog. mode is "dm", "room" or "space"; options
    // optionally carries {addToSpace: bool}.
    function startConversation(mode, options) {
        newConversationDialog.openDialog(mode, options)
    }

    // Development-only: locate a descendant by objectName across both the
    // visual children (Item-derived) and the default-property data list — a
    // Menu/Popup (e.g. RoomDelegate's roomMenu) is not an Item, so it never
    // appears in Item.children, only in Item.data.
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
        function onDemoOpenRoomContextMenu() {
            // The currently-selected row (RoomDelegate's own `selected`
            // property, bound above to model.roomId === app.currentRoomId)
            // — no model-index lookup needed.
            if (!roomList.contentItem) return
            var kids = roomList.contentItem.children
            for (var i = 0; i < kids.length; ++i) {
                if (kids[i] && kids[i].selected === true) {
                    var menu = root.findDemoDescendant(kids[i], "roomContextMenu")
                    if (menu && menu.popup) menu.popup()
                    return
                }
            }
        }
        function onDemoOpenNewConversation() {
            newConversationDialog.openDialog()
            // Seed the omnibox so the "#name" create-room suggestion row
            // (SPEC 1u) actually renders instead of an empty starting
            // state. dmUserPicker lives behind the DM tab's Loader
            // (resetAll()'s default mode), so the search is deferred one
            // tick past open().
            Qt.callLater(function() {
                var picker = root.findDemoDescendant(newConversationDialog, "dmUserPicker")
                if (picker)
                    picker.searchText = "launch-crew"
            })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Workspace header ─────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: workspaceLabel.implicitHeight + AppTheme.spacing12 * 2

            Label {
                id: workspaceLabel
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing12
                    rightMargin: AppTheme.spacing12
                }
                text: {
                    if (!app.spaces)
                        return qsTr("Lightning")
                    var id = app.spaces.activeSpaceId
                    if (id === "" || id === undefined)
                        return qsTr("Lightning")
                    if (id === "@orphans")
                        return qsTr("Other rooms")
                    return app.spaces.spaceName(id) || qsTr("Lightning")
                }
                color: AppTheme.textPrimary
                font.pixelSize: AppTheme.fontRoomTitle
                font.weight: Font.ExtraBold
                elide: Label.ElideRight
            }
        }

        // ── Search bar + new-conversation button ─────────────────────────
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: searchRow.implicitHeight + AppTheme.spacing8 * 2

            RowLayout {
                id: searchRow
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing8; rightMargin: AppTheme.spacing8
                }
                spacing: AppTheme.spacing8

                TextField {
                    id: roomSearch
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search")
                    onTextChanged: app.roomList.searchQuery = text
                    font.pixelSize: AppTheme.fontSizeS
                    leftPadding: searchIcon.width + AppTheme.spacing12
                    rightPadding: kbdHint.width + AppTheme.spacing12
                    background: Rectangle {
                        color: AppTheme.hover
                        border.color: roomSearch.activeFocus ? AppTheme.focusRing : "transparent"
                        border.width: roomSearch.activeFocus ? 2 : 1
                        radius: AppTheme.radiusMd
                    }

                    // Leading search glyph (handoff §2 search field).
                    Icon {
                        id: searchIcon
                        anchors.left: parent.left
                        anchors.leftMargin: AppTheme.spacing8
                        anchors.verticalCenter: parent.verticalCenter
                        name: "search"
                        size: 18
                        color: AppTheme.textMuted
                    }

                    // ⌘K-style keycap hint for the quick switcher.
                    Rectangle {
                        id: kbdHint
                        anchors.right: parent.right
                        anchors.rightMargin: AppTheme.spacing6
                        anchors.verticalCenter: parent.verticalCenter
                        width: kbdLabel.implicitWidth + AppTheme.spacing8
                        height: kbdLabel.implicitHeight + AppTheme.spacing4
                        radius: AppTheme.radiusSm
                        color: AppTheme.sidebar
                        border.color: AppTheme.border
                        visible: !roomSearch.activeFocus
                        Label {
                            id: kbdLabel
                            anchors.centerIn: parent
                            text: qsTr("Ctrl K")
                            font.family: AppTheme.monoFont
                            font.pixelSize: AppTheme.fontCaption
                            color: AppTheme.textMuted
                        }
                    }
                }

                // v0.5.9: start a DM or create a room (Rust backend only —
                // the controller reports unsupported backends itself).
                IconButton {
                    id: newConversationBtn
                    visible: app.loggedIn && app.conversations.supported
                    implicitWidth: 30; implicitHeight: 30
                    radius: AppTheme.radiusMd
                    iconName: "add"
                    iconSize: 18
                    Accessible.name: qsTr("Start a new conversation")
                    ToolTip.text: qsTr("New conversation")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: newConversationDialog.openDialog()
                }
            }
        }

        NewConversationDialog {
            id: newConversationDialog
            parent: Overlay.overlay
        }

        // v0.6.5 (SPEC 1d): RoomDelegate stays signal-only, so the actual
        // clipboard write and the Leave-room confirmation/error surfaces are
        // ONE shared instance per view here in the host, exactly like the
        // shared reaction picker/profile popover pattern used elsewhere.
        TextEdit {
            id: roomLinkClipboard
            visible: false
            width: 0
            height: 0
        }

        // Leave confirmation — Cancel is the default safe action. Modeled on
        // RoomInfoPanel.qml's own leave-confirm dialog (same copy/shape),
        // generalized to name the room being left from the list menu.
        Dialog {
            id: leaveRoomConfirm
            objectName: "leaveRoomConfirmDialog"
            parent: Overlay.overlay
            anchors.centerIn: parent
            width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))
            modal: true
            title: qsTr("Leave room?")
            standardButtons: Dialog.NoButton
            closePolicy: Popup.CloseOnEscape

            property string pendingRoomId: ""
            property string pendingRoomName: ""

            function openFor(roomId, name) {
                pendingRoomId = roomId
                pendingRoomName = name
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
                    text: qsTr("You will stop receiving messages from \"%1\". "
                               + "Server history is not deleted, and you can "
                               + "be invited again later.")
                               .arg(leaveRoomConfirm.pendingRoomName)
                    wrapMode: Text.WordWrap
                    color: AppTheme.textPrimary
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    AppButton {
                        text: qsTr("Cancel")
                        focus: true
                        onClicked: leaveRoomConfirm.close()
                    }
                    AppButton {
                        kind: "danger"
                        text: qsTr("Leave room")
                        onClicked: {
                            leaveRoomConfirm.close()
                            app.roomInfo.leaveRoom(leaveRoomConfirm.pendingRoomId)
                        }
                    }
                }
            }
        }

        // v0.6.5: an ad-hoc leave from the room-list menu is not the same
        // pending/error state as the Room Information panel's own Leave
        // button (RoomInfoController tracks it separately so the two paths
        // never corrupt each other) — surface its honest failure here
        // instead of leaving it silent.
        Dialog {
            id: leaveRoomFailedDialog
            objectName: "leaveRoomFailedDialog"
            parent: Overlay.overlay
            anchors.centerIn: parent
            width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))
            modal: true
            standardButtons: Dialog.Ok
            property string roomLabel: ""
            property string messageText: ""
            title: qsTr("Couldn't leave \"%1\"").arg(leaveRoomFailedDialog.roomLabel)
            contentItem: Label {
                text: leaveRoomFailedDialog.messageText
                wrapMode: Text.WordWrap
                color: AppTheme.textPrimary
            }
        }
        Connections {
            target: app.roomInfo
            function onRoomLeaveFailed(roomId, message) {
                var info = app.roomList.findRoom(roomId)
                leaveRoomFailedDialog.roomLabel =
                    (info && info.name) ? info.name : roomId
                leaveRoomFailedDialog.messageText = message
                leaveRoomFailedDialog.open()
            }
        }

        // ── Room list with DM / ROOMS section headers ─────────────────────
        ListView {
            id: roomList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: app.roomList
            currentIndex: -1
            spacing: 0
            // Instantiate delegates a little past the viewport so their
            // avatars start fetching before the row scrolls into view.
            // Bounded prefetch: roughly one extra screen of rows.
            cacheBuffer: 600
            // Fast-scroll: recycle row delegates instead of re-creating
            // them (RoomDelegate keeps no per-instance state that could
            // leak across model rows).
            reuseItems: true

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            // Section grouping driven by the "category" role from RoomListModel.
            // C++ sorts DMs first so "dm" section appears above "room" section.
            section.property: "category"
            section.criteria: ViewSection.FullString
            section.delegate: Rectangle {
                required property string section
                width: roomList.width
                height: 28
                color: AppTheme.sidebar

                Label {
                    anchors {
                        left: parent.left; verticalCenter: parent.verticalCenter
                        leftMargin: AppTheme.spacing12
                    }
                    text: section === "invite" ? qsTr("INVITES")
                          : section === "dm" ? qsTr("PEOPLE") : qsTr("ROOMS")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.fontSizeXS
                    font.weight: Font.ExtraBold
                    font.letterSpacing: 1.2
                }
            }

            delegate: RoomDelegate {
                width: ListView.view.width
                selected: model.roomId === app.currentRoomId
                onClicked: if (model.membership === "joined") app.openRoom(model.roomId)
                onAcceptInvite: app.roomList.acceptInvite(model.roomId)
                onRejectInvite: app.roomList.rejectInvite(model.roomId)
                onMarkRead: app.roomList.markRoomRead(model.roomId)
                onMarkUnread: app.roomList.markRoomUnread(model.roomId)
                onSetNotificationMode: (mode) =>
                    app.settings.setRoomNotificationMode(model.roomId, mode)
                onCopyRoomLink: {
                    var link = app.roomList.roomPermalink(
                        model.roomId, model.canonicalAlias || "")
                    if (link.length > 0) {
                        roomLinkClipboard.text = link
                        roomLinkClipboard.selectAll()
                        roomLinkClipboard.copy()
                        roomLinkClipboard.text = ""
                    }
                }
                onLeaveRoomRequested:
                    leaveRoomConfirm.openFor(model.roomId, model.name)
            }

            // Empty / loading state
            Label {
                anchors.centerIn: parent
                width: parent.width - AppTheme.spacing24 * 2
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: roomList.count === 0
                text: {
                    if (!app.loggedIn) return qsTr("Sign in to see rooms")
                    if (!app.initialSyncDone) return qsTr("Loading rooms…")
                    if (app.spaces && app.spaces.activeSpaceId &&
                            app.spaces.activeSpaceId !== "" &&
                            app.spaces.activeSpaceId !== "@orphans")
                        return qsTr("No rooms in this Space")
                    return qsTr("No joined rooms")
                }
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.fontSizeS
            }
        }

        // The account entry point lives on the SpacesRail (design shell);
        // this column intentionally has no footer.
    }
}
