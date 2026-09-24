import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Room-list column: workspace header, search with a quick-switcher hint, filter
// chips and the room list. The account entry point lives on the SpacesRail;
// this column has no footer.
Rectangle {
    id: root
    color: AppTheme.sidebar

    // Entry point for Home's and the rail's create actions (routed by
    // MainScreen) into this column's shared new-conversation dialog. mode:
    // "dm", "room" or "space"; options may carry {addToSpace: bool}.
    function startConversation(mode, options) {
        newConversationDialog.openDialog(mode, options)
    }

    // The Activity Center for the shell's shortcut. The panel belongs to this
    // host, so MainScreen asks through a function rather than reaching into its
    // ids. The header bell opens the same one.
    function openActivityCenter() {
        activityPanel.openPanel()
    }

    // Open Discover / Join ("browse" | "address").
    function openDiscover(startMode) {
        discoverJoinDialog.openDialog(startMode)
    }

    // The Channels layout's "Message Search" row. The dialog is MainScreen's
    // (Ctrl+Shift+F opens the same one), so ask by signal.
    signal messageSearchRequested()

    // A Matrix room link from a message: resolve it in the Address tab; a
    // joined room opens directly (and jumps when the link has an event id).
    function openDiscoverForLink(link) {
        discoverJoinDialog.openForLink(link)
    }

    // Development-only: find a descendant by objectName through children and
    // the default data list (a Menu/Popup is not an Item and appears only in
    // data).
    function findDemoDescendant(obj, name) {
        if (!obj) return null
        if (obj.objectName === name) return obj
        // Dialogs/Popups are not Items; their subtree hangs off contentItem.
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

    // Screenshot-demo hooks; inert in a non-demo build.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenRoomContextMenu() {
            // The selected row, via RoomDelegate's own `selected`.
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
            // Seed the omnibox so the "#name" create suggestion renders;
            // deferred one tick because the picker sits behind the DM tab's
            // Loader.
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

        // Workspace header, 60px with a trailing hairline to match the other
        // column headers.
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: Math.max(AppTheme.headerBandHeight,
                headerRow.implicitHeight + AppTheme.spacing12 * 2)

            // Wordmark: "Lightning" with the bolt trailing on the same
            // baseline. The label is not fillWidth and the row is anchored left
            // only, so the RowLayout takes its implicit width and the bolt hugs
            // the label instead of being pushed to the far edge. maximumWidth
            // lets a long Space name elide.
            RowLayout {
                id: headerRow
                anchors {
                    left: parent.left
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing12
                }
                spacing: AppTheme.spacing6

                Label {
                    id: workspaceLabel
                    objectName: "workspaceLabel"
                    Layout.alignment: Qt.AlignVCenter
                    Layout.maximumWidth: Math.max(0, root.width
                        - AppTheme.spacing12 * 2 - headerRow.spacing
                        - wordmarkBolt.implicitWidth)
                    // In Channels the header names the current view (Home,
                    // Direct Messages or the Space); Classic keeps the wordmark
                    // at Home.
                    readonly property bool channelsLayout:
                        app.settings && app.settings.roomNavigationLayout === 1
                    // spaceName() is a method call and app.spaces is constant,
                    // so the binding would evaluate once; a new Space is named
                    // just after its room appears and would stay "Empty Room".
                    // Read this counter so it re-evaluates.
                    property int spacesRevision: 0
                    Connections {
                        target: app.spaces
                        function onSpacesChanged() {
                            workspaceLabel.spacesRevision++
                        }
                    }
                    text: {
                        void spacesRevision
                        if (!app.spaces)
                            return qsTr("Lightning")
                        var id = app.spaces.activeSpaceId
                        if (id === "" || id === undefined)
                            return channelsLayout ? qsTr("Home") : qsTr("Lightning")
                        if (id === "@people")
                            return qsTr("Direct Messages")
                        if (id === "@orphans")
                            return qsTr("Other rooms")
                        return app.spaces.spaceName(id) || qsTr("Lightning")
                    }
                    color: AppTheme.textPrimary
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightBold
                    elide: Label.ElideRight

                    // With a real Space selected, the title also opens its
                    // Space Home.
                    readonly property bool spaceLink:
                        app.spaces !== null
                        && app.spaces.activeSpaceId.length > 0
                        && app.spaces.activeSpaceId.charAt(0) === "!"
                    HoverHandler {
                        id: workspaceHover
                        enabled: workspaceLabel.spaceLink
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        enabled: workspaceLabel.spaceLink
                        onTapped: app.openSpaceHome(app.spaces.activeSpaceId)
                    }
                    ToolTip.visible: workspaceHover.hovered
                    ToolTip.text: qsTr("Open Space overview")
                    ToolTip.delay: 500
                }

                // The bolt mark: a bare glyph sized near the label's cap
                // height; never focusable or resized by hover.
                Icon {
                    id: wordmarkBolt
                    objectName: "workspaceBoltMark"
                    Layout.alignment: Qt.AlignVCenter
                    name: "bolt"
                    size: 15
                    color: AppTheme.wordmarkBolt
                    // Decorative: announce the workspace name only.
                    Accessible.ignored: true
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.border
        }

        // Search bar + new-conversation button
        Rectangle {
            id: searchHeader
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: searchRow.implicitHeight + AppTheme.spacing8 * 2

            // The header wraps to two rows below a derived threshold rather
            // than overflowing: the search card has a 120px floor and the
            // actions are fixed, and layouts do not clip, so a narrow column
            // pushed the actions (including Classic's only Discover entry) off
            // the panel. The actions keep their right edge in both
            // arrangements. The threshold uses the card's floor and the
            // actions' implicit width, neither of which depends on this row's
            // width.
            readonly property real headerOneRowFloor:
                AppTheme.spacing12 * 2 + AppTheme.spacing8
                + searchCard.Layout.minimumWidth + headerActions.implicitWidth
            readonly property bool headerStacked: width < headerOneRowFloor

            GridLayout {
                id: searchRow
                anchors {
                    left: parent.left; right: parent.right
                    verticalCenter: parent.verticalCenter
                    leftMargin: AppTheme.spacing12
                    rightMargin: AppTheme.spacing12
                }
                columns: searchHeader.headerStacked ? 1 : 2
                columnSpacing: AppTheme.spacing8
                rowSpacing: AppTheme.spacing8

                // The search card: surface fill, 1px border, rounded, with a
                // borderless field inside. Focus promotes the border to the
                // focus ring.
                Rectangle {
                    id: searchCard
                    objectName: "roomSearchCard"
                    Layout.fillWidth: true
                    // A floor: a RowLayout would shrink a fillWidth item to
                    // nothing, and the contents would spill over the button
                    // beside it. 120 leaves ~82px of field with the keycap
                    // hidden, enough for the "Search" placeholder.
                    Layout.minimumWidth: 120
                    implicitHeight: 34
                    radius: AppTheme.radiusMd
                    color: AppTheme.surface
                    border.width: roomSearch.activeFocus ? 2 : 1
                    border.color: roomSearch.activeFocus ? AppTheme.focusRing
                                  : searchCardHover.hovered ? AppTheme.borderStrong
                                  : AppTheme.border
                    HoverHandler { id: searchCardHover }
                    // The whole pill focuses the field.
                    TapHandler {
                        onTapped: roomSearch.forceActiveFocus()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing8 + 2
                        anchors.rightMargin: AppTheme.spacing6
                        spacing: AppTheme.spacing6

                        // Leading search glyph.
                        Icon {
                            name: "search"
                            size: 16
                            color: AppTheme.textMuted
                        }
                        TextField {
                            id: roomSearch
                            Layout.fillWidth: true
                            // Fill the card's height so the hit area matches
                            // the pill.
                            Layout.fillHeight: true
                            placeholderText: qsTr("Search")
                            Accessible.name: qsTr("Search rooms")
                            onTextChanged: app.roomList.searchQuery = text
                            font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                            color: AppTheme.textPrimary
                            placeholderTextColor: AppTheme.textMuted
                            selectionColor: AppTheme.accentSoft
                            selectedTextColor: AppTheme.textPrimary
                            verticalAlignment: TextInput.AlignVCenter
                            padding: 0
                            leftPadding: 0
                            rightPadding: 0
                            // The card is the visual container.
                            background: null
                        }
                        // Quick-switcher keycap hint, not translated. storm:
                        // false, since the room list keeps the user's theme.
                        MenuKeycap {
                            keys: "Ctrl+K"
                            storm: false
                            // A hint, first to go when narrow. Tested on the
                            // card's width, which does not depend on this
                            // item's visibility; the field's own width would
                            // oscillate.
                            visible: !roomSearch.activeFocus
                                     && searchCard.width >= 220
                        }
                    }
                }

                // The actions as one item, so the header can move them to a
                // second row. Its implicitWidth is what headerOneRowFloor
                // measures.
                RowLayout {
                    id: headerActions
                    spacing: AppTheme.spacing8
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

                    // Start a DM or create a room (the controller reports
                    // unsupported backends).
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
                    // The Activity Center: everything addressed to you, across
                    // rooms.
                    IconButton {
                        id: activityBtn
                        objectName: "activityCenterButton"
                        visible: app.loggedIn && !!app.activity
                        implicitWidth: 30; implicitHeight: 30
                        radius: AppTheme.radiusMd
                        iconName: "notifications"
                        iconSize: 18
                        active: !!app.activity && app.activity.unseenCount > 0
                        Accessible.name: (!!app.activity && app.activity.unseenCount > 0)
                                         ? qsTr("Activity, %n unseen", "", app.activity.unseenCount)
                                         : qsTr("Activity")
                        ToolTip.text: qsTr("Activity")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: activityPanel.openPanel()
                        Rectangle {
                            objectName: "activityCenterBadge"
                            visible: !!app.activity && app.activity.unseenCount > 0
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.topMargin: -3
                            anchors.rightMargin: -5
                            height: 14
                            width: Math.max(14, activityBadgeLabel.implicitWidth + 6)
                            radius: 7
                            color: AppTheme.bolt
                            Label {
                                id: activityBadgeLabel
                                anchors.centerIn: parent
                                text: !!app.activity
                                      ? (app.activity.unseenCount > 99
                                         ? "99+" : String(app.activity.unseenCount))
                                      : ""
                                color: AppTheme.stormDeep
                                font.pixelSize: 9
                                font.weight: AppTheme.weightBold
                            }
                        }
                    }
                    // Discover / Join: browse the public directory or join by
                    // address or link.
                    IconButton {
                        id: discoverBtn
                        objectName: "discoverJoinButton"
                        visible: app.loggedIn && app.discovery.supported
                        implicitWidth: 30; implicitHeight: 30
                        radius: AppTheme.radiusMd
                        iconName: "explore"
                        iconSize: 18
                        Accessible.name: qsTr("Discover rooms")
                        ToolTip.text: qsTr("Discover rooms")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: discoverJoinDialog.openDialog()
                    }
                }
            }
        }

        // Filter chips. The model owns filtering (RoomListModel::filterMode);
        // the chips reflect and write the persisted per-account preference,
        // which the model follows through the Binding below.
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: filterChips.implicitHeight + AppTheme.spacing6 * 2
            // storm: false, like MenuKeycap here.
            SegmentedControl {
                id: filterChips
                objectName: "roomFilterChips"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: AppTheme.spacing12
                anchors.rightMargin: AppTheme.spacing12
                anchors.verticalCenter: parent.verticalCenter
                dense: true
                // Compacts to fit a resizable column and translated labels.
                fitWidth: true
                // Channels drops People and Rooms: the rail's Home and Direct
                // Messages tabs are that split. The stored preference is not
                // rewritten, so switching back to Classic restores it; it is
                // mapped on the way into this row and the model.
                readonly property bool channelsLayout:
                    app.settings && app.settings.roomNavigationLayout === 1
                // A Space view gets People back: its People group (DMs with the
                // Space's members) is gated on this filter. Rooms stays out,
                // since a Space view already shows its rooms.
                readonly property bool spaceView:
                    app.spaceChannels
                    && app.spaceChannels.viewKind === "space"
                // Home has its own Direct Messages group, so People means that
                // group.
                readonly property bool homeView:
                    app.spaceChannels
                    && app.spaceChannels.viewKind === "home"
                // The mapping, in one place, read by both the chips and the
                // model Binding.
                readonly property int channelsFilterMode: {
                    const stored = app.settings.roomFilterMode
                    if (stored === 3)
                        return 3
                    // People passes through only where the chip is offered (a
                    // Space view and Home).
                    if (stored === 1 && (filterChips.spaceView || filterChips.homeView))
                        return 1
                    return 0
                }
                model: channelsLayout
                       ? ((spaceView || homeView)
                          ? [ { label: qsTr("All"), value: 0 },
                              { label: qsTr("People"), value: 1 },
                              { label: qsTr("Unreads"), value: 3 } ]
                          : [ { label: qsTr("All"), value: 0 },
                              { label: qsTr("Unreads"), value: 3 } ])
                       : [ { label: qsTr("All"), value: 0 },
                           { label: qsTr("People"), value: 1 },
                           { label: qsTr("Rooms"), value: 2 },
                           { label: qsTr("Unreads"), value: 3 } ]
                // Reads the setting it writes, not the model: one direction,
                // chips -> setting -> model.
                current: channelsLayout
                         ? filterChips.channelsFilterMode
                         : app.settings.roomFilterMode
                onActivated: (value) => {
                    app.settings.roomFilterMode = value
                }
            }
        }
        Binding {
            target: app.roomList
            property: "filterMode"
            value: app.settings.roomFilterMode
        }
        // The same chrome drives both layouts. In Channels only All and Unreads
        // exist, so a stored People/Rooms reads as All.
        Binding {
            target: app.spaceChannels
            property: "filterMode"
            value: filterChips.channelsFilterMode
        }
        Binding {
            target: app.spaceChannels
            property: "searchQuery"
            value: roomSearch.text
        }
        Binding {
            target: app.spaceChannels
            property: "messageSearchSupported"
            value: app.loggedIn && app.messageSearch.supported
        }
        // The rail's selection, verbatim, chooses the view (Space, Direct
        // Messages or Home). "@people" is a real selection that is not a Space;
        // the model does the classifying.
        Binding {
            target: app.spaceChannels
            property: "scopeSpaceId"
            value: app.spaces ? app.spaces.activeSpaceId : ""
        }

        NewConversationDialog {
            id: newConversationDialog
            parent: Overlay.overlay
        }

        ActivityCenterPanel { id: activityPanel }
        DiscoverJoinDialog {
            id: discoverJoinDialog
            parent: Overlay.overlay
        }

        // RoomDelegate is signal-only, so the clipboard write and the leave
        // confirmation/error surfaces are one shared instance here.
        TextEdit {
            id: roomLinkClipboard
            visible: false
            width: 0
            height: 0
        }

        // Leave confirmation; Cancel is the default. Same shape as
        // RoomInfoPanel's.
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
                    // Untrusted text: never markup.
                    textFormat: Text.PlainText
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

        // A leave from the list menu has its own pending/error state
        // (RoomInfoController tracks it separately); report failure here.
        Dialog {
            id: leaveRoomFailedDialog
            objectName: "leaveRoomFailedDialog"
            parent: Overlay.overlay
            anchors.centerIn: parent
            width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))
            modal: true
            // Same chrome as leaveRoomConfirm.
            standardButtons: Dialog.NoButton
            closePolicy: Popup.CloseOnEscape
            property string roomLabel: ""
            property string messageText: ""
            title: qsTr("Couldn't leave \"%1\"").arg(leaveRoomFailedDialog.roomLabel)

            background: Rectangle {
                color: AppTheme.surface
                border.color: AppTheme.border
                radius: AppTheme.radiusLg
            }

            contentItem: ColumnLayout {
                spacing: AppTheme.spacing12
                Label {
                    Layout.fillWidth: true
                    text: leaveRoomFailedDialog.messageText
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    color: AppTheme.textPrimary
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    AppButton {
                        text: qsTr("Close")
                        focus: true
                        onClicked: leaveRoomFailedDialog.close()
                    }
                }
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

        // A hairline between the controls and the list, so the chips do not
        // read as list rows. borderStrong: it divides two regions of one pane.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorderStrong
        }

        // List body: the chosen navigation layout. The header, search, dialogs
        // and footer are shared by both layouts. A Loader, so the unchosen
        // layout instantiates nothing. The empty-state label lives in this
        // wrapper, not inside the ListView, whose contentItem collapses to 0
        // when empty.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // The chosen layout; Channels applies everywhere, not only inside a
            // Space.
            readonly property bool channelsChosen:
                app.settings && app.settings.roomNavigationLayout === 1
            readonly property bool channelsUsable: channelsChosen

            Loader {
                anchors.fill: parent
                active: !parent.channelsUsable
                visible: active
                sourceComponent: RoomListClassicPresenter {
                    currentRoomId: app.currentRoomId
                    onRoomActivated: (roomId) => app.openRoom(roomId)
                    onCreateRequested: newConversationDialog.openDialog()
                    onDiscoverRequested: discoverJoinDialog.openDialog()
                    onClearSearchRequested: roomSearch.clear()
                    onRoomLinkCopyRequested: (roomId) => {
                        var row = app.roomList.findRoom(roomId)
                        var link = app.roomList.roomPermalink(
                            roomId, (row && row.canonicalAlias) || "")
                        if (link.length > 0) {
                            roomLinkClipboard.text = link
                            roomLinkClipboard.selectAll()
                            roomLinkClipboard.copy()
                            roomLinkClipboard.text = ""
                        }
                    }
                    onLeaveRoomRequested: (roomId, roomName) =>
                        leaveRoomConfirm.openFor(roomId, roomName)
                }
            }

            Loader {
                anchors.fill: parent
                active: parent.channelsUsable
                visible: active
                sourceComponent: RoomChannelsPresenter {
                    currentRoomId: app.currentRoomId
                    onRoomActivated: (roomId) => app.openRoom(roomId)
                    onLobbyActivated: app.openLobby()
                    onMessageSearchRequested: root.messageSearchRequested()
                    // Reuse the host's own dialogs, so there is one create and
                    // one discover path.
                    onCreateRoomRequested: newConversationDialog.openDialog("room")
                    onCreateChatRequested: newConversationDialog.openDialog("dm")
                    onJoinAddressRequested: discoverJoinDialog.openDialog("address")
                    onExploreSpacesRequested: discoverJoinDialog.openDialog("browse")
                    // The same clipboard proxy and leave confirmation as the
                    // Classic list.
                    onRoomLinkCopyRequested: (roomId) => {
                        var row = app.roomList.findRoom(roomId)
                        var link = app.roomList.roomPermalink(
                            roomId, (row && row.canonicalAlias) || "")
                        if (link.length > 0) {
                            roomLinkClipboard.text = link
                            roomLinkClipboard.selectAll()
                            roomLinkClipboard.copy()
                            roomLinkClipboard.text = ""
                        }
                    }
                    onLeaveRoomRequested: (roomId, roomName) =>
                        leaveRoomConfirm.openFor(roomId, roomName)
                }
            }
        }

        // Voice Connected footer while a call is live; zero height otherwise.
        VoiceConnectedBar {
            objectName: "roomsPanelVoiceBar"
            Layout.fillWidth: true
            Layout.margins: visible ? AppTheme.spacing8 : 0
            onReturnToCallRequested: {
                if (app.groupCall.roomId.length > 0)
                    // openRoom(), not a currentRoomId write: the write only
                    // selects, and openRoom() is the only caller of
                    // openRoomTimeline(). Its alreadyOpen guard would then skip
                    // the real open.
                    app.openRoom(app.groupCall.roomId)
            }
        }
    }
}
