import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    color: AppTheme.background

    // v0.7.1: the Home surface routes create actions to the room list's
    // shared new-conversation dialog (MainScreen wires this to RoomsPanel).
    // options (optional): {addToSpace: bool} — Space Home's "Create room
    // here" preselects placement into the active Space.
    signal newConversationRequested(string mode, var options)

    property var currentRoom: ({})
    // v0.5.9: Room Information side panel (Phases 6/10 surface).
    property bool infoOpen: false
    property bool searchOpen: false
    // v0.6.0: the thread side surface is open when a thread panel OR the
    // room's Threads list view is showing.
    readonly property bool threadSurfaceOpen: app.thread.active
                                              || app.thread.listOpen
    // The authoritative right-panel state. Exactly one surface is open:
    // the thread surface (controller-owned state) wins, then the info/member
    // panel, else none. All open/close paths flow through the two underlying
    // states and their exclusivity handlers below.
    readonly property string rightPanelState:
        threadSurfaceOpen ? "thread"
        : (searchOpen ? "search" : (infoOpen ? "info" : "none"))

    function refreshCurrentRoom() {
        currentRoom = app.currentRoomId === ""
                      ? ({})
                      : app.roomList.findRoom(app.currentRoomId)
    }

    // The right-side region is mutually exclusive: member/info panel OR
    // thread panel, never both layered. Exclusion lives on the two state
    // properties themselves so every open path — buttons, chips,
    // notifications, tests — flows through the same mechanism.
    onThreadSurfaceOpenChanged: {
        if (threadSurfaceOpen) {
            infoOpen = false
            searchOpen = false
        }
    }
    onInfoOpenChanged: {
        if (infoOpen) {
            closeThreadSurface()
            searchOpen = false
        }
    }
    onSearchOpenChanged: {
        if (searchOpen) {
            closeThreadSurface()
            infoOpen = false
        }
    }
    function closeThreadSurface() {
        if (app.thread.active)
            app.thread.close()
        app.thread.closeList()
    }

    function toggleRoomInfo() {
        if (app.currentRoomId === "" || !app.roomInfo.supported)
            return
        if (infoOpen) {
            infoOpen = false
            return
        }
        infoPanel.openForRoom(app.currentRoomId)
        infoOpen = true
    }

    function toggleSearchPanel() {
        if (app.currentRoomId === "") return
        if (searchOpen) {
            searchOpen = false
            return
        }
        if (root.findOpen)
            root.closeFind()
        if (app.messageSearch.roomId !== app.currentRoomId) {
            app.messageSearch.roomId = app.currentRoomId
            app.messageSearch.filters = ({})
        }
        app.roomInfo.roomId = app.currentRoomId
        searchOpen = true
    }

    // Header forum toggle: opens the room's thread surface (list, or the
    // open thread), closes it when it is already showing.
    function toggleThreadSurface() {
        if (threadSurfaceOpen) {
            if (app.thread.active)
                app.thread.close()
            app.thread.closeList()
        } else {
            app.thread.openList(app.currentRoomId)
        }
    }

    // Header pin shortcut: opens the same side panel directly on the
    // Pinned section (mirrors toggleMemberPanel).
    function togglePinnedPanel() {
        if (app.currentRoomId === "" || !app.roomInfo.supported)
            return
        if (infoOpen && infoPanel.section === "pinned") {
            infoOpen = false
            return
        }
        infoPanel.openForRoom(app.currentRoomId)
        infoPanel.section = "pinned"
        infoOpen = true
    }

    // v0.7 design shell: the header's members button opens the same side
    // panel directly on the People section.
    function toggleMemberPanel() {
        if (app.currentRoomId === "" || !app.roomInfo.supported)
            return
        if (infoOpen && infoPanel.section === "people") {
            infoOpen = false
            return
        }
        infoPanel.openForRoom(app.currentRoomId)
        infoPanel.section = "people"
        infoOpen = true
    }

    Connections {
        target: app
        // Full-view Settings clears every right-side surface; exiting
        // Settings must not restore any of them.
        function onCurrentScreenChanged() {
            if (app.currentScreen === 2) {
                root.infoOpen = false
                root.searchOpen = false
            }
        }
        function onCurrentRoomIdChanged() {
            refreshCurrentRoom()
            // A find session belongs to the room it was opened in — the
            // history-mode results included (roomId-scoped server search).
            if (root.findOpen) {
                root.findOpen = false
                root.findHistoryMode = false
                app.messageSearch.query = ""
                findField.text = ""
            }
            // Old-room wheel motion must not continue into the new room.
            timeline.cancelWheelMotion()
            // A pinned message-action toolbar belongs to the room it was
            // pinned in; drop it when the room changes.
            timeline.pinnedActionsKey = ""
            timeline.viewAnchorId = ""
            timeline.viewAnchorOffset = 0
            timeline.viewAnchorLastY = 0
            // A room switch collapses the right side: no panel from the
            // previous room may remain (reopen it deliberately in the new
            // room if wanted).
            root.infoOpen = false
            root.searchOpen = false
        }
    }

    // v0.6.1: find in loaded messages (this room's currently loaded
    // timeline; never a persistent index). v0.7.x adds an explicit History
    // segment — an honest server /search of this room — in unencrypted
    // rooms only; the two modes never mix results.
    property bool findOpen: false
    // v0.7.x: the find bar's second mode — server-side history search of
    // THIS room. Unavailable in encrypted rooms (the server cannot search
    // ciphertext; the loaded-messages find remains the only search there).
    property bool findHistoryMode: false
    // Review H1: the offer needs an AFFIRMATIVE unencrypted state — a room
    // whose encryption state has not synced yet gets no History segment.
    readonly property bool findHistoryAvailable:
        app.messageSearch.supported
        && root.currentRoom.encryptionKnown === true
        && root.currentRoom.encrypted !== true

    function openFind() {
        if (app.currentRoomId === "") return
        root.searchOpen = false
        root.findOpen = true
        app.timeline.beginSearch(findField.text)
        findField.forceActiveFocus()
        findField.selectAll()
    }
    function closeFind() {
        root.findOpen = false
        root.findHistoryMode = false
        app.messageSearch.query = ""
        app.timeline.endSearch()
        // v0.6.5 (C7): the field is about to become invisible/unfocusable
        // (findBar's visible binding follows findOpen) — hand focus back to
        // the timeline explicitly rather than leaving the focus scope with
        // no active item, mirroring the same reclaim call the timeline
        // tap handler already uses elsewhere in this file.
        timeline.forceActiveFocus()
    }
    function scrollToSearchMatch() {
        var eventId = app.timeline.searchCurrentEventId
        if (eventId === "") return
        // The match may be an older row the proxy is still pacing out. Take
        // the whole backlog before resolving it, or the jump resolves to
        // "no such row" and the search silently does nothing.
        timeline.releasePendingRows()
        var row = timeline.viewRowForStableId(eventId)
        if (row < 0) return
        timeline.cancelWheelMotion()
        timeline.stickToBottom = false
        timeline.positionViewAtViewRow(row, true)
    }
    Shortcut {
        sequences: [StandardKey.Find]
        enabled: app.currentRoomId !== ""
        onActivated: root.openFind()
    }
    Connections {
        target: app.timeline
        function onSearchChanged() {
            if (root.findOpen) root.scrollToSearchMatch()
        }
    }

    // Escape closes the room info panel first, then any pinned toolbar.
    Shortcut {
        sequence: "Escape"
        enabled: !timeline.emojiPickerOpen
                 && (root.findOpen || root.infoOpen || root.searchOpen
                     || root.threadSurfaceOpen
                     || timeline.pinnedActionsKey !== "")
        onActivated: {
            if (root.findOpen)
                root.closeFind()
            else if (root.infoOpen)
                root.infoOpen = false
            else if (root.searchOpen)
                root.searchOpen = false
            else if (app.thread.active)
                app.thread.close()
            else if (app.thread.listOpen)
                app.thread.closeList()
            else
                timeline.pinnedActionsKey = ""
        }
    }
    Connections {
        target: app.roomList
        function onDataChanged() { refreshCurrentRoom() }
        function onModelReset() { refreshCurrentRoom() }
    }
    // Read-state on focus change is handled by the ListView's own
    // maybeMarkRead() gate (see the timeline below).
    Component.onCompleted: refreshCurrentRoom()

    // v0.5.9: in-app image viewer + explicit Save As for file attachments.
    ImageViewerOverlay { id: imageViewer }

    // v0.7: ONE reaction picker and ONE sender-profile popover for the whole
    // timeline (previously every message row eagerly built its own picker
    // popup — dozens of live instances per screen). The target event id is
    // snapshotted at open; a room or account switch closes both.
    EmojiPicker {
        id: sharedReactionPicker
        mode: "reaction"
        property string targetEventId: ""
        onOpened: timeline.emojiPickerOpen = true
        onClosed: {
            timeline.emojiPickerOpen = false
            targetEventId = ""
        }
        onEmojiChosen: (emoji) => {
            if (targetEventId !== "")
                app.composer.reactTo(targetEventId, emoji)
        }
    }
    MemberProfilePopover {
        id: senderProfilePopover
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            sharedReactionPicker.close()
            senderProfilePopover.close()
            // The viewer holds decoded pixels and a stale entries snapshot;
            // it must never survive into another room or account (account
            // switches also change the current room).
            imageViewer.close()
        }
        function onAccountSwitchingChanged() {
            if (app.accountSwitching)
                imageViewer.close()
        }
    }
    // Development-only: screenshot-demo popup hooks (see
    // ScreenshotDemoController and SpacesRail.qml:accountSwitcherRequested
    // for the pattern this mirrors). Null target / disabled in a non-demo
    // build makes this an inert no-op.
    Connections {
        target: app.demo
        enabled: app.screenshotDemoActive
        function onDemoOpenMessageContextMenu() {
            var tries = Math.min(timeline.count, 40)
            // Prefer a plain-text row sent by the demo account itself —
            // canEditEvent() is the SAME gate the real "Edit" menu item
            // uses (own + TextMessage + Sent), so a hit here is guaranteed
            // to render Edit (E keycap) and Delete (danger), not just
            // whatever happens to be the newest row.
            for (var i = 0; i < tries; ++i) {
                var eventId = timeline.eventIdAtViewRow(i)
                if (eventId !== "" && app.timeline.canEditEvent(eventId)) {
                    var ownItem = timeline.itemAtViewRow(i)
                    if (ownItem && ownItem.openContextMenu) {
                        ownItem.openContextMenu(ownItem.width / 2, ownItem.height / 2)
                        return
                    }
                }
            }
            // Fall back to any real, currently-instantiated row — walk back
            // from the newest loaded row. openContextMenu() itself no-ops
            // for a virtual/state-activity row or one with no real event id
            // (it never sets menuEventId), so this backward scan skips
            // those without any special-casing here.
            for (var j = 0; j < tries; ++j) {
                var item = timeline.itemAtViewRow(j)
                if (item && item.openContextMenu) {
                    item.openContextMenu(item.width / 2, item.height / 2)
                    if (item.menuEventId !== undefined && item.menuEventId !== "")
                        return
                }
            }
        }
        function onDemoOpenMemberProfile() {
            // A real Design Lounge fictional member (docs/screenshot-demo.md)
            // — MemberProfilePopover only ever renders caller-supplied
            // fields, so no room-membership lookup is needed here.
            timeline.openSenderProfile({
                userId: "@maya:lightning.example",
                displayName: "Maya Chen",
                membership: "joined",
                role: "",
                avatarUrl: "mxc://lightning.example/avatar-maya",
                isOwn: false
            })
        }
        // v0.6.5 (C7): drives the find-in-room card into the exact state a
        // capture needs — open, with a live match counter — through the
        // real openFind() path rather than a demo-only shortcut. Setting
        // findField.text AFTER openFind() (which starts the search session
        // with whatever the field already held) fires the field's own
        // onTextChanged -> app.timeline.updateSearch(text), the same signal
        // path a typing user drives.
        function onDemoOpenFindBar(query) {
            root.openFind()
            findField.text = query
        }
    }
    FileDialog {
        id: saveMediaDialog
        property string pendingMediaKey: ""
        title: qsTr("Save file as…")
        fileMode: FileDialog.SaveFile
        onAccepted: {
            if (pendingMediaKey.length > 0) {
                timeline.noteSaveStarted(pendingMediaKey)
                app.mediaBridge.saveAs(pendingMediaKey, selectedFile)
            }
            pendingMediaKey = ""
        }
        onRejected: pendingMediaKey = ""
    }
    Connections {
        target: app.mediaBridge
        function onSaveFinished(ok, message, mediaKey) {
            saveResult.ok = ok
            saveResult.text = message
            saveResultTimer.restart()
            timeline.noteSaveFinished(ok, mediaKey)
        }
    }
    // v0.6.6: save/unsave GIF feedback — the same auto-clearing
    // banner Save As already uses (an explicit-export action of the same
    // class; see GifStarredStore's header). Honest failures only: no silent
    // drop. `message` is ALREADY a translated, ready-to-display sentence
    // (see GifStarredStore::categoryMessage) — never a raw category token.
    Connections {
        target: app.gif.starredStore
        // v0.6.7: FAILURES ONLY. A successful save or unsave says nothing —
        // the star itself fills or empties, which is feedback in the place the
        // user is already looking, and the banner is one more thing appearing
        // and disappearing at the bottom of the timeline. Honest failures are
        // unaffected: `message` is already a translated, ready-to-display
        // sentence (GifStarredStore::categoryMessage), never a raw category
        // token, and nothing is silently dropped.
        //
        // "already_starred" is reported as ok, so it is silent too. The star
        // was showing UNfilled when the user pressed (that is why the durable
        // check took the star branch at all), but MessageDelegate.qml's own
        // onStarFinished calls refreshStarredState() regardless of `ok`, so
        // the star fills on completion either way — that fill is the feedback,
        // and it is more accurate than a banner would be.
        function onStarFinished(mediaKey, ok, category, message) {
            if (ok)
                return
            saveResult.ok = false
            saveResult.text = message
            saveResultTimer.restart()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

    ColumnLayout {
        objectName: "roomColumn"
        // Deliberate narrow fallback: below 660px pane width the 340px
        // panel plus the 320px timeline minimum cannot coexist, so the open
        // thread surface takes the pane (the room and its state stay alive
        // underneath and return when the panel closes or the window
        // widens). From 660px up, threads are ALWAYS a side panel.
        visible: !(root.threadSurfaceOpen && root.width < 660)
                 && !(root.searchOpen && root.width < 700)
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 320
        spacing: 0

        // Room header — design: 60px, 0 20px padding, 34px room avatar,
        // bare 34×34/radius-8 icon buttons; the open right panel's toggle
        // shows the accent-chip state.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 60
            color: AppTheme.surface
            RowLayout {
                id: header
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing20
                anchors.rightMargin: AppTheme.spacing20
                spacing: AppTheme.spacing12
                Avatar {
                    visible: app.currentRoomId !== ""
                    size: 34
                    squareRadius: 9
                    name: root.currentRoom.name || app.currentRoomId
                    mxc: root.currentRoom.avatarUrl || ""
                    // One fallback-colour policy (was keyed by display
                    // NAME here — a third colour for the same DM partner,
                    // changing on rename).
                    colorKey: root.currentRoom.identityColorKey
                              || app.currentRoomId
                    // Shape rule: people/DMs are circles; rooms and Spaces
                    // are rounded squares.
                    circle: root.currentRoom.isDirect === true
                }
                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Label {
                            text: {
                                if (root.currentRoom.name)
                                    return root.currentRoom.name
                                if (app.currentRoomId !== "")
                                    return app.currentRoomId
                                // No room open: Space Home shows the Space's
                                // own name, not the literal "Home".
                                if (app.spaces
                                        && app.spaces.activeSpaceId.length > 0
                                        && app.spaces.activeSpaceId.charAt(0) === "!")
                                    return app.spaces.spaceName(
                                        app.spaces.activeSpaceId) || qsTr("Space")
                                return qsTr("Home")
                            }
                            color: AppTheme.text
                            font.pixelSize: 15
                            font.weight: Font.ExtraBold
                        }
                        Icon {
                            id: encryptionLock
                            visible: root.currentRoom.encrypted === true
                            name: "lock"
                            size: 13
                            color: AppTheme.textMuted
                            Accessible.role: Accessible.StaticText
                            Accessible.name: qsTr("Room encrypted")
                            HoverHandler { id: encryptionLockHover }
                            ToolTip.text: qsTr("Room encrypted")
                            ToolTip.visible: encryptionLockHover.hovered
                            ToolTip.delay: 600
                        }
                    }
                    Label {
                        text: root.currentRoom.topic || ""
                        color: AppTheme.textMuted
                        font.pixelSize: 12
                        visible: text.length > 0
                        Layout.fillWidth: true
                        elide: Label.ElideRight
                    }
                    // Clicking the header identity opens Room Information.
                    TapHandler {
                        enabled: app.currentRoomId !== "" && app.roomInfo.supported
                        onTapped: root.toggleRoomInfo()
                    }
                }
                Item { Layout.fillWidth: true }
                RowLayout {
                    spacing: AppTheme.spacing6
                    // Pinned-messages shortcut: shown only when the room
                    // actually has pins, so users reach the list in one
                    // click instead of Room Information → Pinned.
                    IconButton {
                        objectName: "pinnedMessagesButton"
                        visible: app.currentRoomId !== ""
                                 && app.roomInfo.supported
                                 && app.pinned
                                 && app.pinned.supported
                                 && app.pinned.roomId === app.currentRoomId
                                 && app.pinned.total > 0
                        iconName: "push_pin"
                        active: root.infoOpen && infoPanel.section === "pinned"
                        Accessible.name: qsTr("Pinned messages")
                        ToolTip.text: qsTr("Pinned messages")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.togglePinnedPanel()
                    }
                    IconButton {
                        objectName: "threadsViewButton"
                        visible: app.currentRoomId !== "" && app.thread.supported
                        iconName: "forum"
                        active: root.threadSurfaceOpen
                        Accessible.name: qsTr("Threads")
                        ToolTip.text: qsTr("Threads in this room")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleThreadSurface()
                    }
                    IconButton {
                        objectName: "timelineSearchButton"
                        visible: app.currentRoomId !== ""
                        iconName: "search"
                        active: root.searchOpen
                        Accessible.name: qsTr("Search messages")
                        ToolTip.text: qsTr("Search room messages")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleSearchPanel()
                    }
                    IconButton {
                        objectName: "memberPanelButton"
                        visible: app.currentRoomId !== "" && app.roomInfo.supported
                        iconName: "group"
                        active: root.infoOpen && infoPanel.section === "people"
                        Accessible.name: qsTr("Members")
                        ToolTip.text: qsTr("Room members")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleMemberPanel()
                    }
                    IconButton {
                        objectName: "roomInfoButton"
                        visible: app.currentRoomId !== "" && app.roomInfo.supported
                        iconName: "info"
                        active: root.infoOpen && infoPanel.section !== "people"
                        Accessible.name: qsTr("Room information")
                        ToolTip.text: qsTr("Room information")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.toggleRoomInfo()
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // v0.7.x room upgrades — the banner half of banner-and-link.
        //
        // Matrix leaves an upgraded room in place and creates a
        // replacement. Lightning does NOT follow that automatically: the
        // old room stays open and readable and the successor is offered,
        // because a room transition discards navigation context and draft
        // state, and the tombstone naming the successor is state anyone
        // with the power level can send.
        //
        // An ordinary Layout child, exactly like findBar below and for the
        // same reason: this is PERSISTENT, so it must reflow the timeline
        // rather than occlude it (the zero-height overflow trick further
        // down is documented as being for transient feedback only).
        //
        // The wording is Lightning's own. The tombstone's `body` is free
        // text chosen by whoever sent the state event, on a control the
        // user is being invited to click — it never crosses the FFI at all.
        Rectangle {
            id: roomUpgradeBanner
            objectName: "roomUpgradeBanner"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8
            // A room can be BOTH an opened successor and the predecessor of
            // another room, so the two rows are independent rather than
            // exclusive.
            readonly property bool showUpgraded: app.roomUpgrade.upgraded
            readonly property bool showPredecessor:
                app.roomUpgrade.predecessorRoomId.length > 0
            visible: showUpgraded || showPredecessor
            implicitHeight: upgradeCol.implicitHeight + AppTheme.spacingS * 2
            radius: AppTheme.radiusLg
            color: AppTheme.surface
            border.color: AppTheme.border
            border.width: 1

            ColumnLayout {
                id: upgradeCol
                anchors.fill: parent
                anchors.margins: AppTheme.spacingS
                spacing: AppTheme.spacingS

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS
                    visible: roomUpgradeBanner.showUpgraded

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("This room has been upgraded.")
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.fontBody
                        font.family: AppTheme.uiFont
                        wrapMode: Text.WordWrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    AppButton {
                        objectName: "roomUpgradeContinueButton"
                        text: qsTr("Continue in new room")
                        kind: "primary"
                        enabled: !app.roomUpgrade.busy
                        onClicked: app.roomUpgrade.continueToSuccessor()
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Continue in the new room")
                        Accessible.description: qsTr("Opens the room that replaced this one, joining it first if you are not already a member.")
                    }
                }

                // The failure is shown HERE, in the old room, because the
                // user must be able to see why Continue did not work
                // without having been moved anywhere.
                Text {
                    objectName: "roomUpgradeError"
                    Layout.fillWidth: true
                    visible: app.roomUpgrade.error.length > 0
                    text: app.roomUpgrade.error
                    color: AppTheme.danger
                    font.pixelSize: AppTheme.fontSecondary
                    font.family: AppTheme.uiFont
                    wrapMode: Text.WordWrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS
                    visible: roomUpgradeBanner.showPredecessor

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("This room replaced an earlier one.")
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.fontSecondary
                        font.family: AppTheme.uiFont
                        wrapMode: Text.WordWrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    AppButton {
                        objectName: "roomUpgradePreviousButton"
                        text: qsTr("Previous room")
                        kind: "secondary"
                        // No join step: the predecessor is a room the user
                        // was already in.
                        onClicked: app.roomUpgrade.goToPredecessor()
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Open the previous room")
                    }
                }
            }
        }

        // v0.6.1: find-in-loaded-messages bar.
        // v0.6.5 (C7): re-hosted as a composer-family floating card — outer
        // Layout margins detach it from the timeline's edges, AppTextField
        // supplies the themed border/focus-halo/search-icon/clear-button
        // chrome instead of a hand-rolled field, and the card keeps a fixed
        // compact height (AppTextField's implicitHeight is a hard 32px
        // constant; its own focus border-width change never feeds back into
        // it) so opening, closing, or focusing the field never reflows
        // anything else. It stays an ordinary Layout child rather than an
        // absolute overlay on purpose: `timeline`'s onHeightChanged handler
        // below already treats a find-bar-driven height change as a
        // first-class, already-solved case ("Viewport resizes (window,
        // right panel, find bar) keep the same reading position") — a true
        // floating overlay would either occlude the newest/anchored message
        // or require re-deriving that same content-inset compensation from
        // scratch for no behavioral gain.
        Rectangle {
            id: findBar
            objectName: "timelineFindBar"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing8
            visible: root.findOpen
            implicitHeight: findCol.implicitHeight + AppTheme.spacingS * 2
            radius: AppTheme.radiusLg
            color: AppTheme.surface
            border.color: AppTheme.border
            border.width: 1
            ColumnLayout {
                id: findCol
                anchors.fill: parent
                anchors.margins: AppTheme.spacingS
                spacing: AppTheme.spacingS
            RowLayout {
                id: findRow
                Layout.fillWidth: true
                spacing: AppTheme.spacingS
                // Loaded-messages find vs server history search. The
                // history segment exists only where it can be honest.
                SegmentedControl {
                    objectName: "findModeToggle"
                    visible: root.findHistoryAvailable
                    dense: true
                    model: [
                        { label: qsTr("Loaded"), value: "loaded" },
                        { label: qsTr("History"), value: "history" }
                    ]
                    current: root.findHistoryMode ? "history" : "loaded"
                    onActivated: (value) => {
                        var wantHistory = value === "history"
                        if (wantHistory === root.findHistoryMode)
                            return
                        root.findHistoryMode = wantHistory
                        if (wantHistory) {
                            app.timeline.endSearch()
                            app.messageSearch.roomId = app.currentRoomId
                            app.messageSearch.filters = ({})
                            app.messageSearch.query = findField.text
                        } else {
                            app.messageSearch.query = ""
                            app.timeline.beginSearch(findField.text)
                        }
                        findField.forceActiveFocus()
                    }
                }
                AppTextField {
                    id: findField
                    objectName: "timelineFindField"
                    Layout.fillWidth: true
                    searchIcon: true
                    clearButton: true
                    placeholderText: root.findHistoryMode
                                     ? qsTr("Search this room's history…")
                                     : qsTr("Search visible messages…")
                    Accessible.name: root.findHistoryMode
                                     ? qsTr("Search room history")
                                     : qsTr("Find in loaded messages")
                    onTextChanged: {
                        if (!root.findOpen)
                            return
                        if (root.findHistoryMode) {
                            // Review M1: the controller is SHARED with the
                            // global dialog, which rescopes it to "". Every
                            // find-bar dispatch re-asserts this room, so a
                            // dialog round-trip can never make the bar show
                            // other rooms' results under this room's label.
                            app.messageSearch.roomId = app.currentRoomId
                            app.messageSearch.query = text
                        } else {
                            app.timeline.updateSearch(text)
                        }
                    }
                    Keys.onReturnPressed: (event) => {
                        if (root.findHistoryMode) {
                            app.messageSearch.roomId = app.currentRoomId
                            app.messageSearch.search()
                        } else if (event.modifiers & Qt.ShiftModifier) {
                            app.timeline.searchPrev()
                        } else {
                            app.timeline.searchNext()
                        }
                        event.accepted = true
                    }
                    Keys.onEscapePressed: root.closeFind()
                }
                Label {
                    objectName: "timelineFindCount"
                    visible: !root.findHistoryMode
                    text: app.timeline.searchResultCount > 0
                          ? qsTr("%1 of %2").arg(app.timeline.searchCurrentPosition)
                                            .arg(app.timeline.searchResultCount)
                          : (findField.text.length > 0 ? qsTr("No matches") : "")
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                }
                IconButton {
                    visible: !root.findHistoryMode
                    implicitWidth: 28; implicitHeight: 28
                    radius: 6
                    iconName: "expand_less"
                    iconSize: 16
                    enabled: app.timeline.searchResultCount > 0
                    Accessible.name: qsTr("Previous match")
                    onClicked: app.timeline.searchPrev()
                }
                IconButton {
                    visible: !root.findHistoryMode
                    implicitWidth: 28; implicitHeight: 28
                    radius: 6
                    iconName: "expand_more"
                    iconSize: 16
                    enabled: app.timeline.searchResultCount > 0
                    Accessible.name: qsTr("Next match")
                    onClicked: app.timeline.searchNext()
                }
                IconButton {
                    implicitWidth: 28; implicitHeight: 28
                    radius: 6
                    iconName: "close"
                    iconSize: 16
                    Accessible.name: qsTr("Close find")
                    onClicked: root.closeFind()
                }
            }

            // v0.7.x: history-mode results (server /search, this room).
            ListView {
                id: historyResultsList
                objectName: "historySearchResultsList"
                visible: root.findHistoryMode
                Layout.fillWidth: true
                Layout.preferredHeight: visible && count > 0
                                        ? Math.min(280, contentHeight) : 0
                clip: true
                spacing: 2
                model: root.findHistoryMode ? app.messageSearch : null
                ScrollBar.vertical: ScrollBar {}
                onAtYEndChanged: {
                    if (atYEnd && app.messageSearch.canLoadMore)
                        app.messageSearch.loadMore()
                }
                delegate: Rectangle {
                    id: historyRow
                    required property int index
                    required property string eventId
                    required property string sender
                    required property string senderDisplayName
                    required property var timestampMs
                    required property string body
                    width: historyResultsList.width
                    height: historyRowCol.implicitHeight + AppTheme.spacing8
                    radius: AppTheme.radiusMd
                    color: historyHover.hovered ? AppTheme.hover : "transparent"
                    HoverHandler { id: historyHover }
                    TapHandler {
                        onTapped: {
                            // The shared navigation path: paginate until the
                            // event is loaded, then centre + highlight. Deep
                            // history past its bounded window reports its
                            // honest "unavailable" message.
                            app.pagination.jumpToEvent(historyRow.eventId)
                        }
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Jump to message")
                    ColumnLayout {
                        id: historyRowCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: AppTheme.spacingS
                        anchors.rightMargin: AppTheme.spacingS
                        spacing: 1
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: historyRow.senderDisplayName.length > 0
                                      ? historyRow.senderDisplayName
                                      : historyRow.sender
                                color: AppTheme.text
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                text: {
                                    var d = new Date(Number(
                                        historyRow.timestampMs))
                                    return d.toLocaleDateString(
                                        Qt.locale(), Locale.ShortFormat)
                                }
                                color: AppTheme.textMuted
                                font.pixelSize: 11
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: historyRow.body
                            color: AppTheme.textSecondary
                            font.pixelSize: 12
                            elide: Label.ElideRight
                            maximumLineCount: 1
                        }
                    }
                }
            }
            Label {
                visible: root.findHistoryMode
                         && app.messageSearch.state !== "idle"
                Layout.fillWidth: true
                text: app.messageSearch.state === "loading"
                      ? qsTr("Searching…")
                      : app.messageSearch.state === "loading_more"
                        ? qsTr("Loading more…")
                        : app.messageSearch.state === "no_results"
                          ? qsTr("No messages found in this room's history")
                          : app.messageSearch.state === "error"
                            ? qsTr("The search could not be completed.")
                            : ""
                color: AppTheme.textMuted
                font.pixelSize: 11
                elide: Label.ElideRight
            }
            }
        }

        // Timeline
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ── Solid timeline: no height virtualization ─────────────────
            // Every loaded row is a real, instantiated item inside one Column,
            // so contentHeight and every row position are MEASURED. There is
            // no per-row estimate, no rowHeightProvider, no content-height
            // reconciliation and no delegate recycling anywhere in the room
            // timeline.
            //
            // This replaced a reversed TableView, and the reason is structural
            // rather than a matter of tuning. Two properties of that design
            // could not be fixed from the outside:
            //
            //  1. QQuickTableViewPrivate::rowsInsertedCallback schedules
            //     RebuildOption::ViewportOnly for an insert ANYWHERE in the
            //     model. That releases every visible delegate to the reuse
            //     pool and re-binds it, so each pagination page re-ran
            //     onReused, the height-seed settle, every nested Loader and
            //     every media identity check for every row on screen. That is
            //     a full viewport rebuild per page, and it is the stall the
            //     reader feels as history loads. Element never does this;
            //     prepending does not touch what is already rendered.
            //  2. contentHeight had to be supplied from metadata ESTIMATES
            //     while TableView laid the loaded rows out at their real
            //     heights. Two coordinate systems that disagree by
            //     construction, with origin/endExtent silently absorbing the
            //     difference. Every position the app computed lived in the
            //     estimated frame; everything the reader saw lived in the real
            //     one. Repeated attempts to reconcile them all failed.
            //
            // With a Column both problems cease to exist rather than being
            // balanced. Older history is a proxy APPEND, so it extends the
            // column at the tail: no item already positioned can move, and
            // contentY needs no correction at all for pagination. Rows keep
            // their identity for as long as they are loaded, so nothing is
            // ever rebound underneath the reader.
            //
            // The cost is that the whole loaded window is instantiated. That
            // is Element's trade too. Rows arrive ~20 at a time as the reader
            // pages, never in one burst, and a hidden row (a filtered activity
            // line) is skipped by the positioner and occupies exactly zero
            // height with no special case.
            Flickable {
                id: timeline
                objectName: "timelineListView"
                anchors.fill: parent
                clip: true
                // Rotate the viewport and counter-rotate each row: proxy row 0
                // (newest) sits physically at the bottom, and older history
                // extends the far/top edge.
                rotation: 180
                flickableDirection: Flickable.VerticalFlick
                // The presentation gate covers the view (it keeps laying out
                // underneath so viewport-fill pagination and positioning run
                // against real geometry); the loading surface sits on top.
                opacity: presentationReady ? 1 : 0
                // The source stays chronological for every backend and
                // non-visual consumer. The proxy exposes newest-to-oldest.
                readonly property int count: rowRepeater.count
                contentWidth: width
                contentHeight: rowColumn.height

                // ── Which rows may activate their heavy content ──────────
                // Every loaded row is instantiated now, so there can be many
                // hundreds of live delegates. Each one asking "am I on screen?"
                // in terms of contentY re-evaluates that binding on EVERY row
                // on EVERY pixel scrolled — hundreds of float comparisons per
                // frame that all answer the same question. That was a large
                // part of the timeline feeling heavy overall.
                //
                // Compute the range ONCE per turn instead and let each row do a
                // pair of integer comparisons against it. The range only
                // changes when a row boundary is crossed, so the per-row
                // bindings stay quiet through most of a gesture.
                //
                // One viewport of slack on each side means content activates
                // before it is reached rather than popping in at the edge.
                property int visibleFirstRow: 0
                property int visibleLastRow: -1
                // A child Timer rather than Qt.callLater: the timer dies with
                // the pane, whereas a queued callLater still fires after the
                // object has been torn down (room switch, logout) and throws
                // because its QML methods are already gone. restart()
                // coalesces repeat requests for free.
                Timer {
                    id: visibleRowRangeTimer
                    interval: 0
                    onTriggered: timeline.updateVisibleRowRange()
                }
                function updateVisibleRowRange() {
                    if (count <= 0) {
                        visibleFirstRow = 0
                        visibleLastRow = -1
                        return
                    }
                    var first = viewRowAtContentY(contentY - height)
                    var last = viewRowAtContentY(contentY + height * 2)
                    visibleFirstRow = first < 0 ? 0 : first
                    visibleLastRow = last < 0 ? count - 1 : last
                }
                function scheduleVisibleRowRange() {
                    visibleRowRangeTimer.restart()
                }

                Column {
                    id: rowColumn
                    // Inset by the side margins directly. A Flickable's
                    // left/right margins only widen the flickable RANGE; they
                    // do not lay content out, and this view never scrolls
                    // horizontally. (The 180° rotation swaps left and right,
                    // which is harmless while the two are equal.)
                    x: timeline.leftMargin
                    // Rows can be built before the pane has its final width. A
                    // non-positive width makes a long wrapped body measure as
                    // one character wide, producing an enormous transient
                    // height. Fall back to a normal message column until the
                    // real viewport width is positive.
                    width: {
                        var available = timeline.width - timeline.leftMargin
                                      - timeline.rightMargin
                        return available > 0 ? available : 640
                    }
                    // Delegates own sender-group spacing: group leaders
                    // receive a compact break while continuations stay
                    // visually glued together. A global gap made every
                    // continuation look like an unrelated row.
                    spacing: 0

                    Repeater {
                        id: rowRepeater
                        model: app.timelineView
                        // Rows are built SYNCHRONOUSLY and are direct children
                        // of the Column. Wrapping them in an asynchronous
                        // Loader was tried and was much worse: an incubating
                        // row has zero height until it finishes, so a page
                        // materialised out of order and visibly squeezed the
                        // column, and the extra item plus incubation overhead
                        // per row made everything slower rather than smoother.
                        delegate: MessageDelegate {
                            width: rowColumn.width
                            rotation: 180
                            // No attached view exists inside a Repeater, so
                            // the row's view reference is injected. This also
                            // deliberately withholds the height-seed/recycling
                            // API (cachedDelegateHeight, rememberDelegateHeight)
                            // that only the reused ListView path needs, so the
                            // delegate falls through to its own natural height
                            // — which here is simply the truth.
                            timelineView: timeline
                            // An INDEX-RANGE test, never a geometry test. See
                            // visibleFirstRow below: with every loaded row
                            // instantiated, a per-row binding on contentY costs
                            // one float comparison per row per pixel scrolled.
                            rowOnScreen: index >= timeline.visibleFirstRow
                                         && index <= timeline.visibleLastRow
                        }
                    }
                }

                // NOTE: there is deliberately no height model here any more.
                // The Column measures itself; contentHeight above is that
                // measurement. Row insertions need no bookkeeping because an
                // append cannot move an item that is already positioned.
                // Lightning owns the scroll position (wheel/pixel motion writes
                // contentY directly, clamped to wheelMinY/wheelMaxY). Pin the
                // Flickable's own bounds to that same range so that if a wheel
                // event is ever also seen natively, it can neither overshoot nor
                // rubber-band past the clamp — no bounce, no kinetic tail
                // fighting the programmatic position. (Nheko uses the same
                // StopAtBounds on its timeline.)
                boundsBehavior: Flickable.StopAtBounds
                topMargin: AppTheme.spacingM
                bottomMargin: AppTheme.spacingM
                // v0.6.7, corrected: the avatar gutter is `rightMargin`, NOT
                // `leftMargin`. Two effects compound here and the first
                // attempt at this missed both:
                //
                //   1. `rotation: 180` above swaps left and right, so the
                //      LOGICAL right margin is the VISUAL left inset;
                //   2. at its left bound a Flickable parks contentX at
                //      -leftMargin, which offsets the content item by
                //      +leftMargin on top of rowColumn's own
                //      `x: timeline.leftMargin`.
                //
                // Net: visual left inset == rightMargin - leftMargin. With
                // both at 12 that was 0, which is why the avatars read as
                // glued to the SplitView handle. Raising leftMargin to 20
                // made it -8 and pushed the avatar column out over the
                // divider — the reported regression. The gutter has to come
                // from the far side.
                //
                // Visual right inset is 2 * leftMargin (24), unchanged.
                readonly property real avatarGutter: 20
                leftMargin: AppTheme.spacingM
                rightMargin: AppTheme.spacingM + avatarGutter

                // Auto-scroll to end on new events when already near the bottom.
                property bool stickToBottom: true

                // ── Bottom-follow ownership ──────────────────────────────
                // `stickToBottom` (FollowingBottom) is LATCHED to user intent,
                // not guessed from proximity. It is recomputed ONLY on user
                // input (wheel/drag/settle), never from asynchronous content
                // growth, so once a reader scrolls up it stays disengaged until
                // they deliberately return to the very end (or hit Jump to
                // latest / open a fresh room). Proximity must never RE-PIN a
                // reader who scrolled up: that re-pin, followed by the next
                // async onContentHeightChanged -> positionViewAtEnd, was the
                // "scroll up teleports me back to the bottom" fight. The slack
                // is deliberately smaller than one message line, so "scroll up
                // to re-read" always disengages, while a downward flick that
                // stops a hair short of the end still resumes following.
                readonly property real bottomFollowSlack: 8
                function atBottomEdge() {
                    return atYBeginning
                           || contentY <= wheelMinY() + bottomFollowSlack
                }

                // ── v0.7: initial-hydration presentation gate ────────────
                // A freshly opened room must not present a one-item partial
                // snapshot that visibly rebuilds while the SDK event cache
                // and the automatic viewport fill catch up. The timeline
                // stays covered by the loading surface until the room is
                // coherently presentable:
                //   * the loaded content already fills the viewport, or
                //   * the backend reports the initial automatic fill settled
                //     (batch landed / start of history / stopped / failed).
                // A bounded guard timer is a last-resort safety valve for a
                // hung backend, never the primary mechanism. The gate is
                // monotonic per room generation: once presented, later diffs
                // apply normally with anchor preservation.
                property bool presentationReady: app.currentRoomId === ""
                // Set while a room reset is in flight. Rows are built into the
                // Column synchronously now, so contentHeight moves DURING the
                // reset and onContentHeightChanged would recompute the gate
                // right then — reading the outgoing room's settled state and
                // opening on a one-item partial snapshot, which is the exact
                // defect the gate exists to prevent. Under the previous
                // virtualized view contentHeight was application-owned and only
                // updated a turn later, so the deferred recompute was enough on
                // its own. It no longer is.
                property bool presentationResetPending: false
                function recomputePresentationReady() {
                    if (presentationReady || presentationResetPending)
                        return
                    if (app.currentRoomId === "") {
                        presentationReady = true
                        presentationGuard.stop()
                        return
                    }
                    var fillsViewport = count > 0
                                        && contentHeight >= height - 1
                    if (fillsViewport || app.pagination.initialContentSettled) {
                        presentationReady = true
                        presentationGuard.stop()
                        // Present at the intended position: one deliberate
                        // deferred end-anchor when following the latest
                        // message (anchor restoration positioned any saved
                        // reading spot while the gate was still closed).
                        if (stickToBottom)
                            Qt.callLater(scrollToEndDeferred)
                    }
                }
                Timer {
                    id: presentationGuard
                    interval: 2500
                    onTriggered: {
                        if (!timeline.presentationReady) {
                            timeline.presentationReady = true
                            if (timeline.stickToBottom)
                                Qt.callLater(timeline.scrollToEndDeferred)
                        }
                    }
                }
                Connections {
                    target: app.pagination
                    function onStateChanged() {
                        timeline.recomputePresentationReady()
                    }
                }

                // ── v0.7: persistent viewport anchor ─────────────────────
                // While the user is reading older history, asynchronous row
                // growth (media hydration, late decryption, link previews,
                // profile resolution) above the viewport must not move the
                // message under the cursor. The anchor is the first visible
                // stable item id plus its pixel offset; every coalesced
                // content-height change re-aligns to it. Bottom-pinned and
                // in-motion states are owned by their own mechanisms. As of
                // v0.7.2 this is the ONLY position-preserving mechanism: the
                // backward-pagination prepend used to keep a dedicated
                // capture/restore pair, and that duplication is what four
                // rounds of live-reported scroll bugs were about.
                property string viewAnchorId: ""
                property real viewAnchorOffset: 0
                // The anchor row's own content-space y at the moment it was
                // last measured — either at capture or after the most recent
                // geometry pass. maintainViewAnchor() re-bases this
                // on every application so the NEXT delta only ever covers
                // growth that happened since then, never growth already
                // compensated (no double-counting across repeated calls).
                property real viewAnchorLastY: 0
                // Row count at the last anchor measurement. The displaced-
                // anchor probe below is only warranted when rows were
                // INSERTED (a prepend always changes count); when the reader
                // has merely scrolled more than a cache-buffer away from the
                // anchor, the delegate is missing for a cheap reason and the
                // re-capture fallback is the right answer. Without this the
                // probe's forceLayout + positionViewAtIndex would run on
                // every coalesced height change in that state, sweeping
                // delegate creation (and media requests) across the rows it
                // passes — the exact cost profile that made loading laggy.
                property int viewAnchorCount: 0
                // Row index, content height, and ListView origin at the last
                // measurement. When
                // an insertion displaces the anchor beyond the delegate cache
                // (a prepend while the reader sits at the top edge), these
                // two give the shift ARITHMETICALLY — the row index rising
                // proves rows were inserted ABOVE the reader, and the content
                // height growth is how tall they were — so the correction
                // needs no delegate materialised for it. That matters
                // enormously: forcing the row into existence sweeps delegate
                // creation and media requests across every row it passes,
                // once per loaded batch, which is what made scrolling up in a
                // BIG room lag while smaller rooms felt fine.
                property int viewAnchorRow: -1
                property real viewAnchorContentHeight: 0
                // Diagnostic baseline for the next live-scroll experiment.
                // ListView moves originY when rows are prepended, but it also
                // moves it while revising the average-height estimate for
                // delegates it has not built. Recording it beside the proven
                // row-index increase lets a physical trace distinguish those
                // two causes without changing any scroll correction yet.
                property real viewAnchorOriginY: 0
                function captureViewAnchor() {
                    if (stickToBottom || count === 0) {
                        viewAnchorId = ""
                        viewAnchorLastY = 0
                        viewAnchorCount = count
                        viewAnchorRow = -1
                        viewAnchorContentHeight = contentHeight
                        if (scrollTrace)
                            viewAnchorOriginY = originY
                        return
                    }
                    var row = viewRowAtPhysicalTop()
                    if (row < 0) {
                        viewAnchorId = ""
                        viewAnchorLastY = 0
                        return
                    }
                    var it = itemAtViewRow(row)
                    for (var probe = row; probe >= 0; --probe) {
                        var candidate = itemAtViewRow(probe)
                        if (!candidate)
                            break
                        if (!candidate.isStateActivity) {
                            row = probe
                            it = candidate
                            break
                        }
                    }
                    viewAnchorId = stableIdAtViewRow(row)
                    var anchorPosition = anchorPositionForItem(it)
                    viewAnchorOffset = it ? contentY - anchorPosition : 0
                    viewAnchorLastY = anchorPosition
                    viewAnchorCount = count
                    viewAnchorRow = row
                    viewAnchorContentHeight = contentHeight
                    if (scrollTrace)
                        viewAnchorOriginY = originY
                }
                property bool viewAnchorScheduled: false
                function maintainViewAnchorCoalesced() {
                    if (viewAnchorScheduled)
                        return
                    viewAnchorScheduled = true
                    Qt.callLater(function() {
                        viewAnchorScheduled = false
                        timeline.maintainViewAnchor()
                    })
                }
                function maintainViewAnchor() {
                    if (viewAnchorId === "" || stickToBottom) {
                        // Diagnostics only: distinguishes "the mechanism ran
                        // and had nothing to correct" (every other counter
                        // legitimately 0) from "the mechanism never engaged
                        // at all" — see the M2 comment on diagNoAnchorReturns.
                        // The two early-return causes are counted SEPARATELY,
                        // not merged — "no anchor captured yet"
                        // (viewAnchorId empty: a real gap in anchor
                        // coverage, e.g. a gesture that started before
                        // captureViewAnchor() ever ran) and "returned to the
                        // bottom before this coalesced call fired"
                        // (stickToBottom). The second is NOT the routine
                        // bottom-pinned case: this function is never
                        // scheduled while stickToBottom is already true, so
                        // it can only count a correction scheduled while
                        // scrolled up and then dropped on return-to-bottom.
                        // Order matters and is deliberate: an empty id wins
                        // when both hold.
                        if (scrollTrace) {
                            if (viewAnchorId === "")
                                diagNoAnchorReturns += 1
                            else
                                diagStickToBottomReturns += 1
                        }
                        return
                    }
                    // v0.7.2: the SINGLE anchor-correction mechanism for
                    // every structural change that can move the anchor row's
                    // own y — a backward-pagination prepend, a pooled
                    // delegate re-measuring after rebinding to a taller row,
                    // late decryption, a link preview resolving. Pagination
                    // used to run a SEPARATE capture/restore pair alongside
                    // this one, and four rounds of live reports were entirely
                    // about the TWO mechanisms coordinating. The revert that
                    // ended that sequence is easy to misread as "don't
                    // compensate during pagination"; the actual lesson is
                    // narrower — don't run TWO mechanisms that both write
                    // contentY and must reason about which one currently owns
                    // the correction. With one mechanism there is nothing to
                    // coordinate with: this runs once per coalesced
                    // onContentHeightChanged regardless of WHY height
                    // changed, and a filtered batch that inserts nothing
                    // fires no such signal, so it needs no keep-alive
                    // bookkeeping. Do NOT reintroduce a pagination stand-down
                    // guard here: that guard is what created those bugs.
                    var row = viewRowForStableId(viewAnchorId)
                    var it = row >= 0 ? itemAtViewRow(row) : null
                    var anchorY = anchorPositionForItem(it)
                    // Diagnostics only. A stable row index increasing is the
                    // proof that rows were inserted before the anchor. On that
                    // exact firing, pair the signed origin shift candidate
                    // (`old originY - new originY`) with the whole-content
                    // delta currently used by the displaced branch. This is
                    // the missing live measurement: originY may be quieter,
                    // equally noisy, or worse, and production behavior must
                    // not guess which before the physical trace answers it.
                    if (scrollTrace && row > viewAnchorRow && viewAnchorRow >= 0) {
                        var insertedRowsForOriginDiag = row - viewAnchorRow
                        var originShiftForDiag = viewAnchorOriginY - originY
                        var contentDeltaForOriginDiag =
                                contentHeight - viewAnchorContentHeight
                        var originPathForDiag = !it
                                ? ((!moving) ? "displaced" : "fallback")
                                : (userScrollActive
                                   ? (selfDrivenScrollActive
                                      ? "materialized" : "drag")
                                   : "idle")
                        diagPrependFirings += 1
                        diagPrependOriginShiftSum += originShiftForDiag
                        if (diagPrependFirings === 1
                                || Math.abs(originShiftForDiag)
                                > Math.abs(diagPrependMaxAbsOriginShift)) {
                            diagPrependMaxAbsOriginShift = originShiftForDiag
                            diagPrependMaxAbsOriginShiftRows =
                                    insertedRowsForOriginDiag
                            diagPrependMaxAbsOriginShiftContentDelta =
                                    contentDeltaForOriginDiag
                            diagPrependMaxAbsOriginShiftPath = originPathForDiag
                        }
                    }
                    if (!it && row >= 0 && !moving && !userScrollActive
                        && row > viewAnchorRow && viewAnchorRow >= 0) {
                        // DISPLACED by an insertion above the reader. The row
                        // index rising is the proof: rows were inserted
                        // before it, which at the top edge pushes it clear of
                        // the delegate cache so Qt destroys it. This estimate-
                        // based fallback is IDLE-ONLY: active input falls
                        // through to the measurement-only re-capture below,
                        // because a real trace proved this quantity can cancel
                        // essentially an entire upward wheel gesture.
                        //
                        // The previous version resolved this by forcing the
                        // row into existence (forceLayout, then
                        // positionViewAtIndex). That is correct but ruinously
                        // expensive on exactly the reported path: in a big
                        // room every scroll-up loads a page, and every page
                        // then swept delegate creation and media requests
                        // across the intervening rows. Small rooms load
                        // rarely, which is why they felt fine.
                        //
                        // Bounded inaccuracy, deliberately accepted: if a
                        // live message is appended BELOW the reader in the
                        // same coalesced turn, its height is included here
                        // too. That is one row's worth, versus a full-view
                        // sweep per batch.
                        //
                        // POSITIVE net only. `grew` is a WHOLE-CONTENT delta, so
                        // a negative value is dominated by either ListView's
                        // average-size estimate for rows it has not built or a
                        // below-viewport shrink — and neither is something this
                        // quantity can correct for. (Not "a negative can never
                        // be a displacement": +1000 above the reader coalesced
                        // with -1200 below yields -200 while a real displacement
                        // happened. Applying -200 would then be wrong by 1200;
                        // applying 0 is wrong by 1000. Skipping is the better of
                        // two approximations, not an exact answer.)
                        //
                        // A live trace of an upward scroll through a media-heavy
                        // room showed contentHeight swinging ±17000 px across
                        // single gestures with no model change at all, with the
                        // correction counter spiking on exactly those gestures.
                        // Feeding that noise into contentY is itself jitter.
                        // A skipped shrink is ABSORBED at settle, not repaired:
                        // captureViewAnchor() records `contentY - it.y` as the
                        // new offset, i.e. it accepts wherever the reader now is
                        // as correct. So the cost is bounded and one-time, while
                        // over-correcting on noise is visible every frame.
                        // viewAnchorLastY is deliberately NOT advanced in the
                        // skipped case: in the dominant pure-noise case the
                        // row's own y has not moved, so leaving the baseline
                        // alone yields delta 0 when the delegate returns —
                        // advancing it would MANUFACTURE a jump there.
                        var grew = contentHeight - viewAnchorContentHeight
                        // Diagnostics only (see the diagDisplaced* block
                        // above) — discriminates the reviewer's H1: whether
                        // this branch fires at all during a real jitter
                        // gesture, and whether |grew| stays proportionate to
                        // insertedRows (real growth) or spikes far beyond
                        // what that many rows could plausibly account for
                        // (the estimate-noise hypothesis). Recorded on EVERY
                        // entry, including the skipped-negative case, so a
                        // real trace can show the swing this branch actually
                        // saw even when nothing was applied. Gated on
                        // scrollTrace, not diagActive — see the M1 comment on
                        // the diagDisplaced* declarations for why this must
                        // survive past the gesture that triggered it.
                        if (scrollTrace) {
                            diagDisplacedFirings += 1
                            var insertedRowsForDiag = row - viewAnchorRow
                            var displacedOriginShiftForDiag =
                                    viewAnchorOriginY - originY
                            if (Math.abs(grew) > Math.abs(diagDisplacedMaxAbsGrew)) {
                                diagDisplacedMaxAbsGrew = grew
                                diagDisplacedMaxAbsGrewRows = insertedRowsForDiag
                                diagDisplacedMaxAbsGrewOriginShift =
                                        displacedOriginShiftForDiag
                            }
                            if (diagDisplacedFirings === 1
                                    || Math.abs(displacedOriginShiftForDiag)
                                    > Math.abs(diagDisplacedMaxAbsOriginShift)) {
                                diagDisplacedMaxAbsOriginShift =
                                        displacedOriginShiftForDiag
                                diagDisplacedMaxAbsOriginShiftContentDelta = grew
                                diagDisplacedMaxAbsOriginShiftRows =
                                        insertedRowsForDiag
                            }
                        }
                        if (grew > 0.5) {
                            if (scrollTrace) {
                                diagGrowthCorrections += 1
                                diagDisplacedAppliedSum += grew
                            }
                            contentY += grew
                            viewAnchorLastY += grew
                        }
                        viewAnchorCount = count
                        viewAnchorRow = row
                        viewAnchorContentHeight = contentHeight
                        if (scrollTrace)
                            viewAnchorOriginY = originY
                        return
                    }
                    if (!it) {
                        // Genuinely unresolvable — the stable id no longer
                        // exists (redaction, local-echo id change), or a
                        // native drag owns the view. Re-measuring a fresh
                        // anchor writes no position (pure measurement), so it
                        // is safe even mid-gesture; the next call's delta is
                        // then measured from this new baseline rather than a
                        // stale, unresolvable one.
                        // Diagnostics only (see the L1 comment on the
                        // diagUnresolvedIdFallbacks/diagEvictedNoInsertFallbacks
                        // declarations): split by whether the stable id
                        // still resolves to a real row at all. row < 0 means
                        // it does not (redaction, local-echo id swap, or a
                        // genuinely stale/unknown id) — a true "give up and
                        // re-derive". row >= 0 means the id is fine and the
                        // delegate was simply evicted from the cache with no
                        // row-index proof anything was inserted above it
                        // (including the moving===true case, where the
                        // displaced branch's arithmetic path is blocked even
                        // if row > viewAnchorRow, and the viewAnchorRow < 0
                        // case where no row was ever recorded) — expected to
                        // dominate in a media-heavy room on ordinary
                        // scrolling alone. Counters are not reset on a room
                        // switch, so the first line after one can carry the
                        // previous room's outcomes ("since the previous
                        // line" applies across rooms too).
                        if (userScrollActive) {
                            var deferredDisplaced = row > viewAnchorRow
                                    && viewAnchorRow >= 0
                                    ? contentHeight - viewAnchorContentHeight
                                    : 0
                            diagNoteActiveDeferral(deferredDisplaced)
                        }
                        if (scrollTrace) {
                            if (row < 0)
                                diagUnresolvedIdFallbacks += 1
                            else
                                diagEvictedNoInsertFallbacks += 1
                        }
                        captureViewAnchor()
                        return
                    }
                    if (userScrollActive) {
                        // deferredDelta is how far the anchor row's own y has
                        // moved since it was last measured. Screen position is
                        // (item y - contentY), so moving contentY by the same
                        // amount holds the reader's message exactly where it
                        // was while the rows around it resize.
                        var deferredDelta = anchorY - viewAnchorLastY
                        diagNoteActiveDeferral(deferredDelta)
                        if (scrollTrace) {
                            diagMaterializedFirings += 1
                            if (Math.abs(deferredDelta)
                                    > Math.abs(diagMaterializedMaxAbsDelta))
                                diagMaterializedMaxAbsDelta = deferredDelta
                            if (!selfDrivenScrollActive)
                                diagDragDeferrals += 1
                        }
                        // NO WRITE. This has now been tried twice and rejected
                        // by physical testing twice, the second time with the
                        // height cache alive and with translateActiveMotion()
                        // carrying the wheel target along — so neither "the
                        // quantity was estimate noise" nor "the engine drove
                        // the correction back out" explains it. Applying it
                        // pulled the reader both up AND down during loading,
                        // and down during ordinary scrolling with nothing
                        // loading at all.
                        //
                        // The lesson is about WHAT the delta measures, not how
                        // it is applied: anchorY moves for reasons that are not
                        // displacement of the reader. TableView re-anchors the
                        // loaded table on its own rebuilds, so the anchor row's
                        // y can change while the reader's view of it did not.
                        // Feeding that into contentY injects motion the reader
                        // never asked for. Do not re-enable this without a
                        // measurement that separates "the row moved under the
                        // reader" from "the table was re-anchored beneath both
                        // of them" — the raw delta cannot tell them apart.
                        //
                        // While input owns the viewport, accept Qt's live
                        // layout position and re-base the measurement. The next
                        // geometry change is measured from here, and settle
                        // captures the final reading position once more.
                        captureViewAnchor()
                        return
                    }
                    // Idle: no gesture to fight, so an absolute restore to
                    // the exact captured offset is safe. Static-case
                    // equivalence with the relative formula above: with
                    // nothing else moving contentY between measurements,
                    // `it.y + viewAnchorOffset` and `contentY + delta` are
                    // identical (viewAnchorOffset was defined as contentY
                    // minus the row's y at capture) — so a prepend landing
                    // while idle is restored exactly like one landing
                    // after input has gone quiet, just via the absolute form,
                    // which is only safe because nothing is competing for
                    // contentY.
                    var desired = anchorY + viewAnchorOffset
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    desired = desired < lo ? lo : (desired > hi ? hi : desired)
                    if (Math.abs(contentY - desired) > 0.5) {
                        if (scrollTrace)
                            diagAnchorCorrections += 1
                        contentY = desired
                    }
                    viewAnchorLastY = anchorY
                    viewAnchorCount = count
                    viewAnchorRow = row
                    viewAnchorContentHeight = contentHeight
                    if (scrollTrace)
                        viewAnchorOriginY = originY
                }

                // v0.6.0: MessageDelegate view contract — the room timeline
                // resolves stable-id actions against app.timeline and never
                // suppresses a row as a pinned thread root.
                property var timelineModel: app.timeline
                // Hand over the proxy's paced backlog immediately. Any path
                // that must address a specific event by row calls this first;
                // see ReverseListProxyModel::releaseAll().
                function releasePendingRows() {
                    if (app.timelineView && app.timelineView.releaseAll)
                        app.timelineView.releaseAll()
                }
                // View row <-> source row. The reversal is anchored on the
                // SOURCE total, never on `count`: the proxy paces newly
                // paginated history out over a few frames, so it can briefly
                // expose fewer rows than the model holds, and those two
                // numbers are then not the same. `count` still bounds which
                // view rows exist. Deriving the mapping from `count` instead
                // would renumber every visible row for as long as a page was
                // draining.
                function sourceRowForViewRowAtCount(row, rowCount) {
                    return row < 0 || row >= rowCount
                            ? -1 : app.timeline.count - 1 - row
                }
                function sourceRowForViewRow(row) {
                    return sourceRowForViewRowAtCount(row, count)
                }
                function viewRowForSourceRow(row) {
                    var viewRow = app.timeline.count - 1 - row
                    return row < 0 || viewRow < 0 || viewRow >= count
                            ? -1 : viewRow
                }
                function stableIdAtViewRow(row) {
                    return app.timeline.stableIdAt(sourceRowForViewRow(row))
                }
                function stableIdAtViewRowAtCount(row, rowCount) {
                    return app.timeline.stableIdAt(
                                sourceRowForViewRowAtCount(row, rowCount))
                }
                function eventIdAtViewRow(row) {
                    return app.timeline.eventIdAt(sourceRowForViewRow(row))
                }
                function viewRowForStableId(stableId) {
                    return viewRowForSourceRow(
                                app.timeline.rowForStableId(stableId))
                }
                // Every loaded row is instantiated, so this is a direct lookup
                // rather than "the delegate IF the virtualizer happens to have
                // built it". Nothing downstream has to handle a null for a row
                // that merely scrolled out of a cache buffer any more.
                function itemAtViewRow(row) {
                    return row < 0 || row >= count
                            ? null : rowRepeater.itemAt(row)
                }
                // Which row occupies a given content-space y. Rows are laid out
                // in ascending y by the Column, so this is a binary search over
                // real geometry — no average-size guess anywhere in it. Hidden
                // rows have zero height and are simply never the answer.
                function viewRowAtContentY(y) {
                    var lo = 0
                    var hi = count - 1
                    var best = -1
                    while (lo <= hi) {
                        var mid = (lo + hi) >> 1
                        var item = rowRepeater.itemAt(mid)
                        if (!item) {
                            hi = mid - 1
                            continue
                        }
                        if (item.y > y) {
                            hi = mid - 1
                        } else {
                            best = mid
                            lo = mid + 1
                        }
                    }
                    return best
                }
                function viewRowAtPhysicalTop() {
                    // Rotated: the physical top of the viewport is the far end
                    // of the visible content range.
                    return viewRowAtContentY(
                        contentY + Math.max(0, height - topMargin - 1))
                }
                // The view is rotated: the logical bottom edge of a row is its
                // physical top edge. Anchor that edge so row-height changes
                // retain the message under the reader.
                function anchorPositionForItem(item) {
                    return item ? item.y + item.height : 0
                }
                // Row 0 is the newest message and sits at content y 0, which
                // the rotation puts at the physical bottom. Following the
                // latest is therefore just the low bound — no alignment enum,
                // no second settling pass to correct an estimate.
                function positionViewAtLatest() {
                    contentY = wheelMinY()
                }
                function positionViewAtViewRow(row, centered) {
                    var item = itemAtViewRow(row)
                    if (!item)
                        return
                    // Place the row's physical top edge at the viewport's
                    // physical top (or its middle when centering). Exact, on
                    // measured geometry — this used to be positionViewAtRow(),
                    // whose alignment ran through TableView's uniform average
                    // row height.
                    var target = centered
                            ? anchorPositionForItem(item)
                              - (height + item.height) / 2
                            : anchorPositionForItem(item) - height + topMargin
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    contentY = target < lo ? lo : (target > hi ? hi : target)
                }
                property string suppressRootEventId: ""
                property bool threadContext: false

                // Which message currently has its action toolbar pinned open
                // (by a click). Shared across delegates so only one can be
                // pinned at a time; keyed by the SDK item id (or event id).
                property string pinnedActionsKey: ""
                property bool emojiPickerOpen: false
                // v0.5.11: whether the open room is encrypted — drives the
                // link-preview privacy gate in each MessageDelegate.
                property bool roomEncrypted: root.currentRoom.encrypted === true
                // Bubbles layout applies to direct messages only.
                property bool isDirectRoom: root.currentRoom.isDirect === true
                property var expandedStateGroups: ({})
                function saveRoomPosition() {
                    if (app.currentRoomId === "") return
                    if (stickToBottom) {
                        app.pagination.saveFollowingLatest(app.currentRoomId)
                        return
                    }
                    var row = viewRowAtPhysicalTop()
                    if (row < 0) return
                    var eventId = eventIdAtViewRow(row)
                    for (var probe = row; eventId === "" && probe >= 0; --probe)
                        eventId = eventIdAtViewRow(probe)
                    if (eventId === "") return
                    var item = itemAtViewRow(viewRowForStableId(eventId))
                    app.pagination.saveScrollAnchor(
                                app.currentRoomId, eventId,
                                item ? contentY
                                       - anchorPositionForItem(item) : 0,
                                false)
                }
                function stateGroupExpansionKey(groupId) {
                    return (app.currentRoomId || "") + "\u001f" + groupId
                }
                function stateGroupExpanded(groupId) {
                    return expandedStateGroups[stateGroupExpansionKey(groupId)] === true
                }
                function toggleStateGroup(groupId) {
                    if (!groupId || groupId.length === 0)
                        return
                    var key = stateGroupExpansionKey(groupId)
                    var next = Object.assign({}, expandedStateGroups)
                    next[key] = !stateGroupExpanded(groupId)
                    expandedStateGroups = next
                }

                // v0.5.9: delegate entry points into the media UI. Kept on
                // the view so MessageDelegate needs no external ids.
                property var openImage: function(mediaKey, httpUrl) {
                    imageViewer.openFor(mediaKey || "", httpUrl)
                }
                // v0.7: shared reaction picker / sender profile entry points
                // (one instance per timeline; the event id is captured at
                // open so delegate reuse can never redirect the action).
                property var openReactionPicker: function(eventId, point) {
                    if (!eventId || eventId.length === 0)
                        return
                    sharedReactionPicker.targetEventId = eventId
                    sharedReactionPicker.anchorPoint = point
                    sharedReactionPicker.open()
                }
                property var openSenderProfile: function(member) {
                    senderProfilePopover.openFor(member)
                }

                property var beginReplyForEvent: function(eventId) {
                    if (!eventId || eventId.length === 0) return
                    var details = timelineModel.messageDetails(eventId)
                    if (!details.eventId) return
                    var previewText = timelineModel.visibleTextForEvent(eventId)
                    app.composer.beginReply(eventId,
                        details.senderName || details.senderId,
                        (previewText || "").substring(0, 80))
                }
                property var openThreadForEvent: function(eventId) {
                    if (!eventId || eventId.length === 0) return
                    var details = timelineModel.messageDetails(eventId)
                    var rootId = (details.threadRootId || "").length > 0
                                 ? details.threadRootId : eventId
                    app.thread.openThread(app.currentRoomId, rootId)
                }
                property var saveMedia: function(mediaKey, filename) {
                    if (!mediaKey || mediaKey.length === 0) return
                    saveMediaDialog.pendingMediaKey = mediaKey
                    saveMediaDialog.currentFile = "file:///" + (filename || "download")
                    saveMediaDialog.open()
                }
                // Per-card save feedback: which media key is being written
                // (indeterminate — MediaBridge saves atomically, no progress
                // API), and the last finished key for a brief success/error
                // flash on its card. Keyed state, so an unrelated card can
                // never show another download's outcome.
                property string saveInFlightKey: ""
                property string lastSavedKey: ""
                property bool lastSaveOk: true
                Timer {
                    id: savedFlashTimer
                    interval: 4000
                    onTriggered: timeline.lastSavedKey = ""
                }
                function noteSaveStarted(mediaKey) {
                    saveInFlightKey = mediaKey
                    lastSavedKey = ""
                }
                function noteSaveFinished(ok, mediaKey) {
                    // Keyed by the bridge's completion: a viewer save or an
                    // overlapping second save can never flash the wrong
                    // card's outcome.
                    if (!mediaKey || mediaKey === "")
                        return
                    if (saveInFlightKey === mediaKey)
                        saveInFlightKey = ""
                    lastSavedKey = mediaKey
                    lastSaveOk = ok
                    savedFlashTimer.restart()
                }

                // Scroll to the newest row *after* the model/view have finished
                // reconciling. Positioning synchronously inside
                // onCountChanged during a reset (e.g. switching from a 10-row
                // room to a 2-row snapshot) made the backing DelegateModel try
                // to cancel a delegate at a now-out-of-range index
                // ("DelegateModel::cancel: index out range 10 2"). Qt.callLater
                // coalesces repeated requests into a single deferred call and
                // re-checks state at fire time, so it never runs mid-reset.
                function scrollToEndDeferred() {
                    // Never fight an in-flight wheel motion: the motion owns
                    // contentY and its own settle pass recomputes follow-
                    // latest (a deferred re-pin interleaving with a fresh
                    // keyboard/wheel motion could settle it instantly).
                    if (count > 0 && stickToBottom && !wheelAnimating)
                        positionViewAtLatest()
                }

                // TimelineScrollController supplies the configured mouse-
                // wheel speed and keyboard paging. The reversed model/layout
                // below removes the unstable prepend-before-visible-rows
                // operation that previously moved its coordinate frame.
                property bool wheelAnimating: app.timelineScroll.motionActive

                // ── Scroll-session state ─────────────────────────────────
                // True while the user's input owns the viewport: a native
                // drag/flick (moving/dragging), wheel/keyboard animation
                // (wheelAnimating), or direct touchpad deltas (the settle
                // timer). While this is true, Lightning must not write an
                // anchor correction into the input-owned position.
                readonly property bool userScrollActive:
                    moving || wheelAnimating || scrollSettleTimer.running

                // True only while Lightning itself drives contentY. The
                // distinction is diagnostic only: NO active input path
                // receives an anchor write.
                readonly property bool selfDrivenScrollActive:
                    !moving && (wheelAnimating || scrollSettleTimer.running)

                // ── Bounded per-gesture scroll diagnostics ───────────────
                // Off unless LIGHTNING_SCROLL_TRACE is set (read once in
                // TimelineScrollController). When on, ONE summary line is
                // emitted per wheel/touchpad gesture at settle — never one per
                // event — so a physical tester can capture a real trace and
                // send it back. See the counter block below for what
                // anchorCorrections and growthCorrections each mean across
                // maintainViewAnchor()'s five distinct outcomes (no-anchor
                // return, displaced, fallback capture, drag deferral,
                // materialized delta / idle restore).
                // v0.7.2: a separate paginationRestores counter used to
                // live here for the pagination anchor's own restore path. It
                // is gone because that whole mechanism is gone — a prepend
                // landing mid-gesture is now counted by activeDeferrals and
                // absorbed into a measurement-only re-capture.
                // Its disappearance IS the evidence the two mechanisms
                // actually merged rather than one being renamed.
                // No message content, ids, or URLs are logged.
                readonly property bool scrollTrace: app.timelineScroll.scrollTraceEnabled
                property bool diagActive: false
                property int diagEvents: 0
                property int diagPixelEvents: 0
                property int diagAngleEvents: 0
                // diagAnchorCorrections counts ONLY the idle absolute-restore
                // path in maintainViewAnchor(). Under the carry-bucket
                // semantics (counters reset at print, not per gesture) it is
                // EXPECTED to be non-zero on ordinary traces: the idle path
                // runs between gestures — media hydration or late decryption
                // while the reader is parked mid-history — and those counts
                // carry into the next printed line. It can NOT run while a
                // gesture owns the view (the userScrollActive block returns
                // first), so it is no longer a fought-the-input red flag;
                // read it as "idle restores since the previous line".
                // diagGrowthCorrections counts the displaced-anchor fallback
                // that actually wrote contentY after input was already idle.
                // Active geometry deltas are instead recorded by the
                // activeDeferred* fields below and never applied.
                property int diagAnchorCorrections: 0
                property int diagGrowthCorrections: 0
                // Rows instantiated in the Column. There is no height cache
                // to count commits against any more: every row's height IS its
                // measured height, so the old heightCommits/heightCached/
                // heightCorr triple has nothing left to report.
                property int diagRowCount: 0
                // v0.7.x live report round 2 ("teleporty ... when there is
                // images ... scrolling up"): a proposed fix to the displaced-
                // anchor branch (bounding/symmetrizing its correction) was
                // reviewed and WITHDRAWN — the reviewer showed the premise
                // could not be confirmed from the existing single combined
                // diagGrowthCorrections counter, which cannot tell which of
                // the FIVE distinct outcomes in maintainViewAnchor() actually
                // ran — displaced, the capture-fallback, drag-deferral,
                // materialized, or the idle absolute restore — nor whether
                // the magnitude of any one correction was proportionate to
                // real inserted content. These per-outcome counters exist
                // ONLY to answer that empirically, from a real physical
                // gesture, before any behavior changes again. A second review
                // pass then found the counters below still had a structural
                // blind spot for exactly the scenario this exists to
                // diagnose, plus two smaller gaps; both are fixed here:
                //
                //   M1 (blind spot): diagFlushGesture() used to run as the
                //   LAST statement of scrollSettleTimer.onTriggered, i.e.
                //   while diagActive was still true — but a correction that
                //   lands async relative to settle (originally: the
                //   near-top backfill staging window's release-time flush,
                //   since removed outright — see the near-top backfill
                //   comment further down; the reasoning below is kept
                //   because the SAME async-loss risk still applies to any
                //   other post-settle correction, e.g. a media hydration or
                //   late-decryption height change landing a turn or two
                //   after settle) does NOT happen inside that handler, so a
                //   naive "print on the last statement of settle" misses it
                //   entirely: the correction fires after the line was
                //   already printed and diagActive already false. Instead,
                //   every outcome counter below (everything except the
                //   events/pixel/angle/netY/dContentH group, which describes
                //   THIS gesture's own physical input and has no async-loss
                //   risk) is now gated on `scrollTrace` alone, not
                //   `diagActive`, and is drained (printed, then zeroed) only
                //   at the point it is actually printed, not at the next
                //   gesture's first input. A correction landing in the gap
                //   between one flush and the next gesture's first delta is
                //   therefore carried forward and appears on the NEXT
                //   printed line rather than being silently dropped — never
                //   lost, at the cost of coarser attribution: a line's
                //   outcome counts are "since the previous line", which can
                //   include the tail of the gesture before it. For the
                //   maintainer's actual capture procedure (continuous
                //   scrolling) the gap is short and the attribution stays
                //   meaningful; a long pause between two unrelated gestures
                //   could carry a stale correction into an unrelated line.
                //   Read the counters as "what happened since the last
                //   line", not "what this gesture caused".
                //
                //   M2 (uninterpretable all-zero line): a line where every
                //   outcome counter is 0 does not distinguish "the mechanism
                //   ran and had nothing to correct" from "the mechanism
                //   never ran at all" (viewAnchorId empty, or stickToBottom).
                //   diagNoAnchorReturns and diagStickToBottomReturns below
                //   count those two causes separately (see the call site).
                //   READ THEM CAREFULLY: maintainViewAnchor() is never
                //   SCHEDULED while stickToBottom (both content/height
                //   handlers take the scrollToEndDeferred arm instead), so
                //   an ordinary bottom-pinned gesture moves NEITHER counter.
                //   A non-zero diagStickToBottomReturns means specifically
                //   that a correction was scheduled while scrolled up and
                //   then dropped because the reader returned to the bottom
                //   before the coalesced call fired — harmless in
                //   production (scrollToEndDeferred pins the view anyway),
                //   but it is a real signal, not routine noise. When BOTH
                //   conditions hold the empty-anchor arm wins, so an empty
                //   id while bottom-pinned reports as noAnchorReturns.
                //
                //   L1 (conflated fallback causes): the old single
                //   captureFallbacks counter merged two different causes of
                //   "the delegate isn't there" — a stable id that no longer
                //   resolves to any row at all (redaction, local-echo id
                //   swap: diagUnresolvedIdFallbacks) and a row that still
                //   resolves fine but whose delegate was simply evicted from
                //   the cache with NO proof any row was inserted above it
                //   (diagEvictedNoInsertFallbacks — expected to DOMINATE in
                //   a media-heavy room, since tall image rows push the
                //   cache window around on ordinary scrolling with no
                //   pagination involved at all). Distinguishing them is
                //   exactly what a real capture needs to tell "an insertion
                //   happened but couldn't be resolved" apart from "nothing
                //   was inserted, this is just eviction".
                //
                // displacedMaxAbsGrew and materializedMaxAbsDelta store the
                // SIGNED value at the sample with the largest ABSOLUTE
                // magnitude (selection by |x|, storage without abs()) — a
                // large negative outlier is not lost to Math.abs(), and the
                // printed sign tells you which direction the outlier swung.
                //
                // Numbers and a fixed branch label only — no message content,
                // ids, or URLs.
                property int diagNoAnchorReturns: 0
                property int diagStickToBottomReturns: 0
                property int diagDisplacedFirings: 0
                property real diagDisplacedAppliedSum: 0
                property real diagDisplacedMaxAbsGrew: 0
                property int diagDisplacedMaxAbsGrewRows: 0
                // Paired samples, deliberately selected in BOTH directions:
                // the origin shift on the largest-|grew| firing answers the
                // known skipped-negative case, while the content delta and
                // rows on the largest-|origin shift| firing reveal whether
                // originY has outliers of its own. Independent maxima without
                // these pairings would conflate two unrelated firings.
                property real diagDisplacedMaxAbsGrewOriginShift: 0
                property real diagDisplacedMaxAbsOriginShift: 0
                property real diagDisplacedMaxAbsOriginShiftContentDelta: 0
                property int diagDisplacedMaxAbsOriginShiftRows: 0
                property int diagMaterializedFirings: 0
                property real diagMaterializedMaxAbsDelta: 0
                property int diagUnresolvedIdFallbacks: 0
                property int diagEvictedNoInsertFallbacks: 0
                property int diagDragDeferrals: 0
                property int diagActiveDeferrals: 0
                property real diagActiveDeferredSum: 0
                property real diagActiveDeferredMaxAbs: 0
                property int diagPrependFirings: 0
                property real diagPrependOriginShiftSum: 0
                property real diagPrependMaxAbsOriginShift: 0
                property int diagPrependMaxAbsOriginShiftRows: 0
                property real diagPrependMaxAbsOriginShiftContentDelta: 0
                property string diagPrependMaxAbsOriginShiftPath: "none"
                property real diagStartY: 0
                property real diagStartHeight: 0
                property real diagStartOriginY: 0
                function diagNoteActiveDeferral(delta) {
                    if (!scrollTrace)
                        return
                    diagActiveDeferrals += 1
                    diagActiveDeferredSum += delta
                    if (Math.abs(delta)
                            > Math.abs(diagActiveDeferredMaxAbs))
                        diagActiveDeferredMaxAbs = delta
                }
                function diagNoteEvent(isPixel) {
                    if (!scrollTrace)
                        return
                    if (!diagActive) {
                        diagActive = true
                        diagEvents = 0
                        diagPixelEvents = 0
                        diagAngleEvents = 0
                        diagStartY = contentY
                        diagStartHeight = contentHeight
                        diagStartOriginY = originY
                        diagGestureStartMs = Date.now()
                        diagWorstNotchMs = 0
                    }
                    diagEvents += 1
                    if (isPixel)
                        diagPixelEvents += 1
                    else
                        diagAngleEvents += 1
                }
                // Wall clock actually spent handling ONE wheel event, called
                // by the handler around its own body. This is the number a
                // "scrolling locks up" report needs: row counts alone cannot
                // distinguish a timeline that is merely large from one whose
                // rows are individually expensive.
                property real diagGestureStartMs: 0
                property real diagWorstNotchMs: 0
                function diagStateRowCount() {
                    if (!app || !app.timeline
                        || typeof app.timeline.stateActivityRowCount !== "function")
                        return -1
                    return app.timeline.stateActivityRowCount()
                }
                function diagStateGroupCount() {
                    if (!app || !app.timeline
                        || typeof app.timeline.stateGroupCount !== "function")
                        return -1
                    return app.timeline.stateGroupCount()
                }
                function diagNoteNotchCost(startMs) {
                    if (!scrollTrace || !diagActive)
                        return
                    var spent = Date.now() - startMs
                    if (spent > diagWorstNotchMs)
                        diagWorstNotchMs = spent
                }
                // Drains (prints then zeroes) every outcome counter — see the
                // M1 comment above for why this is the ONLY place they reset,
                // rather than at the next gesture's first input.
                function diagFlushGesture() {
                    if (!scrollTrace || !diagActive)
                        return
                    diagActive = false
                    console.info("scroll-gesture"
                        + " events=" + diagEvents
                        + " pixel=" + diagPixelEvents
                        + " angle=" + diagAngleEvents
                        + " netY=" + Math.round(contentY - diagStartY)
                        + " netOffset=" + Math.round(
                            (contentY - originY)
                            - (diagStartY - diagStartOriginY))
                        + " dContentH=" + Math.round(contentHeight - diagStartHeight)
                        + " originY=" + Math.round(originY)
                        + " dOriginY=" + Math.round(originY - diagStartOriginY)
                        + " noAnchorReturns=" + diagNoAnchorReturns
                        + " stickToBottomReturns=" + diagStickToBottomReturns
                        + " anchorCorrections=" + diagAnchorCorrections
                        + " growthCorrections=" + diagGrowthCorrections
                        + " displacedFirings=" + diagDisplacedFirings
                        + " displacedApplied=" + Math.round(diagDisplacedAppliedSum)
                        + " displacedMaxAbsGrew=" + Math.round(diagDisplacedMaxAbsGrew)
                        + " displacedMaxAbsGrewRows=" + diagDisplacedMaxAbsGrewRows
                        + " displacedMaxAbsGrewOriginShift=" + Math.round(diagDisplacedMaxAbsGrewOriginShift)
                        + " displacedMaxAbsOriginShift=" + Math.round(diagDisplacedMaxAbsOriginShift)
                        + " displacedMaxAbsOriginShiftDContentH=" + Math.round(diagDisplacedMaxAbsOriginShiftContentDelta)
                        + " displacedMaxAbsOriginShiftRows=" + diagDisplacedMaxAbsOriginShiftRows
                        + " materializedFirings=" + diagMaterializedFirings
                        + " materializedMaxAbsDelta=" + Math.round(diagMaterializedMaxAbsDelta)
                        + " unresolvedId=" + diagUnresolvedIdFallbacks
                        + " evictedNoInsert=" + diagEvictedNoInsertFallbacks
                        + " dragDeferrals=" + diagDragDeferrals
                        + " activeDeferrals=" + diagActiveDeferrals
                        + " activeDeferredSum=" + Math.round(diagActiveDeferredSum)
                        + " activeDeferredMaxAbs=" + Math.round(diagActiveDeferredMaxAbs)
                        + " prependFirings=" + diagPrependFirings
                        + " prependOriginShift=" + Math.round(diagPrependOriginShiftSum)
                        + " prependMaxAbsOriginShift=" + Math.round(diagPrependMaxAbsOriginShift)
                        + " prependMaxAbsOriginShiftRows=" + diagPrependMaxAbsOriginShiftRows
                        + " prependMaxAbsOriginShiftDContentH=" + Math.round(diagPrependMaxAbsOriginShiftContentDelta)
                        + " prependMaxAbsOriginShiftPath=" + diagPrependMaxAbsOriginShiftPath
                        + " rows=" + count
                        // Cost, and what the loaded timeline is MADE of.
                        // gestureMs is wall clock across the whole gesture;
                        // worstNotchMs is the slowest single wheel event,
                        // which is what the user actually feels as a stall.
                        // stateRows/stateGroups say whether a large row count
                        // is mostly collapsed room activity — a run of 1000
                        // state rows drawing three summary lines is a very
                        // different defect from 1000 real messages.
                        + " gestureMs=" + Math.round(Date.now() - diagGestureStartMs)
                        + " worstNotchMs=" + Math.round(diagWorstNotchMs)
                        // -1 means "not available here", never zero: a
                        // fixture without a real TimelineModel must not be
                        // reported as a timeline containing no state rows.
                        // Guarded rather than assumed — throwing inside this
                        // string would abort the whole line, which is exactly
                        // the regression scrollTraceLineIncludesAllPerBranchFields
                        // exists to catch.
                        + " stateRows=" + diagStateRowCount()
                        + " stateGroups=" + diagStateGroupCount()
                        + " contentH=" + Math.round(contentHeight)
                        + " stick=" + (stickToBottom ? 1 : 0)
                        + " topDist=" + Math.round(distanceFromTop())
                        + " nearTop=" + (distanceFromTop() <= nearTopEnterDistance ? 1 : 0))
                    diagNoAnchorReturns = 0
                    diagStickToBottomReturns = 0
                    diagAnchorCorrections = 0
                    diagGrowthCorrections = 0
                    diagDisplacedFirings = 0
                    diagDisplacedAppliedSum = 0
                    diagDisplacedMaxAbsGrew = 0
                    diagDisplacedMaxAbsGrewRows = 0
                    diagDisplacedMaxAbsGrewOriginShift = 0
                    diagDisplacedMaxAbsOriginShift = 0
                    diagDisplacedMaxAbsOriginShiftContentDelta = 0
                    diagDisplacedMaxAbsOriginShiftRows = 0
                    diagMaterializedFirings = 0
                    diagMaterializedMaxAbsDelta = 0
                    diagUnresolvedIdFallbacks = 0
                    diagEvictedNoInsertFallbacks = 0
                    diagDragDeferrals = 0
                    diagActiveDeferrals = 0
                    diagActiveDeferredSum = 0
                    diagActiveDeferredMaxAbs = 0
                    diagPrependFirings = 0
                    diagPrependOriginShiftSum = 0
                    diagPrependMaxAbsOriginShift = 0
                    diagPrependMaxAbsOriginShiftRows = 0
                    diagPrependMaxAbsOriginShiftContentDelta = 0
                    diagPrependMaxAbsOriginShiftPath = "none"
                }

                // Valid contentY range for this ListView, accounting for the
                // scroll margins, the pagination header, and a prepend-shifted
                // origin. Clamping here keeps fast wheel input from
                // overshooting or jittering against the ends.
                function wheelMinY() { return originY - topMargin }
                function wheelMaxY() {
                    var maxY = originY + contentHeight + bottomMargin - height
                    var minY = wheelMinY()
                    return maxY < minY ? minY : maxY
                }
                // Recompute follow-latest and near-top pagination the way the
                // drag/flick path does. Needed because wheel/pixel motion sets
                // contentY programmatically, so Flickable.moving stays false.
                function updateStickAndPaginate() {
                    stickToBottom = atBottomEdge()
                    // Active user scroll (wheel/pixel/keyboard): edge-latched so
                    // reaching the top re-arms the bounded backfill exactly once
                    // per approach, not on every settle.
                    checkNearTopEdge(true)
                }

                // Coalesce near-top backfill onto the next event-loop turn, the
                // same way maybeFillViewport() does. contentY / atYBeginning fire
                // every animation frame while scrolling and every time the
                // pagination header toggles its height, so dispatching directly
                // produced a burst of near_top requests (single-flighted, but
                // one fresh request per completed empty page). One coalesced
                // dispatch per turn collapses that churn; each call site keeps
                // its own trigger condition, and the controller still
                // single-flights the dispatch and bounds consecutive empty
                // (filtered-only) pages.
                property bool nearTopCheckScheduled: false
                property bool nearTopCheckUserInitiated: false
                function maybeRequestNearTop(userInitiated) {
                    // Establish the anchor at REQUEST time if the reader does
                    // not have one yet. captureViewAnchor() otherwise only
                    // runs at gesture settle, so a reader who scrolls
                    // continuously from the bottom to the top has no anchor
                    // for the whole gesture — every prepend during it would
                    // find viewAnchorId empty, compensate nothing, and the
                    // settle-time capture 250ms later would lock in whatever
                    // position the jump left. One capture per dispatched
                    // request, never per delta, so this does not reintroduce
                    // the touchpad hot-path scan.
                    if (!stickToBottom && viewAnchorId === "")
                        captureViewAnchor()
                    if (userInitiated)
                        nearTopCheckUserInitiated = true
                    if (nearTopCheckScheduled)
                        return
                    nearTopCheckScheduled = true
                    Qt.callLater(function() {
                        nearTopCheckScheduled = false
                        var ui = nearTopCheckUserInitiated
                        nearTopCheckUserInitiated = false
                        if (app.currentRoomId !== "")
                            app.pagination.requestNearTop(ui)
                    })
                }

                // v0.6.4: near-top pagination is EDGE-triggered with
                // hysteresis, not level-triggered. While the reader sits near
                // the top, updateStickAndPaginate / onContentYChanged / settle
                // fire continuously; the old level test (`contentY < height*0.5`)
                // re-sent a userInitiated near-top request on every one, which
                // reset the controller's zero-progress strike bound and let a
                // run of filtered (thread-only) history spin as a request loop.
                // Latch on region ENTRY and only re-arm after the reader leaves
                // a WIDER exit band, so one deliberate approach to the top = at
                // most one user request; the controller owns the bounded
                // continuation through filtered pages. A successful visible
                // prepend pushes the reader well below the exit band (older rows
                // now sit above), which naturally re-arms for the next approach.
                // DISTANCES from the earliest loaded row, not contentY
                // thresholds — the ...Distance names are deliberate. The
                // previous ...Y names invited exactly the frame confusion
                // described below, which is the whole of this defect.
                // Keep multiple viewports of loaded runway ahead of an
                // aggressive upward wheel gesture. Waiting until half a
                // viewport remained guaranteed that a fast mouse reached the
                // old hard bound before one 100ms SDK poll could land. Earlier
                // prefetch keeps position ownership with the user's live input;
                // unlike replaying overscroll after a load, it never skips a
                // page the reader has not seen.
                readonly property real nearTopEnterDistance: height * 2.5
                readonly property real nearTopExitDistance: height * 3.25
                property bool nearTopArmed: true
                // How far the viewport top sits BELOW the earliest loaded row.
                // The proximity bands MUST be measured against this and never
                // against raw contentY, because contentY is not a distance from
                // anything: it is an offset from originY, and originY is an
                // arbitrary value that MOVES as history loads and as ListView
                // re-estimates the rows it has not built.
                //
                // MEASURED, in the offscreen fixture: originY sat at ~+2484
                // with the reader at the very TOP of loaded history, so
                // `contentY <= height/2` was permanently FALSE there — standing
                // exactly at the top did not register as near the top at all,
                // and the latch was never consumed.
                // INFERRED, from the user's live trace: it sat far enough the
                // other way that raw contentY stayed inside the band through all
                // of loaded history, so the enter test was permanently true and
                // the exit test unreachable. (The trace logged netY/dContentH,
                // not originY. The inference is that a gesture moving +1869 px
                // AWAY from the top still reported nearTop=1, and atYBeginning
                // only fires on a false->true edge, so the band itself must have
                // been true there. Treat it as a strong inference, not a
                // measurement.)
                //
                // Both are the same fact: `contentY <= height/2` was not a weak
                // proximity test, it was not a proximity test at all, and which
                // way it failed depended on where originY happened to sit. Live
                // it over-triggered, so the settle re-arm fired after EVERY
                // gesture in either direction and each re-arm reset the
                // controller's filtered-page bound to buy four more batches —
                // the reported "it keeps loading old messages each time I scroll
                // up ... and down", and that loading storm is what the lag and
                // jitter were made of.
                function distanceFromTop() { return wheelMaxY() - contentY }
                // The CLOSEST distanceFromTop() reached during this visit to the
                // band — a ratchet, not "the distance at the last dispatch".
                // The difference is the whole guarantee: with only the dispatch
                // distance recorded, an upward gesture that dispatched on band
                // ENTRY (say 225) and then carried on up to 40 left everything
                // between 40 and 225 unpaid, so the next DOWNWARD gesture's
                // first sample at 45 satisfied `45 < 224` and fetched. That is
                // the ordinary "scroll up to near the top, then scroll down a
                // little" sequence, i.e. the reported defect surviving its own
                // fix. Ratcheting to the closest approach makes the invariant
                // real: only motion strictly closer to the top than the reader
                // has already been can fetch.
                // Reset to Infinity on leaving via the exit band or on returning
                // to the bottom, because a landed page moves the top and a fresh
                // approach is owed a fresh baseline.
                property real nearTopRequestDistance: Infinity
                function checkNearTopEdge(userInitiated) {
                    if (stickToBottom) {
                        nearTopArmed = true
                        nearTopRequestDistance = Infinity
                        return
                    }
                    var fromTop = distanceFromTop()
                    if (fromTop <= nearTopEnterDistance) {
                        // The progress gate lives HERE, at the dispatch, not on
                        // the gesture-settle re-arm below. Guarding only the
                        // re-arm left the reported symptom reachable: an upward
                        // gesture re-armed the latch, and the next DOWNWARD
                        // gesture then consumed it and fetched a page. Consuming
                        // the latch requires either being pinned against the top
                        // (nothing further up to scroll to — the reader cannot
                        // express the intent any more strongly, and this is the
                        // one place they most want history) or having come
                        // strictly closer to the top than the reader has ALREADY
                        // BEEN this visit. Because the baseline below is a
                        // ratchet, a downward sample can satisfy neither.
                        if (nearTopArmed
                            && (fromTop <= 1
                                || fromTop < nearTopRequestDistance - 1)) {
                            nearTopArmed = false
                            maybeRequestNearTop(userInitiated)
                        }
                        // Ratchet AFTER the gate has read the old value, and on
                        // every in-band sample — including the ones that did not
                        // dispatch because the latch was already consumed. Those
                        // are exactly the samples whose omission left the region
                        // between the top and the last dispatch unpaid.
                        if (fromTop < nearTopRequestDistance)
                            nearTopRequestDistance = fromTop
                    } else if (fromTop >= nearTopExitDistance) {
                        nearTopArmed = true
                        nearTopRequestDistance = Infinity
                    }
                }

                function cancelWheelMotion() {
                    app.timelineScroll.cancel()
                }

                function beginWheelTo(targetY) {
                    // Clamp defensively so keyboard callers (which pass an
                    // unclamped target) and any rounding can never drive an
                    // out-of-range or jittering contentY.
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    targetY = targetY < lo ? lo : (targetY > hi ? hi : targetY)
                    app.timelineScroll.animateTo(targetY, contentY, lo, hi,
                                                 height)
                    updateStickAndPaginate()
                    // Any upward intent leaves follow-latest — applied last so
                    // the geometry recompute above (still on the pre-motion
                    // position) cannot re-enable it while scrolling up.
                    if (targetY > contentY + 0.5)
                        stickToBottom = false
                    scrollSettleTimer.restart()
                }

                // ── v0.5.19: keyboard timeline navigation ────────────────
                // Distances are viewport-relative and independent of the
                // mouse-wheel speed setting; motion reuses the single
                // coalescing animation so repeated key presses never queue.
                // These fire only from the ListView's own Keys handler, i.e.
                // only while the timeline holds active focus — so a focused
                // composer, search field, dialog, or menu keeps its keys.
                function keyboardPage(direction) {   // -1 up, +1 down
                    // The table is rotated: increasing logical contentY moves
                    // physically upward toward older rows.
                    beginWheelTo(contentY - direction * height * 0.9)
                }
                // Home is programmatic navigation like End: it bypasses the
                // wheel motion engine and jumps directly, then recomputes
                // pagination / follow-latest and saves one settled anchor.
                function goToEarliestLoaded() {
                    cancelWheelMotion()
                    contentY = wheelMaxY()
                    updateStickAndPaginate()
                    scrollSettleTimer.restart()
                }
                function goToLatest() {
                    cancelWheelMotion()
                    stickToBottom = true
                    // positioning row zero below can re-seed the position
                    // frame from an estimate; a surviving anchor baseline
                    // would then measure a delta across two different frames.
                    viewAnchorId = ""
                    viewAnchorLastY = 0
                    app.pagination.saveFollowingLatest(app.currentRoomId)
                    positionViewAtLatest()
                    Qt.callLater(function() {
                        positionViewAtLatest()
                        app.readReceipts.reevaluate()
                    })
                }
                activeFocusOnTab: true
                Keys.onPressed: (event) => {
                    switch (event.key) {
                    case Qt.Key_PageUp:
                        keyboardPage(-1); event.accepted = true; break
                    case Qt.Key_PageDown:
                        keyboardPage(1); event.accepted = true; break
                    case Qt.Key_Home:
                        goToEarliestLoaded(); event.accepted = true; break
                    case Qt.Key_End:
                        goToLatest(); event.accepted = true; break
                    case Qt.Key_Space:
                        // Shift+Space pages up, Space pages down. Only reachable
                        // when the timeline (not a text input) owns the key.
                        keyboardPage((event.modifiers & Qt.ShiftModifier) ? -1 : 1)
                        event.accepted = true; break
                    default:
                        event.accepted = false
                    }
                }
                // Clicking the timeline surface gives it keyboard focus for the
                // navigation keys above, without stealing focus while typing.
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: timeline.forceActiveFocus()
                }

                // TimelineScrollController drives mouse-wheel and keyboard
                // motion. QML applies each frame against the live bounds.
                Connections {
                    target: app.timelineScroll
                    function onWheelPositionChanged(y) {
                        var lo = timeline.wheelMinY()
                        var hi = timeline.wheelMaxY()
                        var clamped = y < lo ? lo : (y > hi ? hi : y)
                        timeline.contentY = clamped
                        if (clamped !== y)
                            app.timelineScroll.notifyBoundReached(clamped)
                    }
                    function onWheelMotionSettled() {
                        timeline.updateStickAndPaginate()
                        // Refresh the view anchor the moment wheel motion ends,
                        // closing the 250ms stale-anchor window on the mouse
                        // path so a late delegate-height change cannot re-align
                        // contentY to a position the reader has already left.
                        timeline.captureViewAnchor()
                        scrollSettleTimer.restart()
                    }
                }

                // Save the settled position once input stops, rather than
                // hundreds of intermediate anchors mid-scroll.
                Timer {
                    id: scrollSettleTimer
                    objectName: "scrollSettleTimer"
                    interval: 250
                    onTriggered: {
                        timeline.updateStickAndPaginate()
                        timeline.saveRoomPosition()
                        timeline.captureViewAnchor()
                        // A COMPLETED gesture that left the reader near the
                        // top re-arms the edge. Without this, backfill stops
                        // dead in a filtered/thread-heavy room: the reader's
                        // first approach consumes the latch, the controller
                        // spends its 4-page strike budget on empty pages and
                        // latches too, and every further upward scroll is
                        // ignored — the edge is only re-armed by scrolling
                        // back DOWN past the exit band. Re-arming per
                        // completed gesture keeps the bound the hysteresis
                        // exists for (one request per deliberate gesture,
                        // never a per-frame spin) while letting "keep
                        // scrolling up" keep meaning "keep loading".
                        // Re-arming here is unconditional within the band on
                        // purpose: checkNearTopEdge() owns the progress gate, so
                        // an armed latch a downward gesture cannot consume is
                        // harmless. Putting the direction test HERE instead was
                        // wrong twice over — it left an upward-then-downward
                        // sequence able to fetch, and it permanently stranded a
                        // reader parked at the exact top, where contentY is at
                        // its minimum and can never decrease further.
                        if (!timeline.stickToBottom
                            && !app.pagination.reachedStart
                            && timeline.distanceFromTop()
                               <= timeline.nearTopEnterDistance)
                            timeline.nearTopArmed = true
                        // The single wheel-gesture settle point: emit one
                        // bounded diagnostic summary.
                        timeline.diagFlushGesture()
                    }
                }

                WheelHandler {
                    id: timelineWheelHandler
                    objectName: "timelineWheelHandler"
                    // This handler is the single pointer-wheel owner. The
                    // reversed proxy plus rotated table ensures loading older
                    // history extends the far edge instead of moving every
                    // visible row underneath that owner.
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (event) => {
                        // Timed only while LIGHTNING_SCROLL_TRACE is set;
                        // Date.now() is not called at all otherwise.
                        var notchStartMs = timeline.scrollTrace ? Date.now() : 0
                        var minY = timeline.wheelMinY()
                        var maxY = timeline.wheelMaxY()
                        if (event.pixelDelta.y !== 0) {
                            timeline.cancelWheelMotion()
                            timeline.contentY = app.timelineScroll.pixelTargetY(
                                -event.pixelDelta.y, timeline.contentY,
                                minY, maxY)
                            timeline.updateStickAndPaginate()
                            if (event.pixelDelta.y > 0)
                                timeline.stickToBottom = false
                            timeline.diagNoteEvent(true)
                            scrollSettleTimer.restart()
                        } else if (event.angleDelta.y !== 0) {
                            app.timelineScroll.wheelNotch(
                                -event.angleDelta.y, timeline.contentY,
                                minY, maxY, timeline.height)
                            timeline.updateStickAndPaginate()
                            if (event.angleDelta.y > 0)
                                timeline.stickToBottom = false
                            timeline.diagNoteEvent(false)
                            scrollSettleTimer.restart()
                        }
                        timeline.diagNoteNotchCost(notchStartMs)
                        event.accepted = true
                    }
                }

                Component.onDestruction: cancelWheelMotion()

                // v0.5.11: read state is decided by ReadReceiptCoordinator
                // in C++ (window focus, debounce, event eligibility,
                // duplicate suppression). QML only reports what it alone
                // knows: whether the timeline is on screen and whether the
                // user is following the bottom of the conversation.
                Binding {
                    target: app.readReceipts
                    property: "timelineVisible"
                    value: timeline.visible && timeline.presentationReady
                           && app.currentRoomId !== ""
                }
                Binding {
                    target: app.readReceipts
                    property: "nearBottom"
                    value: timeline.stickToBottom
                }
                // v0.6.0 checkpoint 11: active-room notification suppression
                // uses the same signals as read receipts: on screen, window
                // focused, following the latest message.
                Binding {
                    target: app
                    property: "activeRoomAtLatest"
                    value: timeline.visible && timeline.presentationReady
                           && timeline.Window.active === true
                           && timeline.stickToBottom
                }

                // v0.6.6: TimelineModel's near-top backfill "virtual
                // scrolling" staging window — which used to hold a landed
                // batch out of the exposed row space entirely while a
                // gesture was held through an unsettled near-top approach —
                // was removed outright. With the chain-continuation defect
                // fixed (finishBatch() now ends a near-top run at the FIRST
                // productive page instead of auto-chaining while staged —
                // see PaginationController::finishBatch()), staging no
                // longer had multiple batches to coalesce: it held at most
                // one bounded page, and for that single page it cost a real
                // wall — the loaded page sat invisible and contentY could
                // not advance until the gesture physically ended, the
                // opposite of the maintainer's actual ask ("when content
                // loads actually allow normal scrolling again"). A live
                // bottom-append landing during that window could also be
                // misclassified as the run's own progress, ending it having
                // delivered no history at all, with no way to ask again
                // while frozen. Every landed batch is now an ordinary
                // immediate prepend again — exactly the un-staged path this
                // file already used for ViewportFill/Retry — compensated by
                // the same maintainViewAnchor() mechanism below as soon as
                // it lands, never deferred to gesture-end.

                // v0.5.11: ask the pagination controller for one more batch
                // whenever the loaded content cannot fill the viewport (a
                // short initial snapshot never scrolls, so a scroll-position
                // trigger alone would deadlock). The controller enforces the
                // fill budget, no-progress stop and single-flight. Geometry
                // changes are coalesced onto the next event-loop turn: the
                // pagination header itself changes contentHeight when the
                // controller enters/leaves Loading, so dispatching directly
                // from onContentHeightChanged re-entered the header's state
                // binding and produced a paginationState binding loop.
                property bool viewportFillCheckScheduled: false
                function maybeFillViewport() {
                    if (viewportFillCheckScheduled)
                        return
                    viewportFillCheckScheduled = true
                    Qt.callLater(function() {
                        viewportFillCheckScheduled = false
                        if (app.currentRoomId !== "" && contentHeight < height)
                            app.pagination.requestViewportFill()
                    })
                }
                // Content height changes whenever any delegate's height
                // settles (text measurement, media hydration, link preview,
                // decryption replacement). One coalesced reaction per batch:
                // keep the newest event pinned while following the bottom,
                // otherwise hold the reader's anchor steady.
                onContentHeightChanged: {
                    scheduleVisibleRowRange()
                    maybeFillViewport()
                    recomputePresentationReady()
                    if (stickToBottom) {
                        if (!moving && !wheelAnimating && count > 0)
                            Qt.callLater(scrollToEndDeferred)
                    } else {
                        maintainViewAnchorCoalesced()
                    }
                }
                onHeightChanged: {
                    maybeFillViewport()
                    recomputePresentationReady()
                    // Viewport resizes (window, right panel, find bar) keep
                    // the same reading position: pinned stays pinned, an
                    // anchored reader keeps the anchored message.
                    if (stickToBottom) {
                        if (count > 0)
                            Qt.callLater(scrollToEndDeferred)
                    } else {
                        maintainViewAnchorCoalesced()
                    }
                }
                // NOTE: no onWidthChanged invalidation. Wrapped text heights
                // used to be cached per row and had to be thrown away when the
                // wrapping constraint changed. Rows now re-wrap and re-measure
                // themselves, and the Column re-lays out from the result.

                // v0.5.11 through v0.7: backward-pagination prepend
                // compensation used to be a SEPARATE capture/restore pair
                // here (anchorStableId/anchorOffset/anchorContentHeight/
                // anchorItemY/anchorCaptureToken, captureAnchor()/
                // restoreCapturedAnchor()/restoreAnchor(), plus a
                // willContinue keep-alive across a multi-batch run). v0.7.2
                // removed it: a prepend is, from the anchor's point of view,
                // exactly the same event as a pooled delegate resizing above
                // the reader — something changed the tracked row's own
                // content-space y — and the persistent view anchor above
                // (viewAnchorId/viewAnchorLastY, maintainViewAnchor())
                // already reacts to that via onContentHeightChanged, which a
                // prepend fires just as reliably as a resize does. Four
                // same-day rounds were entirely about these two mechanisms
                // coordinating with each other: sharing a run, standing one
                // down while the other owned it, a stale capture token, a
                // jump-then-unjump double reposition per batch. With one
                // mechanism left there is nothing to coordinate with — no
                // capture at request start, no willContinue bookkeeping to
                // keep a capture alive across a multi-batch filtered run (a
                // zero-insert batch fires no onContentHeightChanged, so
                // there is nothing to correct and nothing to keep alive),
                // and no stale-token guard (there is only ever one anchor,
                // continuously tracked). Do not reintroduce a
                // pagination-specific anchor here — see maintainViewAnchor().

                Connections {
                    target: app.pagination
                    function onPaginationCompleted(insertedCount, reachedStart,
                                                   willContinue) {
                        if (insertedCount <= 0 || reachedStart || willContinue)
                            return
                        // A productive page relocated the history edge. Start a
                        // fresh distance ratchet from that new edge so continued
                        // upward motion can prefetch the following page before
                        // exhausting the newly loaded runway. No position write.
                        timeline.nearTopArmed = true
                        timeline.nearTopRequestDistance =
                                timeline.distanceFromTop()
                    }
                    function onTargetLocated(row, pixelOffset, highlight) {
                        // Reply navigation takes control immediately.
                        timeline.cancelWheelMotion()
                        Qt.callLater(function() {
                            // The target can be older than what the proxy has
                            // paced out so far; without this the jump resolves
                            // to -1 and nothing happens at all.
                            timeline.releasePendingRows()
                            var viewRow = timeline.viewRowForSourceRow(row)
                            if (viewRow < 0)
                                return
                            timeline.cancelWheelMotion()
                            timeline.stickToBottom = false
                            timeline.positionViewAtViewRow(viewRow, highlight)
                            if (!highlight) {
                                var item = timeline.itemAtViewRow(viewRow)
                                if (item)
                                    timeline.contentY =
                                        timeline.anchorPositionForItem(item)
                                        + pixelOffset
                            }
                            timeline.saveRoomPosition()
                            timeline.captureViewAnchor()
                        })
                    }
                    function onRestoreLatestRequested() {
                        timeline.cancelWheelMotion()
                        timeline.stickToBottom = true
                        // Same frame-rebase reason as goToLatest().
                        timeline.viewAnchorId = ""
                        timeline.viewAnchorLastY = 0
                        Qt.callLater(timeline.scrollToEndDeferred)
                    }
                }
                Connections {
                    target: app.settings
                    function onShowRoomActivityChanged() {
                        if (timeline.stickToBottom) {
                            Qt.callLater(timeline.scrollToEndDeferred)
                            return
                        }
                        // v0.7.2: this used to run the now-removed
                        // pagination-specific capture/restore pair — a THIRD
                        // call site of that duplicated mechanism, for an
                        // unrelated cause (a settings toggle, not a fetch).
                        // Capture the surviving anchor explicitly right
                        // before the reflow and correct right after, so the
                        // correction lands on the same deferred turn as
                        // before rather than waiting for a separately
                        // coalesced onContentHeightChanged.
                        timeline.captureViewAnchor()
                        Qt.callLater(function() {
                            timeline.maintainViewAnchor()
                            timeline.maybeFillViewport()
                        })
                    }
                }

                onContentYChanged: {
                    // Unconditional: the activation range must track the
                    // viewport even for programmatic moves.
                    scheduleVisibleRowRange()
                    // React to native drag/flick/wheel movement and keyboard
                    // paging, but ignore unrelated programmatic navigation.
                    // A native wheel does not need to expose `moving`: the
                    // passive observer above keeps the settle timer active for
                    // the duration of its event stream.
                    if (!userScrollActive) return
                    stickToBottom = atBottomEdge()
                    // Trigger backfill as the user approaches the top
                    // (drag/flick/wheel), edge-latched with hysteresis so it
                    // fires once per approach rather than every frame.
                    checkNearTopEdge(true)
                }
                onCountChanged: {
                    scheduleVisibleRowRange()
                    // A new event arrived (or the timeline reset). Follow the
                    // bottom.
                    recomputePresentationReady()
                    if (stickToBottom) Qt.callLater(scrollToEndDeferred)
                }
                onMovementEnded: {
                    // Scrolling settled: recompute whether we are at the
                    // bottom (return-to-bottom must clear unread — the
                    // coordinator reacts to the nearBottom binding).
                    stickToBottom = atBottomEdge()
                    saveRoomPosition()
                    captureViewAnchor()
                }
                Component.onCompleted: {
                    Qt.callLater(scrollToEndDeferred)
                    maybeFillViewport()
                    // The pane may be (re)created while a room is already
                    // loaded; evaluate the gate against current state instead
                    // of waiting for the next model reset.
                    if (app.currentRoomId !== "" && count > 0)
                        presentationGuard.restart()
                    recomputePresentationReady()
                }
                // A room switch / fresh timeline snapshot opens at the bottom:
                // reset stickToBottom (it may have been left false after
                // scrolling up in the previous room).
                Connections {
                    target: app.timeline
                    function onModelReset() {
                        // A room switch / fresh snapshot must cancel any
                        // in-flight wheel motion from the previous room.
                        timeline.cancelWheelMotion()
                        timeline.stickToBottom = true
                        timeline.pinnedActionsKey = ""
                        timeline.emojiPickerOpen = false
                        timeline.viewAnchorId = ""
                        timeline.viewAnchorOffset = 0
                        timeline.viewAnchorLastY = 0
                        // Fresh room opens at the bottom: re-arm the near-top
                        // edge so the first genuine approach to the top of the
                        // new room triggers backfill.
                        timeline.nearTopArmed = true
                        timeline.nearTopRequestDistance = Infinity
                        timeline.expandedStateGroups = ({})
                        // Re-engage the presentation gate for the fresh
                        // snapshot. Recompute only after this whole signal
                        // dispatch settles: the pagination controller's own
                        // reset slot may run after this handler, and reading
                        // its previous room's settled state here could open
                        // the gate on a one-item partial snapshot. A warm
                        // cache that already fills the viewport re-opens the
                        // gate on the same deferred turn (sub-frame).
                        timeline.presentationReady = false
                        timeline.presentationResetPending = true
                        presentationGuard.restart()
                        Qt.callLater(function() {
                            app.pagination.restoreScrollAnchor(app.currentRoomId)
                        })
                        Qt.callLater(timeline.maybeFillViewport)
                        Qt.callLater(function() {
                            timeline.presentationResetPending = false
                            timeline.recomputePresentationReady()
                        })
                    }
                }

                // Pagination trigger: scroll to top with backfill available.
                // Duplicate and reached-start suppression live in the
                // controller.
                //
                // Deliberately NOT edge-latched through checkNearTopEdge()/
                // nearTopArmed the way the active drag/wheel path is: that
                // latch early-returns whole while stickToBottom is true (see
                // its own body), which would silently swallow the fresh-room
                // initial-history kick-off — a just-opened room starts
                // stickToBottom=true with too little content to scroll, and
                // this passive atYBeginning edge is the ONLY trigger for that
                // first fill. Reusing the active latch here would need to
                // special-case stickToBottom inside it, which risks the
                // active path's own precisely-tested hysteresis (many
                // existing tests pin nearTopArmed's exact enter/exit/re-arm
                // behavior) for a passive signal that already has its own,
                // different bound:
                //   * onAtYBeginningChanged only re-fires on an actual
                //     false->true transition of Qt's own atYBeginning
                //     property — never a per-frame poll — so a dispatch here
                //     always costs at least one real property-change event,
                //     not a tight loop;
                //   * userInitiated=false, so it can never RESET the
                //     controller's empty-strike counter (only a genuine user
                //     gesture does) — a run of filtered/empty pages this path
                //     alone triggers still latches at kMaxNearTopEmptyStrikes,
                //     identically to the active path;
                //   * PaginationController::request()'s single-flight makes a
                //     same-turn re-fire (e.g. two toggles inside one
                //     coalesced Qt.callLater window) a harmless "duplicate
                //     suppressed" no-op, logged and otherwise inert;
                //   * a PRODUCTIVE page (the remaining risk: a genuinely
                //     resettable-to-true atYBeginning caused by a ListView
                //     content-height ESTIMATE settling back to originY after
                //     a prepend, not real user motion) still ends the run
                //     immediately per finishBatch() — one page, then nothing
                //     further dispatches until atYBeginning genuinely toggles
                //     false and true again, each occurrence separately paced
                //     by a real network round trip. Not literally unbounded
                //     in the sense the withdrawn M5 staging chain was (no
                //     bound at all beyond exhausting the room); bounded by
                //     the union of the above, at the cost of a possible extra
                //     page or two if that estimate-churn scenario recurs —
                //     accepted rather than risking the active path's tested
                //     invariants for an unconfirmed, narrower edge case.
                onAtYEndChanged: {
                    if (atYEnd)
                        maybeRequestNearTop(false)
                }

                // v0.6.4: the transient loading / failure indicator is NOT a
                // ListView header. As list content its 0<->32 height toggle
                // changed contentHeight every time pagination entered/left
                // Loading, shoving the reader's viewport (uncompensated during
                // the busy window) and flipping atYBeginning into extra
                // near-top requests. It now lives as a top overlay (see
                // paginationHeader, a sibling of this ListView) so the loading
                // state never perturbs timeline geometry. The beginning of
                // history is still the virtual "Beginning of conversation" row
                // (eventType 9), so there is no lingering "scroll up"
                // placeholder.

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                Label {
                    anchors.centerIn: parent
                    // The no-room state is now the Home surface below; this
                    // label only covers an empty selected room.
                    visible: app.currentRoomId !== ""
                             && timeline.count === 0 && timeline.presentationReady
                    text: qsTr("No messages yet")
                    color: AppTheme.textMuted
                }
            }

            // v0.6.4: pagination loading / failure indicator — a TOP OVERLAY,
            // never ListView content, so entering or leaving Loading cannot
            // change the timeline's contentHeight or flip atYBeginning. Its
            // height still tracks the single semantic PaginationController
            // state (the contract test enforces "Hidden ? 0 : 32" and forbids
            // a re-entrant local mirror). Exposed as objectName
            // "paginationHeader" so TimelinePaneQmlTest.cpp can locate and
            // assert the presentation surface without a coordinate probe.
            Item {
                objectName: "paginationHeader"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                z: 15
                clip: true
                height: app.pagination.presentationState
                        === PaginationController.Hidden ? 0 : 32
                visible: app.currentRoomId !== ""
                         && app.pagination.presentationState
                            !== PaginationController.Hidden
                Rectangle {
                    anchors.centerIn: parent
                    height: 26
                    width: paginationPill.implicitWidth + 20
                    radius: 13
                    color: AppTheme.cardElevated
                    border.width: 1
                    border.color: AppTheme.border
                    Row {
                        id: paginationPill
                        anchors.centerIn: parent
                        spacing: 6
                        BusyIndicator {
                            width: 16; height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            running: app.pagination.presentationState
                                     === PaginationController.Loading
                            visible: running
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: app.pagination.presentationState
                                  === PaginationController.Loading
                                  ? qsTr("Loading older messages…")
                                  : qsTr("Could not load older messages —")
                            color: app.pagination.presentationState
                                   === PaginationController.Failed
                                   ? AppTheme.error : AppTheme.textMuted
                            font.pixelSize: 11
                        }
                        // v0.6.5: an inline text link — reads AppTheme.link
                        // (periwinkle under Storm), not accent (bolt,
                        // reserved for selection/focus/the one primary
                        // action), consistent with every other inline
                        // "Retry"/action link in the timeline.
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: app.pagination.presentationState
                                     === PaginationController.Failed
                            text: qsTr("Retry")
                            color: AppTheme.link
                            font.pixelSize: 11
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: app.pagination.retry()
                            }
                        }
                    }
                }
            }

            // v0.7.1: Home surface — replaces the bare "select a room"
            // placeholder when nothing is selected. Sits over the (empty,
            // hidden) timeline; the ListView stays present for tests and to
            // resume the selected room instantly.
            // A REAL selected Space gets its dedicated management surface;
            // the pseudo-spaces (All rooms / not-in-a-space) keep Home.
            readonly property bool spaceViewActive:
                app.currentRoomId === ""
                && app.spaces && app.spaces.activeSpaceId.length > 0
                && app.spaces.activeSpaceId.charAt(0) === "!"

            HomePane {
                objectName: "homePane"
                anchors.fill: parent
                visible: app.currentRoomId === "" && !parent.spaceViewActive
                onNewMessageRequested: root.newConversationRequested("dm")
                onCreateRoomRequested: root.newConversationRequested("room")
                onCreateSpaceRequested: root.newConversationRequested("space")
            }

            // v0.7: Space Home — never an ordinary room timeline/composer.
            Loader {
                objectName: "spaceHomeLoader"
                anchors.fill: parent
                active: parent.spaceViewActive
                visible: active
                sourceComponent: spaceHomeComponent
            }

            // v0.7: room-loading surface shown while the presentation gate
            // holds the timeline back. Deliberately calm: no partial rows,
            // no progressive rebuild, one quiet loading row.
            Item {
                objectName: "timelineLoadingSurface"
                anchors.fill: parent
                visible: !timeline.presentationReady
                Row {
                    anchors.centerIn: parent
                    spacing: AppTheme.spacingS
                    BusyIndicator {
                        width: 18; height: 18
                        anchors.verticalCenter: parent.verticalCenter
                        running: !timeline.presentationReady
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Loading conversation…")
                        color: AppTheme.textMuted
                        font.pixelSize: 12
                    }
                }
            }

            // Floating Lightning pill: a compact accent-fill chevron with an
            // optional new-message count. Stays an AbstractButton so click()
            // and visible drive the existing scroll tests.
            AbstractButton {
                id: jumpToLatestButton
                objectName: "jumpToLatestButton"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: AppTheme.spacingM + 12
                anchors.bottomMargin: AppTheme.spacingM + 8
                visible: app.currentRoomId !== "" && !timeline.stickToBottom
                z: 20
                focusPolicy: Qt.StrongFocus
                hoverEnabled: true
                implicitHeight: 34
                implicitWidth: jumpRow.implicitWidth + 24
                readonly property int newCount:
                    (root.currentRoom && root.currentRoom.unreadCount)
                        ? root.currentRoom.unreadCount : 0
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Jump to latest")
                ToolTip.text: qsTr("Return to the newest message")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                // Shares goToLatest() with the End key — both cancel any wheel
                // animation, resume follow-latest, and re-evaluate read state.
                onClicked: timeline.goToLatest()

                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: jumpToLatestButton.down ? AppTheme.accentPressed
                         : jumpToLatestButton.hovered ? AppTheme.accentHover
                         : AppTheme.accent
                }
                Row {
                    id: jumpRow
                    anchors.centerIn: parent
                    spacing: AppTheme.spacingXS
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: "expand_more"
                        size: 20
                        color: AppTheme.accentText
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: jumpToLatestButton.newCount > 0
                        text: jumpToLatestButton.newCount > 99
                              ? "99+" : jumpToLatestButton.newCount
                        color: AppTheme.accentText
                        font.family: AppTheme.uiFont
                        font.pixelSize: 12
                        font.weight: Font.ExtraBold
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    radius: (parent.height + 6) / 2
                    color: "transparent"
                    border.color: AppTheme.focusRing
                    border.width: 2
                    visible: jumpToLatestButton.visualFocus
                }
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: jumpToLatestButton.visible
                                ? jumpToLatestButton.top : parent.bottom
                anchors.bottomMargin: AppTheme.spacingS
                visible: app.pagination.navigationMessage.length > 0
                text: app.pagination.navigationMessage
                color: AppTheme.text
                padding: AppTheme.spacingS
                z: 21
                background: Rectangle {
                    color: AppTheme.surface
                    border.color: AppTheme.border
                    radius: AppTheme.radiusSm
                }
            }
        }

        // Typing indicator — a constant-height slot while a room is shown.
        // It used to be a conditional row, so every appearance/disappearance
        // resized the timeline viewport and the re-pin handler shifted the
        // whole message stack by the indicator's height (the reported
        // "jitters the chat up and down"). Only the label's opacity changes
        // now; the reserved strip never moves the timeline.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: app.currentRoomId !== ""
                ? Math.max(typingLabel.implicitHeight, typingMetrics.height) + 6
                : 0
            visible: app.currentRoomId !== ""
            color: AppTheme.background
            FontMetrics {
                id: typingMetrics
                font.italic: true
                font.pixelSize: 11
            }
            Label {
                id: typingLabel
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacingM
                anchors.right: parent.right
                anchors.rightMargin: AppTheme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                text: app.timeline.typingText
                opacity: text.length > 0 ? 1 : 0
                // The slot is always present; keep the EMPTY state out of
                // the accessibility tree.
                Accessible.ignored: text.length === 0
                elide: Text.ElideRight
                color: AppTheme.textMuted
                font.italic: true
                font.pixelSize: 11
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
        }

        // v0.5.9: Save As result feedback (auto-clears).
        //
        // v0.6.7: it reserves NO layout height. It used to be an ordinary
        // row, so every appearance pushed the whole timeline up and every
        // auto-clear dropped it back down — a visible jump on something as
        // routine as saving a GIF. The wrapper is a zero-height layout item
        // and the banner is anchored to its bottom, overflowing upward over
        // the timeline instead of displacing it.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 0
            z: 5
            Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: saveResult.text.length > 0
            height: saveResult.implicitHeight + 6
            color: AppTheme.background
            Label {
                id: saveResult
                property bool ok: true
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                color: ok ? AppTheme.success : AppTheme.danger
                font.pixelSize: 11
                Timer {
                    id: saveResultTimer
                    interval: 5000
                    onTriggered: saveResult.text = ""
                }
            }
            }
        }

        // The composer has no target when no room is selected — the Home
        // surface is shown instead. visible:false collapses its space.
        MessageComposerBar {
            id: messageComposer
            objectName: "messageComposer"
            Layout.fillWidth: true
            visible: app.currentRoomId !== ""
        }
    }

    // ── Thread side panel ────────────────────────────────────────────────
    Rectangle {
        visible: root.threadSurfaceOpen && root.width >= 660
        Layout.fillHeight: true
        implicitWidth: 1
        color: AppTheme.borderStrong
    }
    ThreadPanel {
        id: threadPanel
        objectName: "threadPanel"
        visible: root.threadSurfaceOpen
        Layout.fillHeight: true
        // Correction spec §4: the thread panel is exactly 340px wide.
        Layout.preferredWidth: root.width >= 660 ? 340 : root.width
        Layout.fillWidth: root.threadSurfaceOpen && root.width < 660
        onCloseRequested: {
            // Closing the thread collapses the right side completely: the
            // panel state becomes "none" — Room Information / People are
            // never restored implicitly.
            root.closeThreadSurface()
        }
        openImage: function(mediaKey, httpUrl) {
            imageViewer.openFor(mediaKey || "", httpUrl)
        }
        saveMedia: function(mediaKey, filename) {
            if (!mediaKey || mediaKey.length === 0) return
            saveMediaDialog.pendingMediaKey = mediaKey
            saveMediaDialog.currentFile = "file:///" + (filename || "download")
            saveMediaDialog.open()
        }
    }

    // ── Room Information side panel ──────────────────────────────────────
    Rectangle {
        visible: root.infoOpen
        Layout.fillHeight: true
        implicitWidth: 1
        color: AppTheme.border
    }
    RoomInfoPanel {
        id: infoPanel
        objectName: "roomInfoPanel"
        Layout.fillHeight: true
        Layout.preferredWidth: root.infoOpen ? 320 : 0
        // Collapse cleanly at narrow widths instead of crushing the chat.
        visible: root.infoOpen && root.width >= 700
        onCloseRequested: root.infoOpen = false
        onOpenImageRequested: (mediaKey, httpUrl) =>
            imageViewer.openFor(mediaKey, httpUrl)
        onSaveMediaRequested: (mediaKey, filename) => {
            saveMediaDialog.pendingMediaKey = mediaKey
            saveMediaDialog.currentFile = "file:///" + filename
            saveMediaDialog.open()
        }
        // v0.7.x pinned messages: a pin is very often outside the loaded
        // window, which is exactly what PaginationController::jumpToEvent
        // already handles for replies, permalinks and search hits. Reusing
        // it means pins hydrate through the ONE navigation path instead of
        // a second one that would have to re-learn anchoring.
        onJumpToEventRequested: (eventId) => {
            if (eventId !== "")
                app.pagination.jumpToEvent(eventId)
        }
    }

    // ── Room message search side panel ──────────────────────────────────
    Rectangle {
        visible: root.searchOpen && root.width >= 700
        Layout.fillHeight: true
        implicitWidth: 1
        color: AppTheme.border
    }
    SearchPanel {
        id: searchPanel
        objectName: "roomSearchPanel"
        visible: root.searchOpen
        Layout.fillHeight: true
        Layout.preferredWidth: root.width >= 700 ? 360 : root.width
        Layout.fillWidth: root.searchOpen && root.width < 700
        historyAvailable: root.findHistoryAvailable
        onCloseRequested: root.searchOpen = false
        onFindLoadedRequested: {
            root.searchOpen = false
            root.openFind()
        }
        onJumpToEventRequested: (eventId) => {
            if (eventId !== "")
                app.pagination.jumpToEvent(eventId)
        }
    }

    } // RowLayout

    // v0.7: Space Home — the management surface for a selected Space
    // (never an ordinary timeline). Hierarchy data is authoritative
    // SpaceManager state (m.space.child via sync); "Add existing room"
    // sends the real state event and the list updates when sync confirms.
    Component {
        id: spaceHomeComponent
        Rectangle {
            id: spaceHome
            objectName: "spaceHomePane"
            color: AppTheme.surface

            readonly property string spaceId:
                app.spaces ? app.spaces.activeSpaceId : ""
            property var info: ({})
            property var childRooms: []
            // v0.7.x: /hierarchy children the account has not joined
            // (join offers). Refreshed through RoomDiscoveryController.
            property var unjoinedChildren: []
            property string addNotice: ""
            property bool settingsOpen: false
            function refresh() {
                info = app.spaces ? app.spaces.spaceInfo(spaceId) : {}
                childRooms = app.spaces
                           ? app.spaces.childRoomsDetailed(spaceId) : []
                refreshUnjoined()
            }
            function refreshUnjoined() {
                if (spaceId === "" || !app.discovery.supported) {
                    unjoinedChildren = []
                    return
                }
                var rows = app.discovery.spaceChildren(spaceId)
                var out = []
                for (var i = 0; i < rows.length; ++i) {
                    if (rows[i].membership !== "joined")
                        out.push(rows[i])
                }
                unjoinedChildren = out
            }
            onSpaceIdChanged: {
                addNotice = ""
                refresh()
                if (spaceId !== "" && app.discovery.supported)
                    app.discovery.refreshSpaceChildren(spaceId)
            }
            Component.onCompleted: {
                refresh()
                if (spaceId !== "" && app.discovery.supported)
                    app.discovery.refreshSpaceChildren(spaceId)
            }
            Connections {
                target: app.discovery
                function onSpaceChildrenChanged(changedSpaceId) {
                    if (changedSpaceId === spaceHome.spaceId)
                        spaceHome.refreshUnjoined()
                }
                // A join changes a row's membership; the hierarchy answer
                // is re-read so the offer disappears (the joined list
                // itself updates through authoritative sync).
                function onRoomJoined() {
                    if (spaceHome.spaceId !== "")
                        app.discovery.refreshSpaceChildren(spaceHome.spaceId)
                }
                function onKnockSent() {
                    if (spaceHome.spaceId !== "")
                        app.discovery.refreshSpaceChildren(spaceHome.spaceId)
                }
            }
            Timer {
                id: spaceRefreshCoalesce
                interval: 250
                repeat: false
                onTriggered: spaceHome.refresh()
            }
            Connections {
                target: app.spaces
                function onSpacesChanged() { spaceRefreshCoalesce.restart() }
                function onChildAddFinished(spaceId, roomId, ok) {
                    if (spaceId !== spaceHome.spaceId) return
                    spaceHome.addNotice = ok
                        ? qsTr("Room added — waiting for the server to "
                               + "confirm.")
                        : qsTr("The room could not be added to this Space.")
                    spaceHome.refresh()
                }
                function onChildRemoveFinished(spaceId, roomId, ok) {
                    if (spaceId !== spaceHome.spaceId) return
                    spaceHome.addNotice = ok
                        ? qsTr("Room removed from this Space. The room "
                               + "itself is untouched.")
                        : qsTr("The room could not be removed — you may "
                               + "not have permission.")
                    spaceHome.refresh()
                }
            }

            Flickable {
                anchors.fill: parent
                contentWidth: width
                contentHeight: spaceCol.implicitHeight + AppTheme.spacing24 * 2
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                ColumnLayout {
                    id: spaceCol
                    width: Math.min(640, parent.width - AppTheme.spacing24 * 2)
                    x: (parent.width - width) / 2
                    y: AppTheme.spacing24
                    spacing: AppTheme.spacing16

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacing12
                        Avatar {
                            size: 56
                            name: spaceHome.info.name || ""
                            mxc: spaceHome.info.avatarUrl || ""
                            colorKey: spaceHome.spaceId
                            roomGlyph: true
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                objectName: "spaceHomeName"
                                text: spaceHome.info.name || qsTr("Space")
                                color: AppTheme.text
                                font.family: AppTheme.uiFont
                                font.pixelSize: 22
                                font.weight: Font.ExtraBold
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                visible: (spaceHome.info.topic || "").length > 0
                                text: spaceHome.info.topic || ""
                                color: AppTheme.textSecondary
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                                maximumLineCount: 3
                                elide: Label.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                text: {
                                    var c = spaceHome.info.childCount || 0
                                    var u = spaceHome.info.unreadTotal || 0
                                    var line = c === 1
                                        ? qsTr("1 room") : qsTr("%1 rooms").arg(c)
                                    if (u > 0)
                                        line += qsTr(" • %1 unread").arg(u)
                                    return line
                                }
                                color: AppTheme.textMuted
                                font.pixelSize: 12
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingS
                        AppButton {
                            objectName: "spaceCreateRoomButton"
                            kind: "primary"
                            text: qsTr("Create room here")
                            visible: app.conversations
                                     && app.conversations.supported
                            onClicked: root.newConversationRequested(
                                           "room", { addToSpace: true })
                        }
                        AppButton {
                            objectName: "spaceAddRoomButton"
                            text: qsTr("Add existing room")
                            onClicked: {
                                addRoomPopup.query = ""
                                addRoomPopup.refresh()
                                addRoomPopup.open()
                            }
                        }
                        AppButton {
                            objectName: "spaceSettingsButton"
                            text: spaceHome.settingsOpen
                                  ? qsTr("Hide settings") : qsTr("Space settings")
                            onClicked:
                                spaceHome.settingsOpen = !spaceHome.settingsOpen
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        visible: spaceHome.addNotice.length > 0
                        text: spaceHome.addNotice
                        color: AppTheme.textMuted
                        font.pixelSize: 12
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }

                    // Space settings — the Space IS a Matrix room; edits go
                    // through the same permission-gated room-edit backend.
                    Rectangle {
                        visible: spaceHome.settingsOpen
                        Layout.fillWidth: true
                        radius: AppTheme.radiusMd
                        color: AppTheme.cardElevated
                        border.color: AppTheme.border
                        border.width: 1
                        implicitHeight: settingsCol.implicitHeight
                                        + AppTheme.spacing16 * 2
                        onVisibleChanged: {
                            if (visible && app.roomInfo)
                                app.roomInfo.roomId = spaceHome.spaceId
                        }
                        FileDialog {
                            id: spaceAvatarDialog
                            title: qsTr("Choose Space avatar")
                            fileMode: FileDialog.OpenFile
                            nameFilters: [ qsTr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)") ]
                            // Same permission-gated backend as a room's own
                            // avatar — a Space IS a Matrix room, so this is
                            // m.room.avatar either way and Lightning invents
                            // no Space-specific storage.
                            onAccepted: app.roomInfo.setRoomAvatar(selectedFile)
                        }
                        ColumnLayout {
                            id: settingsCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing16
                            spacing: AppTheme.spacingS
                            Label {
                                text: qsTr("Avatar")
                                color: AppTheme.textMuted
                                font.pixelSize: 12
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacingS
                                AppButton {
                                    objectName: "spaceChangeAvatarButton"
                                    text: qsTr("Change avatar…")
                                    // canEditAvatar is the room's REAL
                                    // required level for m.room.avatar, read
                                    // from the member snapshot — never a role
                                    // label and never optimistic.
                                    enabled: app.roomInfo
                                             && app.roomInfo.canEditAvatar
                                             && !app.roomInfo.editPending
                                    onClicked: spaceAvatarDialog.open()
                                    Accessible.name: qsTr("Change the Space avatar")
                                }
                                AppButton {
                                    objectName: "spaceRemoveAvatarButton"
                                    kind: "danger"
                                    text: qsTr("Remove avatar")
                                    enabled: app.roomInfo
                                             && app.roomInfo.canEditAvatar
                                             && !app.roomInfo.editPending
                                    onClicked: app.roomInfo.removeRoomAvatar()
                                    Accessible.name: qsTr("Remove the Space avatar")
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                text: qsTr("Name")
                                color: AppTheme.textMuted
                                font.pixelSize: 12
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacingS
                                AppTextField {
                                    id: spaceNameField
                                    objectName: "spaceNameEditField"
                                    Layout.fillWidth: true
                                    text: spaceHome.info.name || ""
                                    enabled: app.roomInfo
                                             && app.roomInfo.canEditName
                                    Accessible.name: qsTr("Space name")
                                }
                                AppButton {
                                    text: qsTr("Rename")
                                    enabled: app.roomInfo
                                             && app.roomInfo.canEditName
                                             && spaceNameField.text.trim().length > 0
                                             && !app.roomInfo.editPending
                                    onClicked: app.roomInfo.setRoomName(
                                                   spaceNameField.text.trim())
                                }
                            }
                            Label {
                                text: qsTr("Topic")
                                color: AppTheme.textMuted
                                font.pixelSize: 12
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacingS
                                AppTextField {
                                    id: spaceTopicField
                                    objectName: "spaceTopicEditField"
                                    Layout.fillWidth: true
                                    text: spaceHome.info.topic || ""
                                    enabled: app.roomInfo
                                             && app.roomInfo.canEditTopic
                                    Accessible.name: qsTr("Space topic")
                                }
                                AppButton {
                                    text: qsTr("Save")
                                    enabled: app.roomInfo
                                             && app.roomInfo.canEditTopic
                                             && !app.roomInfo.editPending
                                    onClicked: app.roomInfo.setRoomTopic(
                                                   spaceTopicField.text.trim())
                                }
                            }
                            Label {
                                visible: app.roomInfo
                                         && app.roomInfo.editError.length > 0
                                text: app.roomInfo ? app.roomInfo.editError : ""
                                color: AppTheme.danger
                                font.pixelSize: 12
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            RowLayout {
                                Layout.topMargin: AppTheme.spacingS
                                spacing: AppTheme.spacingS
                                AppButton {
                                    objectName: "spaceLeaveButton"
                                    kind: "danger"
                                    text: qsTr("Leave Space")
                                    enabled: app.roomInfo
                                             && !app.roomInfo.leavePending
                                    onClicked: leaveSpaceConfirm.open()
                                }
                                Label {
                                    text: qsTr("Leaving does not remove the "
                                               + "rooms inside it.")
                                    color: AppTheme.textMuted
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }

                    Label {
                        text: qsTr("ROOMS IN THIS SPACE")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: 11
                        font.weight: Font.ExtraBold
                        font.letterSpacing: 0.8
                        Layout.topMargin: AppTheme.spacingS
                    }

                    // Empty state for a fresh Space.
                    Rectangle {
                        visible: spaceHome.childRooms.length === 0
                        Layout.fillWidth: true
                        radius: AppTheme.radiusMd
                        color: AppTheme.cardElevated
                        border.color: AppTheme.border
                        border.width: 1
                        implicitHeight: emptyCol.implicitHeight
                                        + AppTheme.spacing16 * 2
                        ColumnLayout {
                            id: emptyCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing16
                            spacing: AppTheme.spacingXS
                            Label {
                                text: qsTr("No rooms yet")
                                color: AppTheme.text
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Create a room here or add one of "
                                           + "your existing rooms to organise "
                                           + "it under this Space.")
                                color: AppTheme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Repeater {
                        model: spaceHome.childRooms
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 46
                            radius: AppTheme.radiusMd
                            color: childHover.hovered
                                   ? AppTheme.hover : "transparent"
                            HoverHandler { id: childHover }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacingS
                                anchors.rightMargin: AppTheme.spacingS
                                spacing: AppTheme.spacingS
                                Avatar {
                                    size: 32
                                    name: modelData.name || ""
                                    mxc: modelData.avatarUrl || ""
                                    colorKey: modelData.identityColorKey || modelData.roomId || ""
                                    circle: modelData.isDirect === true
                                    roomGlyph: modelData.isDirect !== true
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.name || qsTr("Room")
                                    color: AppTheme.text
                                    font.family: AppTheme.uiFont
                                    font.pixelSize: AppTheme.scaled(
                                        AppTheme.fontBody)
                                    font.weight: modelData.hasUnread
                                                 ? Font.Bold : Font.Medium
                                    elide: Label.ElideRight
                                }
                                Rectangle {
                                    visible: (modelData.highlightCount || 0) > 0
                                    radius: height / 2
                                    color: AppTheme.danger
                                    implicitHeight: 18
                                    implicitWidth: Math.max(
                                        18, childMention.implicitWidth + 10)
                                    Label {
                                        id: childMention
                                        anchors.centerIn: parent
                                        text: "@"
                                        color: AppTheme.accentText
                                        font.pixelSize: 10
                                        font.weight: Font.ExtraBold
                                    }
                                }
                                // v0.6.5: Space child-room unread badge —
                                // the same token every other unread badge in
                                // the app already reads (RoomDelegate,
                                // SpacesRail); this one was missed when
                                // unreadBadge was split off from accent, so
                                // it still rendered bolt-yellow under Storm
                                // while every other unread badge is
                                // periwinkle.
                                Rectangle {
                                    visible: modelData.hasUnread === true
                                    radius: height / 2
                                    color: AppTheme.unreadBadge
                                    implicitHeight: 18
                                    implicitWidth: Math.max(
                                        18, childCount.implicitWidth + 10)
                                    Label {
                                        id: childCount
                                        anchors.centerIn: parent
                                        visible: (modelData.unreadCount || 0) > 0
                                        text: modelData.unreadCount > 99
                                              ? "99+" : modelData.unreadCount
                                        color: AppTheme.accentText
                                        font.pixelSize: 11
                                        font.weight: Font.ExtraBold
                                    }
                                }
                                // MSC1772 child removal — the room itself
                                // stays; server-side permissions decide.
                                IconButton {
                                    objectName: "spaceChildRemoveButton"
                                    visible: childHover.hovered
                                    iconName: "close"
                                    iconSize: 14
                                    implicitWidth: 24; implicitHeight: 24
                                    Accessible.name: qsTr("Remove %1 from "
                                        + "this Space").arg(modelData.name || "")
                                    ToolTip.text: qsTr("Remove from Space")
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 600
                                    onClicked: {
                                        removeChildConfirm.roomId =
                                            modelData.roomId || ""
                                        removeChildConfirm.roomName =
                                            modelData.name || ""
                                        removeChildConfirm.open()
                                    }
                                }
                            }
                            TapHandler {
                                onTapped: if (modelData.roomId)
                                              app.openRoom(modelData.roomId)
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Open %1")
                                .arg(modelData.name || "")
                        }
                    }

                    // v0.7.x Discover: children of this Space the account
                    // has NOT joined, from the server's /hierarchy (SDK
                    // SpaceRoomList). The joined list above stays
                    // authoritative sync state; these rows are join offers.
                    Label {
                        visible: spaceHome.unjoinedChildren.length > 0
                        text: qsTr("MORE ROOMS IN THIS SPACE")
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                        font.weight: Font.ExtraBold
                        font.letterSpacing: 0.8
                        Layout.topMargin: AppTheme.spacingS
                    }
                    Repeater {
                        model: spaceHome.unjoinedChildren
                        delegate: Rectangle {
                            id: unjoinedRow
                            required property var modelData
                            readonly property bool rowKnocks:
                                modelData.joinRule === "knock"
                                || modelData.joinRule === "knock_restricted"
                            Layout.fillWidth: true
                            implicitHeight: 46
                            radius: AppTheme.radiusMd
                            color: unjoinedHover.hovered
                                   ? AppTheme.hover : "transparent"
                            HoverHandler { id: unjoinedHover }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacingS
                                anchors.rightMargin: AppTheme.spacingS
                                spacing: AppTheme.spacingS
                                Avatar {
                                    size: 32
                                    name: unjoinedRow.modelData.name || ""
                                    mxc: unjoinedRow.modelData.avatarUrl || ""
                                    colorKey: unjoinedRow.modelData.roomId || ""
                                    roomGlyph: true
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label {
                                        Layout.fillWidth: true
                                        text: unjoinedRow.modelData.name
                                              || qsTr("Room")
                                        color: AppTheme.text
                                        font.family: AppTheme.uiFont
                                        font.pixelSize: AppTheme.scaled(
                                            AppTheme.fontBody)
                                        font.weight: Font.Medium
                                        elide: Label.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("%n member(s)", "",
                                            Number(unjoinedRow.modelData.members
                                                   || 0))
                                        color: AppTheme.textMuted
                                        font.pixelSize: 11
                                        elide: Label.ElideRight
                                    }
                                }
                                Label {
                                    visible: unjoinedRow.modelData.membership
                                             === "knocked"
                                    text: qsTr("Request pending")
                                    color: AppTheme.textMuted
                                    font.pixelSize: 11
                                }
                                AppButton {
                                    visible: unjoinedRow.modelData.membership
                                             !== "knocked"
                                    kind: "primary"
                                    enabled: !app.discovery.busy
                                    text: unjoinedRow.rowKnocks
                                          ? qsTr("Ask to join") : qsTr("Join")
                                    onClicked: {
                                        var via = unjoinedRow.modelData.via || []
                                        if (unjoinedRow.rowKnocks)
                                            app.discovery.knock(
                                                unjoinedRow.modelData.roomId,
                                                via, "")
                                        else
                                            app.discovery.join(
                                                unjoinedRow.modelData.roomId,
                                                via,
                                                unjoinedRow.modelData.isSpace
                                                === true)
                                    }
                                }
                            }
                        }
                    }
                    Label {
                        visible: app.discovery.errorMessage.length > 0
                                 && spaceHome.unjoinedChildren.length > 0
                        Layout.fillWidth: true
                        text: app.discovery.errorMessage
                        color: AppTheme.danger
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }

            // Child-removal confirmation. Destructive only for the
            // hierarchy relation — never the room.
            Popup {
                id: removeChildConfirm
                property string roomId: ""
                property string roomName: ""
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                focus: true
                padding: AppTheme.spacing16
                background: Rectangle {
                    color: AppTheme.surface
                    radius: AppTheme.radiusMd
                    border.color: AppTheme.borderStrong
                    border.width: 1
                }
                contentItem: ColumnLayout {
                    spacing: AppTheme.spacing12
                    Label {
                        text: qsTr("Remove %1 from this Space?")
                            .arg(removeChildConfirm.roomName || qsTr("room"))
                        color: AppTheme.text
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: qsTr("The room keeps existing and you stay "
                                   + "in it — it just leaves this Space's "
                                   + "list.")
                        color: AppTheme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Item { Layout.fillWidth: true }
                        AppButton {
                            text: qsTr("Cancel")
                            onClicked: removeChildConfirm.close()
                        }
                        AppButton {
                            objectName: "spaceChildRemoveConfirmButton"
                            kind: "danger"
                            text: qsTr("Remove")
                            onClicked: {
                                app.spaces.removeRoomFromSpace(
                                    spaceHome.spaceId,
                                    removeChildConfirm.roomId)
                                removeChildConfirm.close()
                            }
                        }
                    }
                }
            }

            // Leave confirmation — leaving a Space never touches its rooms.
            Popup {
                id: leaveSpaceConfirm
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                focus: true
                padding: AppTheme.spacing16
                background: Rectangle {
                    color: AppTheme.surface
                    radius: AppTheme.radiusMd
                    border.color: AppTheme.borderStrong
                    border.width: 1
                }
                contentItem: ColumnLayout {
                    spacing: AppTheme.spacing12
                    Label {
                        text: qsTr("Leave %1?")
                            .arg(spaceHome.info.name || qsTr("this Space"))
                        color: AppTheme.text
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: qsTr("The rooms inside stay untouched.")
                        color: AppTheme.textSecondary
                        font.pixelSize: 12
                    }
                    RowLayout {
                        spacing: AppTheme.spacingS
                        Item { Layout.fillWidth: true }
                        AppButton {
                            text: qsTr("Cancel")
                            onClicked: leaveSpaceConfirm.close()
                        }
                        AppButton {
                            objectName: "spaceLeaveConfirmButton"
                            kind: "danger"
                            text: qsTr("Leave Space")
                            onClicked: {
                                leaveSpaceConfirm.close()
                                if (app.roomInfo) {
                                    app.roomInfo.roomId = spaceHome.spaceId
                                    app.roomInfo.leaveRoom()
                                }
                                app.spaces.activeSpaceId = ""
                            }
                        }
                    }
                }
            }

            // Add-existing-room picker: joined non-Space rooms, filtered,
            // with existing children clearly marked and un-addable.
            Popup {
                id: addRoomPopup
                objectName: "spaceAddRoomPopup"
                parent: Overlay.overlay
                anchors.centerIn: parent
                modal: true
                focus: true
                width: Math.min(420, (parent ? parent.width : 420)
                                - AppTheme.spacing24 * 2)
                padding: AppTheme.spacing16
                property string query: ""
                property var results: []
                function refresh() {
                    results = app.spaces
                              ? app.spaces.addableRooms(spaceHome.spaceId, query)
                              : []
                }
                background: Rectangle {
                    color: AppTheme.surface
                    radius: AppTheme.radiusLg
                    border.color: AppTheme.border
                    border.width: 1
                }
                contentItem: ColumnLayout {
                    spacing: AppTheme.spacing12
                    Label {
                        text: qsTr("Add a room to %1")
                            .arg(spaceHome.info.name || qsTr("this Space"))
                        color: AppTheme.text
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    AppTextField {
                        objectName: "spaceAddRoomSearch"
                        Layout.fillWidth: true
                        searchIcon: true
                        clearButton: true
                        placeholderText: qsTr("Search your rooms…")
                        Accessible.name: qsTr("Search rooms to add")
                        onTextChanged: {
                            addRoomPopup.query = text
                            addRoomPopup.refresh()
                        }
                    }
                    Label {
                        visible: addRoomPopup.results.length === 0
                        text: qsTr("No rooms to add.")
                        color: AppTheme.textMuted
                        font.pixelSize: 12
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(280, contentHeight)
                        clip: true
                        model: addRoomPopup.results
                        spacing: 2
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            height: 40
                            radius: AppTheme.radiusSm
                            color: addHover.hovered
                                   ? AppTheme.hover : "transparent"
                            HoverHandler { id: addHover }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: AppTheme.spacingXS
                                anchors.rightMargin: AppTheme.spacingXS
                                spacing: AppTheme.spacingS
                                Avatar {
                                    size: 26
                                    name: modelData.name || ""
                                    mxc: modelData.avatarUrl || ""
                                    colorKey: modelData.identityColorKey || modelData.roomId || ""
                                    circle: modelData.isDirect === true
                                    roomGlyph: modelData.isDirect !== true
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.name || qsTr("Room")
                                    color: AppTheme.text
                                    font.pixelSize: 13
                                    elide: Label.ElideRight
                                }
                                Label {
                                    visible: modelData.alreadyChild === true
                                    text: qsTr("Already added")
                                    color: AppTheme.textMuted
                                    font.pixelSize: 11
                                }
                                AppButton {
                                    visible: modelData.alreadyChild !== true
                                    text: qsTr("Add")
                                    Accessible.name: qsTr("Add %1 to the Space")
                                        .arg(modelData.name || "")
                                    onClicked: {
                                        app.spaces.addRoomToSpace(
                                            spaceHome.spaceId,
                                            modelData.roomId)
                                        addRoomPopup.close()
                                    }
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        AppButton {
                            text: qsTr("Close")
                            onClicked: addRoomPopup.close()
                        }
                    }
                }
            }
        }
    }

    // Files dragged anywhere over the CHAT — not only over the composer —
    // queue as attachments in the composer's tray (live feedback). DropArea
    // only consumes drag events, so scrolling and clicks are untouched.
    DropArea {
        id: chatDropArea
        // review H1: never cover the thread surface — ThreadPanel carries
        // its own DropArea routing to app.thread.addAttachment, and a
        // pane-wide acceptor here would hijack those drops into the ROOM
        // composer (CLAUDE.md section 8: thread attachments must use the
        // thread send path). Side-by-side layouts stop 340px short of the
        // right edge; the full-width thread layout (<660) collapses this
        // area to zero and the thread's own DropArea owns every drop.
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.threadSurfaceOpen
               ? (root.width >= 660 ? root.width - 340 : 0)
               : root.searchOpen
                 ? (root.width >= 700 ? root.width - 360 : 0)
                 : root.infoOpen && root.width >= 700
                   ? root.width - 320 : root.width
        z: 400
        enabled: app.composer.attachmentsSupported
                 && app.currentRoomId.length > 0
        keys: ["text/uri-list"]
        onDropped: (drop) => {
            if (!drop.hasUrls) return
            for (var i = 0; i < drop.urls.length; ++i)
                app.composer.addAttachment(drop.urls[i])
            drop.accept(Qt.CopyAction)
            messageComposer.focusStagedAttachmentSend()
        }
    }
    Rectangle {
        // The hint mirrors the drop area's thread-excluding geometry.
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: chatDropArea.width
        visible: chatDropArea.containsDrag
        z: 400
        color: "transparent"
        border.color: AppTheme.focusRing
        border.width: 2
        radius: AppTheme.radiusSm
        Rectangle {
            anchors.centerIn: parent
            radius: AppTheme.radiusMd
            color: AppTheme.surfaceElevated
            border.color: AppTheme.border
            border.width: 1
            width: dropHint.implicitWidth + AppTheme.spacing24
            height: dropHint.implicitHeight + AppTheme.spacing16
            Label {
                id: dropHint
                anchors.centerIn: parent
                text: qsTr("Drop files to attach")
                color: AppTheme.textPrimary
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
        }
    }
}
