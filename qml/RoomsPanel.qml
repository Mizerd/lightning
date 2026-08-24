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

    // v0.7.x: open the Discover / Join dialog ("browse" | "address").
    function openDiscover(startMode) {
        discoverJoinDialog.openDialog(startMode)
    }

    // A Matrix room link from a message: resolve it in the Address tab; a
    // link to an already-joined room auto-opens (and jumps when it carries
    // an event id).
    function openDiscoverForLink(link) {
        discoverJoinDialog.openForLink(link)
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

        // ── Workspace header: brand mark + workspace name ────────────────
        // 2026-08-21: pinned to 60px and given the trailing hairline every
        // other column already had. The four column headers used to be 46 /
        // 60 / 58 / 54 px tall with a rule under only three of them, so a
        // wide window drew three horizontal lines at three heights and left
        // the fourth column open — the single clearest "assembled from
        // parts" tell in the shell. 60 is the timeline header's height, the
        // one this column sits next to.
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: Math.max(
                60, headerRow.implicitHeight + AppTheme.spacing12 * 2)

            // v0.6.5 (C5): the wordmark reads "Lightning ⚡" — the bolt trails
            // the workspace name on a shared baseline, one glyph, no banner
            // tile. workspaceLabel is intentionally NOT Layout.fillWidth: a
            // fillWidth label pins a trailing sibling to the row's far edge,
            // which reads as a right-aligned button rather than a wordmark
            // suffix. headerRow is anchored left-only (no `right`): anchoring
            // both edges forces the RowLayout wider than its content, and
            // with neither child set to fillWidth, QtQuickLayouts still
            // distributes that surplus between them — which is exactly what
            // pinned the bolt to the column's far-right edge instead of
            // hugging the label. Left-anchoring only makes the RowLayout take
            // its own implicit width (label + spacing + bolt, nothing more),
            // so there is no surplus to distribute regardless of layout
            // policy. Layout.maximumWidth on the label still accounts for
            // the right margin (root.width - spacing12*2 - spacing - bolt
            // width) so a long Space name elides instead of overflowing the
            // column.
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
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightBold
                    elide: Label.ElideRight

                    // When a real Space is selected the workspace title is a
                    // second route to its Space Home overview (the rail's
                    // double-click being the other).
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

                // The Lightning bolt mark — the app brand above all chats,
                // echoing the trust surface's bolt and the application icon.
                // A bare trailing glyph (no tile background), optically
                // sized close to the label's cap-height and vertically
                // centered on the same row; never focusable, never resized
                // by hover/focus (header height is driven by headerRow's
                // implicit height alone, and neither child here reacts to
                // hover/focus at all).
                Icon {
                    id: wordmarkBolt
                    objectName: "workspaceBoltMark"
                    Layout.alignment: Qt.AlignVCenter
                    name: "bolt"
                    size: 15
                    color: AppTheme.accent
                    // Decorative: the region should announce the workspace
                    // name text alone, not an unnamed glyph after it.
                    Accessible.ignored: true
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.border
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
                    leftMargin: AppTheme.spacing12
                    rightMargin: AppTheme.spacing12
                }
                spacing: AppTheme.spacing8

                // The search card — same component family as the composer
                // card: surface fill, 1px border, rounded, with the field
                // itself borderless and transparent inside. Focus promotes
                // the card border to the shared focus ring.
                Rectangle {
                    id: searchCard
                    objectName: "roomSearchCard"
                    Layout.fillWidth: true
                    implicitHeight: 34
                    radius: AppTheme.radiusMd
                    color: AppTheme.surface
                    border.width: roomSearch.activeFocus ? 2 : 1
                    border.color: roomSearch.activeFocus ? AppTheme.focusRing
                                  : searchCardHover.hovered ? AppTheme.borderStrong
                                  : AppTheme.border
                    HoverHandler { id: searchCardHover }
                    // The whole pill is the input: a press on the glyph,
                    // the keycap or the padding must focus the field, not
                    // dead-drop. (The field's own presses grab before this
                    // handler; forceActiveFocus is idempotent either way.)
                    TapHandler {
                        onTapped: roomSearch.forceActiveFocus()
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing8 + 2
                        anchors.rightMargin: AppTheme.spacing6
                        spacing: AppTheme.spacing6

                        // Leading search glyph (handoff §2 search field).
                        Icon {
                            name: "search"
                            size: 16
                            color: AppTheme.textMuted
                        }
                        TextField {
                            id: roomSearch
                            Layout.fillWidth: true
                            // Fill the card's height so the field's own hit
                            // area matches the painted pill.
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
                            // The card is the visual container: the field
                            // itself draws no chrome of its own.
                            background: null
                        }
                        // Quick-switcher keycap hint. Not translated —
                        // shortcut chips render literally (the Settings
                        // "Ctrl+," convention). storm: false — the room
                        // list keeps the user's theme (SPEC-storm §5), so
                        // this one keycap renders the themed variant.
                        MenuKeycap {
                            keys: "Ctrl+K"
                            storm: false
                            visible: !roomSearch.activeFocus
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
                // v0.7.x Discover / Join: browse the public directory or
                // join by address/link.
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

        // Element-style list filter chips. The MODEL owns the filtering
        // (RoomListModel::filterMode); the chip row just reflects and
        // writes the persisted per-account preference, which the model
        // follows through the Binding below — so an account switch or a
        // restart restores the chosen view.
        Rectangle {
            Layout.fillWidth: true
            color: AppTheme.sidebar
            implicitHeight: filterChips.implicitHeight + AppTheme.spacing6 * 2
            // storm: deliberately left false — the room-list family keeps
            // the default themed treatment (same rule as MenuKeycap here).
            SegmentedControl {
                id: filterChips
                objectName: "roomFilterChips"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: AppTheme.spacing12
                anchors.rightMargin: AppTheme.spacing12
                anchors.verticalCenter: parent.verticalCenter
                dense: true
                // The column is 300px by default and user-resizable, and the
                // four labels translate to whatever they translate to — so
                // the row compacts instead of running off the pane edge
                // ("Unreads" was clipped by the column boundary).
                fitWidth: true
                model: [
                    { label: qsTr("All"), value: 0 },
                    { label: qsTr("People"), value: 1 },
                    { label: qsTr("Rooms"), value: 2 },
                    { label: qsTr("Unreads"), value: 3 }
                ]
                // Reads the SETTING it writes, not the model it drives. With
                // `current` bound to app.roomList.filterMode the chips
                // reported the model while every click wrote the setting, so
                // any moment the two disagreed — an account switch being the
                // reliable one — left the chips showing a filter the user had
                // not chosen and made clicking the stored value a no-op.
                // One direction now: chips -> setting -> model (Binding).
                current: app.settings.roomFilterMode
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

        NewConversationDialog {
            id: newConversationDialog
            parent: Overlay.overlay
        }

        DiscoverJoinDialog {
            id: discoverJoinDialog
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
            // Was `standardButtons: Dialog.Ok` with no background override,
            // i.e. Basic's square canvas-coloured panel and a 100x40 stock
            // grey button — under Storm the panel was the same colour as the
            // screen behind it. Same chrome as leaveRoomConfirm above now, so
            // the failure of an action looks like the action that failed.
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

        // A hairline between the CONTROLS above (filter chips, search) and
        // the LIST below. Without it the chips read as the first rows of the
        // list rather than as chrome acting on it — reported 2026-08-21:
        // "a line to seperate the controls like all people rooms and search
        // from the acctual list". borderStrong rather than border: this
        // divides two regions of one pane, where the pane's own edges use
        // the quieter tone.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: AppTheme.stormBorderStrong
        }

        // ── Room list with DM / ROOMS section headers ─────────────────────
        // The empty-state label lives in this wrapper Item, NOT inside the
        // ListView: children declared inside a view are reparented into its
        // contentItem, whose height collapses to 0 with an empty model —
        // centring there put the label half above the clipped viewport.
        // ── List body: the chosen navigation layout ──────────────────────
        //
        // HOST / PRESENTER split. The header, search field, dialogs and
        // footer above and below this slot are shared by BOTH layouts, so
        // switching layout changes how conversations are organised and
        // nothing else — no second header to keep in sync, no second create
        // path.
        //
        // Loader rather than two visibility-gated children: the layout that
        // is not chosen must not instantiate its ListView, its delegates, or
        // its empty state at all. Two live room lists is two sets of avatar
        // fetches for one visible column.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Channels needs a Space to show. At Home there is no hierarchy
            // at all, so the host falls back to Classic rather than
            // rendering an empty column: "this space has no channels" and
            // "you are not in a space" are different facts, and only the
            // first one is this layout's to state.
            readonly property bool channelsChosen:
                app.settings && app.settings.roomNavigationLayout === 1
            readonly property bool channelsUsable:
                channelsChosen && app.spaceChannels
                && app.spaceChannels.spaceId.length > 0

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
                    onSpaceActivated: (spaceId) => app.openSpaceHome(spaceId)
                    // The SAME clipboard proxy and leave-confirm dialog the
                    // Classic list uses, so a room left from either layout
                    // gets the same confirmation and the same honest failure.
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

        // Voice Connected: a persistent footer while a call is live, so the
        // user can browse other rooms without leaving the call and can get
        // back to it in one click. Collapses to zero height otherwise, so
        // the column is unchanged when there is no call.
        VoiceConnectedBar {
            objectName: "roomsPanelVoiceBar"
            Layout.fillWidth: true
            Layout.margins: visible ? AppTheme.spacing8 : 0
            onReturnToCallRequested: {
                if (app.groupCall.roomId.length > 0)
                    app.currentRoomId = app.groupCall.roomId
            }
        }

        // The account entry point lives on the SpacesRail (design shell);
        // this column intentionally has no footer.
    }
}
