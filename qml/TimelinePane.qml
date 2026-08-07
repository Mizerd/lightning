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
    // v0.6.0: the thread side surface is open when a thread panel OR the
    // room's Threads list view is showing.
    readonly property bool threadSurfaceOpen: app.thread.active
                                              || app.thread.listOpen
    // The authoritative right-panel state. Exactly one of three values:
    // the thread surface (controller-owned state) wins, then the info/member
    // panel, else none. All open/close paths flow through the two underlying
    // states and their exclusivity handlers below.
    readonly property string rightPanelState:
        threadSurfaceOpen ? "thread" : (infoOpen ? "info" : "none")

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
        if (threadSurfaceOpen)
            infoOpen = false
    }
    onInfoOpenChanged: {
        if (infoOpen)
            closeThreadSurface()
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
            if (app.currentScreen === 2)
                root.infoOpen = false
        }
        function onCurrentRoomIdChanged() {
            refreshCurrentRoom()
            // A find session belongs to the room it was opened in.
            if (root.findOpen) {
                root.findOpen = false
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
        }
    }

    // v0.6.1: find in loaded messages (this room's currently loaded timeline
    // only — never a server history search, never a persistent index).
    property bool findOpen: false
    function openFind() {
        if (app.currentRoomId === "") return
        root.findOpen = true
        app.timeline.beginSearch(findField.text)
        findField.forceActiveFocus()
        findField.selectAll()
    }
    function closeFind() {
        root.findOpen = false
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
        var row = app.timeline.rowForStableId(eventId)
        if (row < 0) return
        timeline.cancelWheelMotion()
        timeline.stickToBottom = false
        timeline.positionViewAtIndex(row, ListView.Center)
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
                 && (root.findOpen || root.infoOpen || root.threadSurfaceOpen
                     || timeline.pinnedActionsKey !== "")
        onActivated: {
            if (root.findOpen)
                root.closeFind()
            else if (root.infoOpen)
                root.infoOpen = false
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
            for (var i = timeline.count - 1; i >= timeline.count - tries; --i) {
                var eventId = app.timeline.eventIdAt(i)
                if (eventId !== "" && app.timeline.canEditEvent(eventId)) {
                    var ownItem = timeline.itemAtIndex(i)
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
            for (var j = timeline.count - 1; j >= timeline.count - tries; --j) {
                var item = timeline.itemAtIndex(j)
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
    // v0.6.6: "Star GIF" / "Unstar GIF" feedback — the same auto-clearing
    // banner Save As already uses (an explicit-export action of the same
    // class; see GifStarredStore's header). Honest failures only: no silent
    // drop. `message` is ALREADY a translated, ready-to-display sentence
    // (see GifStarredStore::categoryMessage) — never a raw category token.
    Connections {
        target: app.gif.starredStore
        function onStarFinished(mediaKey, ok, category, message) {
            saveResult.ok = ok
            saveResult.text = ok ? qsTr("Starred.") : message
            saveResultTimer.restart()
        }
        function onUnstarFinished(hash) {
            saveResult.ok = true
            saveResult.text = qsTr("Removed from starred GIFs.")
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
                            text: root.currentRoom.name
                                  ? root.currentRoom.name
                                  : (app.currentRoomId === ""
                                     ? qsTr("Home")
                                     : app.currentRoomId)
                            color: AppTheme.text
                            font.pixelSize: 15
                            font.weight: Font.ExtraBold
                        }
                        Icon {
                            visible: root.currentRoom.encrypted === true
                            name: "lock"
                            size: 13
                            color: AppTheme.textMuted
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
                        active: root.findOpen
                        Accessible.name: qsTr("Find in loaded messages")
                        ToolTip.text: qsTr("Find in loaded messages")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        onClicked: root.findOpen ? root.closeFind()
                                                 : root.openFind()
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
            implicitHeight: findRow.implicitHeight + AppTheme.spacingS * 2
            radius: AppTheme.radiusLg
            color: AppTheme.surface
            border.color: AppTheme.border
            border.width: 1
            RowLayout {
                id: findRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacingS
                spacing: AppTheme.spacingS
                AppTextField {
                    id: findField
                    objectName: "timelineFindField"
                    Layout.fillWidth: true
                    searchIcon: true
                    clearButton: true
                    placeholderText: qsTr("Search visible messages…")
                    Accessible.name: qsTr("Find in loaded messages")
                    onTextChanged: if (root.findOpen)
                                       app.timeline.updateSearch(text)
                    Keys.onReturnPressed: (event) => {
                        if (event.modifiers & Qt.ShiftModifier)
                            app.timeline.searchPrev()
                        else
                            app.timeline.searchNext()
                        event.accepted = true
                    }
                    Keys.onEscapePressed: root.closeFind()
                }
                Label {
                    objectName: "timelineFindCount"
                    text: app.timeline.searchResultCount > 0
                          ? qsTr("%1 of %2").arg(app.timeline.searchCurrentPosition)
                                            .arg(app.timeline.searchResultCount)
                          : (findField.text.length > 0 ? qsTr("No matches") : "")
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                }
                IconButton {
                    implicitWidth: 28; implicitHeight: 28
                    radius: 6
                    iconName: "expand_less"
                    iconSize: 16
                    enabled: app.timeline.searchResultCount > 0
                    Accessible.name: qsTr("Previous match")
                    onClicked: app.timeline.searchPrev()
                }
                IconButton {
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
        }

        // Timeline
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: timeline
                objectName: "timelineListView"
                anchors.fill: parent
                clip: true
                // The presentation gate covers the view (it keeps laying out
                // underneath so viewport-fill pagination and positioning run
                // against real geometry); the loading surface sits on top.
                opacity: presentationReady ? 1 : 0
                // Fast-scroll: pool delegates instead of re-instantiating
                // them mid-flick. This is safe because every per-row field is
                // either model-bound with a reset-on-change handler (preview
                // via onActionKeyChanged; media/GIF via onMediaIdentityChanged
                // inside a Loader), backend-owned (decryption retry is bounded
                // in the model, not per delegate), rendered in the Overlay
                // (context/reaction popups, details dialog — not pooled with
                // the row), or a write-before-use popup target. MessageDelegate
                // additionally scrubs the last group defensively in
                // ListView.onReused -> resetForReuse(). cacheBuffer keeps a
                // screen of pooled delegates warm above/below the viewport.
                reuseItems: true
                // A bigger buffer was tried (1600) as an up-scroll jitter
                // mitigation — wrapped-text rows resolve height only on
                // delegate creation — and REJECTED on measurement: it more
                // than doubled the timeline suite's runtime and woke the
                // documented offscreen adjacency flake (~29%), for a benefit
                // no one had physically verified. The jitter's real fix, if
                // the feel report persists, is height handling, not buffer
                // size.
                cacheBuffer: 800
                // Delegates own sender-group spacing: group leaders receive
                // a compact break while continuations stay visually glued
                // together. A global gap made every continuation look like
                // an unrelated row.
                spacing: 0
                model: app.timeline
                verticalLayoutDirection: ListView.TopToBottom
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
                leftMargin: AppTheme.spacingM
                rightMargin: AppTheme.spacingM

                delegate: MessageDelegate {
                    // Delegate incubation can begin before ListView.view has
                    // its final width. A non-positive startup width made long
                    // wrapped bodies measure as one-character-wide, producing
                    // enormous transient heights and an endless create/drop
                    // cycle. Use a normal message-column fallback only until
                    // the real viewport width is positive.
                    width: {
                        var available = ListView.view
                                      ? ListView.view.width
                                        - AppTheme.spacingM * 2 : 0
                        return available > 0 ? available : 640
                    }
                }

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
                    return atYEnd
                           || (contentY + height >= contentHeight - bottomFollowSlack)
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
                function recomputePresentationReady() {
                    if (presentationReady)
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
                // mid-gesture correction. maintainViewAnchor() re-bases this
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
                // Row index and content height at the last measurement. When
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
                function captureViewAnchor() {
                    if (stickToBottom || count === 0) {
                        viewAnchorId = ""
                        viewAnchorLastY = 0
                        viewAnchorCount = count
                        viewAnchorRow = -1
                        viewAnchorContentHeight = contentHeight
                        return
                    }
                    var row = indexAt(width / 2, contentY + topMargin + 1)
                    if (row < 0) {
                        viewAnchorId = ""
                        viewAnchorLastY = 0
                        return
                    }
                    var it = itemAtIndex(row)
                    for (var probe = row; probe < count; ++probe) {
                        var candidate = itemAtIndex(probe)
                        if (!candidate)
                            break
                        if (!candidate.isStateActivity) {
                            row = probe
                            it = candidate
                            break
                        }
                    }
                    viewAnchorId = app.timeline.stableIdAt(row)
                    viewAnchorOffset = it ? (contentY - it.y) : 0
                    viewAnchorLastY = it ? it.y : 0
                    viewAnchorCount = count
                    viewAnchorRow = row
                    viewAnchorContentHeight = contentHeight
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
                    if (viewAnchorId === "" || stickToBottom)
                        return
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
                    var row = app.timeline.rowForStableId(viewAnchorId)
                    var it = row >= 0 ? itemAtIndex(row) : null
                    var anchorY = it ? it.y : 0
                    if (!it && row >= 0 && !moving
                        && row > viewAnchorRow && viewAnchorRow >= 0) {
                        // DISPLACED by an insertion above the reader. The row
                        // index rising is the proof: rows were inserted
                        // before it, which at the top edge pushes it clear of
                        // the delegate cache so Qt destroys it. The shift is
                        // then exactly how much taller the content became —
                        // no delegate needs to exist to know that. Applying
                        // it relatively preserves an in-flight gesture, and
                        // the baseline moves by the same amount (the row's own
                        // y rose by the growth), so nothing double-counts.
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
                        if (grew > 0.5) {
                            if (diagActive)
                                diagGrowthCorrections += 1
                            contentY += grew
                            if (app.timelineScroll.motionActive)
                                app.timelineScroll.translateActiveMotion(grew)
                            viewAnchorLastY += grew
                        }
                        viewAnchorCount = count
                        viewAnchorRow = row
                        viewAnchorContentHeight = contentHeight
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
                        captureViewAnchor()
                        return
                    }
                    if (userScrollActive) {
                        if (!selfDrivenScrollActive) {
                            // Native drag/flick: Flickable owns contentY and
                            // rebuilds it from the press position each move,
                            // so a correction written here would be silently
                            // discarded — and re-basing viewAnchorLastY would
                            // then hide the growth from settle-time handling
                            // too. Leave BOTH untouched: at settle the growth
                            // is absorbed and re-anchored (onMovementEnded ->
                            // captureViewAnchor), exactly as it was before
                            // this round.
                            return
                        }
                        // Mid-gesture correction, on the self-driven paths
                        // only. The key invariant: user input (wheel notch,
                        // touchpad pixel delta) only ever
                        // changes contentY — it never moves a row's own
                        // content-space y. So `it.y` changes if and only if
                        // content ABOVE this row resized OR rows were
                        // inserted above it — a pagination prepend is simply
                        // the insertion case of the same rule and needs no
                        // special handling: same delta, same application,
                        // including translating an active glide rather than
                        // freezing it. The dominant resize
                        // source is a POST-CREATION height change — a pooled
                        // (reuseItems) delegate re-measuring once it is bound
                        // to a taller row, media/decryption/link-preview
                        // resolution — which re-triggers layoutVisibleItems():
                        // that re-anchors on the first visible item and pushes
                        // every following row, including this one, down. (Row
                        // CREATION above the viewport is NOT the mechanism:
                        // ListView positions a new row upward from the
                        // existing first item, moving originY/contentHeight
                        // rather than this row's y.) `delta = it.y - viewAnchorLastY` is
                        // therefore PURELY that resize — never the user's own
                        // scrolling — so applying it as `contentY += delta` is
                        // a pure frame-of-reference correction: it preserves
                        // the gesture's in-flight distance and velocity
                        // exactly, unlike writing an ABSOLUTE position (the
                        // idle-path formula below), which can disagree with
                        // wherever the gesture has since moved the view and
                        // fight it. This is the same reason
                        // translateActiveMotion() applies a RELATIVE shift
                        // rather than an absolute write. Deliberately NOT
                        // clamped to
                        // wheelMinY()/wheelMaxY(). The UPPER bound tracks
                        // growth exactly (originY + contentHeight is the last
                        // row's end), so a positive delta cannot push past
                        // it. The LOWER bound does NOT move with the delta —
                        // originY follows the averaged-size estimate, not
                        // this row's resize — so the shrink case rests on a
                        // different argument: a resize located above the
                        // viewport top means contentY is already at least
                        // that far past the first visible item's y, so
                        // subtracting it cannot cross wheelMinY(). Do not
                        // restate this as "both bounds move together": they
                        // do not.
                        var delta = anchorY - viewAnchorLastY
                        if (Math.abs(delta) > 0.5) {
                            if (diagActive)
                                diagGrowthCorrections += 1
                            contentY += delta
                            // A discrete-wheel glide carries its own
                            // coalesced target inside TimelineScrollController.
                            // Shifting contentY here without also translating
                            // it would let the glide's very next frame
                            // overwrite this correction with its stale
                            // pre-growth position.
                            if (app.timelineScroll.motionActive)
                                app.timelineScroll.translateActiveMotion(delta)
                        }
                        // Re-based even when the delta was below the
                        // threshold: sub-pixel churn is deliberately absorbed
                        // rather than accumulated, since applying it would be
                        // its own source of jitter.
                        viewAnchorLastY = anchorY
                        viewAnchorCount = count
                        viewAnchorRow = row
                        viewAnchorContentHeight = contentHeight
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
                    // mid-gesture, just via the absolute form, which is only
                    // safe because nothing is competing for contentY.
                    var desired = anchorY + viewAnchorOffset
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    desired = desired < lo ? lo : (desired > hi ? hi : desired)
                    if (Math.abs(contentY - desired) > 0.5) {
                        if (diagActive)
                            diagAnchorCorrections += 1
                        contentY = desired
                    }
                    viewAnchorLastY = anchorY
                    viewAnchorCount = count
                    viewAnchorRow = row
                    viewAnchorContentHeight = contentHeight
                }

                // v0.6.0: MessageDelegate view contract — the room timeline
                // resolves stable-id actions against app.timeline and never
                // suppresses a row as a pinned thread root.
                property var timelineModel: app.timeline
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
                    var row = indexAt(width / 2, contentY + topMargin + 1)
                    if (row < 0) return
                    var eventId = app.timeline.eventIdAt(row)
                    for (var probe = row; eventId === "" && probe < count; ++probe)
                        eventId = app.timeline.eventIdAt(probe)
                    if (eventId === "") return
                    var item = itemAtIndex(app.timeline.rowForStableId(eventId))
                    app.pagination.saveScrollAnchor(
                                app.currentRoomId, eventId,
                                item ? contentY - item.y : 0, false)
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
                // reconciling. Calling positionViewAtEnd() synchronously inside
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
                        positionViewAtEnd()
                }

                // ── v0.5.19/v0.6.0: device-aware wheel scrolling ─────────
                // Discrete mouse-wheel notches are coalesced by
                // TimelineScrollController (app.timelineScroll), which in
                // 0.6.0 also OWNS the motion: a wheel-only C++ engine
                // integrates contentY toward the coalesced target each frame
                // with continuous velocity, so movement through one tall
                // wrapped message is a single smooth glide instead of the
                // restart-per-notch bursts a fixed-duration animation
                // produced. Touchpad / high-resolution pixel deltas still
                // move contentY directly to keep fine movement and native
                // momentum precise. Programmatic navigation cancels wheel
                // motion first, so restoration is never animated as if it
                // were physical input.
                property bool wheelAnimating: app.timelineScroll.motionActive

                // ── Scroll-session state ─────────────────────────────────
                // True while the user's input owns the viewport: a native
                // drag/flick (moving/dragging), the mouse-wheel motion engine
                // (wheelAnimating), OR a high-resolution touchpad gesture. The
                // touchpad path writes contentY programmatically (so
                // Flickable.moving never turns true) and cancels the wheel
                // engine, so neither native flag detects it; the settle timer,
                // restarted on every wheel/pixel delta, is the reliable
                // "input seen within the last 250ms" signal. Qt's QML WheelEvent
                // exposes no scroll phase and no device type (and phase
                // begin/end is macOS-only even in C++), so a restarted quiet
                // timer — not phases — is the portable gesture-active heuristic
                // on KDE Wayland. While this is true, Lightning must not write
                // the timeline position except through the one active input
                // owner; deferred corrections wait for it to clear.
                // (`moving` already covers both dragging and flicking.)
                readonly property bool userScrollActive:
                    moving || wheelAnimating || scrollSettleTimer.running

                // True only while a gesture Lightning ITSELF drives contentY
                // for: the wheel glide and the touchpad pixel path both write
                // contentY programmatically, which is exactly why they leave
                // Flickable.moving false. During a NATIVE drag or kinetic
                // flick (`moving`), QQuickFlickable owns contentY and
                // recomputes it from the recorded press position on every
                // move — an external write landing between two moves is
                // discarded by construction — so a growth correction there
                // must be DEFERRED to settle, never applied-and-lost.
                readonly property bool selfDrivenScrollActive:
                    !moving && (wheelAnimating || scrollSettleTimer.running)

                // ── Bounded per-gesture scroll diagnostics ───────────────
                // Off unless LIGHTNING_SCROLL_TRACE is set (read once in
                // TimelineScrollController). When on, ONE summary line is
                // emitted per wheel/touchpad gesture at settle — never one per
                // event — so a physical tester can capture a real trace and
                // send it back. See the counter block below for what
                // anchorCorrections and growthCorrections each mean now that
                // maintainViewAnchor() has two modes.
                // v0.7.2: a separate paginationRestores counter used to
                // live here for the pagination anchor's own restore path. It
                // is gone because that whole mechanism is gone — a prepend
                // landing mid-gesture is now counted by growthCorrections,
                // exactly like any other structural change above the anchor.
                // Its disappearance IS the evidence the two mechanisms
                // actually merged rather than one being renamed.
                // No message content, ids, or URLs are logged.
                readonly property bool scrollTrace: app.timelineScroll.scrollTraceEnabled
                property bool diagActive: false
                property int diagEvents: 0
                property int diagPixelEvents: 0
                property int diagAngleEvents: 0
                // diagAnchorCorrections counts ONLY the idle absolute-restore
                // path in maintainViewAnchor() — it must stay 0 while a
                // gesture owns the view; a non-zero value there would be an
                // absolute write fighting the input. diagGrowthCorrections
                // counts the SEPARATE relative-delta path that DOES run
                // mid-gesture: how many times content growth above the anchor
                // (a pooled delegate re-measuring after rebinding, media
                // hydration, late decryption, a link preview) was compensated
                // by shifting contentY by exactly that growth. Non-zero during
                // a media-heavy gesture is EXPECTED — it is the fix for the
                // "an image pops in while scrolling and the view jumps"
                // defect, not a regression of it.
                property int diagAnchorCorrections: 0
                property int diagGrowthCorrections: 0
                property real diagStartY: 0
                property real diagStartHeight: 0
                function diagNoteEvent(isPixel) {
                    if (!scrollTrace)
                        return
                    if (!diagActive) {
                        diagActive = true
                        diagEvents = 0
                        diagPixelEvents = 0
                        diagAngleEvents = 0
                        diagAnchorCorrections = 0
                        diagGrowthCorrections = 0
                        diagStartY = contentY
                        diagStartHeight = contentHeight
                    }
                    diagEvents += 1
                    if (isPixel)
                        diagPixelEvents += 1
                    else
                        diagAngleEvents += 1
                }
                function diagFlushGesture() {
                    if (!scrollTrace || !diagActive)
                        return
                    diagActive = false
                    console.info("scroll-gesture"
                        + " events=" + diagEvents
                        + " pixel=" + diagPixelEvents
                        + " angle=" + diagAngleEvents
                        + " netY=" + Math.round(contentY - diagStartY)
                        + " dContentH=" + Math.round(contentHeight - diagStartHeight)
                        + " anchorCorrections=" + diagAnchorCorrections
                        + " growthCorrections=" + diagGrowthCorrections
                        + " stick=" + (stickToBottom ? 1 : 0)
                        + " topDist=" + Math.round(distanceFromTop())
                        + " nearTop=" + (distanceFromTop() <= nearTopEnterDistance ? 1 : 0))
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
                readonly property real nearTopEnterDistance: height * 0.5
                readonly property real nearTopExitDistance: height * 0.9
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
                function distanceFromTop() { return contentY - wheelMinY() }
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
                    app.timelineScroll.animateTo(targetY, contentY, lo, hi)
                    updateStickAndPaginate()
                    // Any upward intent leaves follow-latest — applied last so
                    // the geometry recompute above (still on the pre-motion
                    // position) cannot re-enable it while scrolling up.
                    if (targetY < contentY - 0.5)
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
                    beginWheelTo(contentY + direction * height * 0.9)
                }
                // Home is programmatic navigation like End: it bypasses the
                // wheel motion engine and jumps directly, then recomputes
                // pagination / follow-latest and saves one settled anchor.
                function goToEarliestLoaded() {
                    cancelWheelMotion()
                    contentY = wheelMinY()
                    updateStickAndPaginate()
                    scrollSettleTimer.restart()
                }
                function goToLatest() {
                    cancelWheelMotion()
                    stickToBottom = true
                    // positionViewAtEnd() below can re-seed the position
                    // frame from an estimate; a surviving anchor baseline
                    // would then measure a delta across two different frames.
                    viewAnchorId = ""
                    viewAnchorLastY = 0
                    app.pagination.saveFollowingLatest(app.currentRoomId)
                    positionViewAtEnd()
                    Qt.callLater(function() {
                        positionViewAtEnd()
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

                // v0.6.0: the wheel motion engine lives in C++ (one ticker,
                // active only while motion is in flight — never a per-event
                // animation queue and never a permanently running timer).
                // QML's job is to apply each emitted frame to contentY,
                // re-clamped against LIVE geometry: a pagination prepend or a
                // delegate height change mid-motion can move the valid range,
                // and pushing past a bound must end the motion instead of
                // jittering against it.
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
                        // The single gesture-settle point for both the touchpad
                        // and mouse-engine paths (the engine's settle restarts
                        // this timer): emit one bounded diagnostic summary.
                        timeline.diagFlushGesture()
                    }
                }

                WheelHandler {
                    id: timelineWheelHandler
                    objectName: "timelineWheelHandler"
                    // We move contentY ourselves; the handler must not also
                    // manipulate a target of its own.
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: (event) => {
                        var minY = timeline.wheelMinY()
                        var maxY = timeline.wheelMaxY()
                        if (event.pixelDelta.y !== 0) {
                            // Touchpad / high-resolution: precise and direct,
                            // no notch multiplier, no competing animation.
                            timeline.cancelWheelMotion()
                            timeline.contentY = app.timelineScroll.pixelTargetY(
                                event.pixelDelta.y, timeline.contentY, minY, maxY)
                            timeline.updateStickAndPaginate()
                            // Any upward intent immediately establishes user
                            // ownership and leaves follow-latest — parity with
                            // the mouse branch below. Without this a touchpad
                            // up-scroll near the bottom (which cancels the wheel
                            // engine, so wheelAnimating is false) could not
                            // disengage bottom-follow, and the next async
                            // content-height change teleported the view down.
                            if (event.pixelDelta.y > 0)
                                timeline.stickToBottom = false
                            // No per-delta anchor RE-CAPTURE: this does not
                            // re-run a fresh indexAt/itemAtIndex/stableIdAt
                            // scan on every touchpad delta to find "the row
                            // currently under the reader" (that full
                            // re-derivation still happens only once the
                            // gesture settles, via captureViewAnchor()).
                            // maintainViewAnchor() DOES still run mid-gesture
                            // whenever content height changes, but it measures
                            // against the ALREADY-tracked viewAnchorId via a
                            // rowForStableId + itemAtIndex lookup
                            // (rowForStableId is an O(n) scan of the event
                            // mirror, bounded here by the callLater
                            // coalescing to once per event-loop turn) and
                            // applies a RELATIVE delta (see
                            // maintainViewAnchor()) — never the per-event full
                            // scan the earlier code ran, which was pure cost
                            // on the touchpad hot path, worst near the top and
                            // over tall media rows.
                            timeline.diagNoteEvent(true)
                            scrollSettleTimer.restart()
                        } else if (event.angleDelta.y !== 0) {
                            // Discrete mouse wheel: the C++ engine coalesces
                            // the notch into the in-flight motion (or starts
                            // one) with continuous velocity.
                            app.timelineScroll.wheelNotch(
                                event.angleDelta.y, timeline.contentY,
                                minY, maxY, timeline.height)
                            timeline.updateStickAndPaginate()
                            // Any upward intent leaves follow-latest.
                            if (event.angleDelta.y > 0)
                                timeline.stickToBottom = false
                            timeline.diagNoteEvent(false)
                            scrollSettleTimer.restart()
                        }
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

                // ── v0.7.x: near-top backfill "virtual scrolling" ────────
                // Live report: loading gets "jittery and laggy and
                // teleporty" once messages start loading WHILE the reader
                // holds a scroll gesture through it — the per-batch anchor
                // correction below IS exact for a run that never engages
                // the staging window this comment describes (see
                // TimelinePaneQmlTest.cpp's
                // nearTopUnstagedRunStillCompensatesEveryBatchImmediately,
                // which deliberately never lets nearTopRunActive or
                // nearTopArmed become true so it can prove exactly that
                // path — a NearTop-driven run, the shape this comment is
                // actually about, engages staging below instead, and is
                // proven in the same file by
                // nearTopControllerDrivenRunAlsoStagesAcrossEveryBatchThenReconcilesOnce
                // and
                // nearTopVirtualScrollingFreezesTheViewportAcrossAHeldRunThenReconcilesOnce
                // — names kept on one line so they grep). Either way, every
                // landed batch still pays real relayout/delegate/media-
                // request cost stacked against the held gesture. The fix
                // is architectural, not another compensation tweak:
                // while a gesture is active AND a near-top approach to the
                // top is unsettled, TimelineModel holds landed batches out
                // of the exposed row space entirely (see
                // TimelineModel::setBackfillStagingActive) — the ListView
                // sees no structural change at all until this flips back to
                // false. That is genuinely "scroll what's already loaded,
                // then reconcile once" rather than buffering the visual
                // CONSEQUENCES of updates that still land one at a time.
                //
                // "A near-top approach is unsettled" is deliberately the OR
                // of two signals, not just one:
                //   * !nearTopArmed — QML's OWN edge latch (checkNearTopEdge)
                //     is consumed at dispatch and only re-arms at gesture
                //     settle or leaving the exit band, so it already spans
                //     the WHOLE approach across every chained batch a
                //     continued upward scroll triggers — not just one
                //     request's own round trip. This is what makes multiple
                //     real backfill pages during one continuous scroll
                //     coalesce into a single reconcile instead of one per
                //     batch.
                //   * app.pagination.nearTopRunActive — the controller's own
                //     view of "a NearTop request or its bounded continuation
                //     is in flight right now", true even when something
                //     dispatched a request WITHOUT going through QML's edge
                //     latch (a bounded continuation re-dispatches directly in
                //     C++; so can a test or any future caller).
                // Either alone is a real signal that the reader is mid-
                // approach; this must never depend on exactly which path
                // triggered the fetch.
                readonly property bool backfillStagingActive:
                    userScrollActive
                    && (!nearTopArmed || app.pagination.nearTopRunActive)
                // NOT a direct Binding. QQmlTimer.restart() while already
                // running is stop()+start() — two separate synchronous
                // runningChanged emits, momentarily false then true — and
                // scrollSettleTimer.restart() is called from EVERY wheel/
                // touchpad delta during a real gesture (see the WheelHandler
                // below). A direct Binding would therefore flush-then-
                // restage on every single input event during the exact
                // gesture this exists to keep smooth — the opposite of the
                // goal, and additional jitter of its own. Qt.callLater
                // coalesces the momentary blip-and-recover within the same
                // synchronous turn into zero net change, applying only the
                // value that is still current once the call stack unwinds —
                // the same pattern maintainViewAnchorCoalesced() and
                // maybeRequestNearTop() already use for this exact reason.
                // No generation/room token, unlike PaginationController's
                // deferred C++ callbacks (scheduleNearTopContinuation() etc,
                // which DO carry one) — and that is not an oversight, it is
                // the same reasoning maintainViewAnchorCoalesced() already
                // relies on without one. Those C++ callbacks capture FROZEN
                // values at schedule time (rowsAtDispatch, a specific batch)
                // that a room switch can make meaningless, so they must
                // reject a stale generation outright. This callback captures
                // NOTHING: it reads `timeline.backfillStagingActive` fresh,
                // live, at the moment it actually runs — never a value
                // snapshotted at schedule time — so it always applies
                // whatever is CURRENTLY true, however many gestures, room
                // switches, or property changes happened in between. A room
                // switch in that window is also independently safe on the
                // C++ side: TimelineModel::reload() zeroes the hidden prefix
                // and the staging flag directly (not through this property)
                // the instant the switch happens, synchronously, before this
                // already-queued callback can run — so even a "stale"
                // pending write lands on a model that has already reset
                // itself and simply re-applies current truth.
                property bool backfillStagingSyncScheduled: false
                onBackfillStagingActiveChanged: {
                    if (backfillStagingSyncScheduled)
                        return
                    backfillStagingSyncScheduled = true
                    Qt.callLater(function() {
                        backfillStagingSyncScheduled = false
                        // Re-read here, not a captured parameter — this IS
                        // the "apply the latest transition" guarantee: no
                        // matter how many changes coalesced into this one
                        // callback, or what else ran meanwhile, the value
                        // written is whatever is true right now.
                        app.timeline.backfillStagingActive =
                            timeline.backfillStagingActive
                    })
                }

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
                    function onTargetLocated(row, pixelOffset, highlight) {
                        // Reply navigation takes control immediately.
                        timeline.cancelWheelMotion()
                        Qt.callLater(function() {
                            timeline.cancelWheelMotion()
                            timeline.stickToBottom = false
                            timeline.positionViewAtIndex(
                                        row, highlight ? ListView.Center
                                                       : ListView.Beginning)
                            if (!highlight) {
                                var item = timeline.itemAtIndex(row)
                                if (item) timeline.contentY = item.y + pixelOffset
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
                    // React to a user drag/flick AND to our own wheel/pixel
                    // motion; ignore programmatic navigation (jump, reply,
                    // restore) which manages follow-latest itself, and the
                    // anchor probe's transient materialising move — reading
                    // stickToBottom or dispatching a near-top request from a
                    // position the reader was never at would be wrong.
                    if (!moving && !wheelAnimating) return
                    stickToBottom = atBottomEdge()
                    // Trigger backfill as the user approaches the top
                    // (drag/flick/wheel), edge-latched with hysteresis so it
                    // fires once per approach rather than every frame.
                    checkNearTopEdge(true)
                }
                onCountChanged: {
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
                        presentationGuard.restart()
                        Qt.callLater(function() {
                            app.pagination.restoreScrollAnchor(app.currentRoomId)
                        })
                        Qt.callLater(timeline.maybeFillViewport)
                        Qt.callLater(timeline.recomputePresentationReady)
                    }
                }

                // Pagination trigger: scroll to top with backfill available.
                // Duplicate and reached-start suppression live in the
                // controller.
                onAtYBeginningChanged: {
                    // Reaching the exact top is a PASSIVE geometry signal (also
                    // the first empty/incubating frame that kicks off the
                    // initial-history fill). It stays userInitiated=false so it
                    // never resets the controller's filtered-page bound, and the
                    // controller now bounds passive requests too — so this can
                    // neither defeat the loop stop nor be the loop. It is NOT
                    // edge-latched because the fresh-room fill must fire even
                    // while stickToBottom is still true.
                    if (atYBeginning)
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

        // Typing indicator
        Rectangle {
            Layout.fillWidth: true
            visible: app.timeline.typingText && app.timeline.typingText.length > 0
            implicitHeight: typingLabel.implicitHeight + 6
            color: AppTheme.background
            Label {
                id: typingLabel
                anchors.left: parent.left
                anchors.leftMargin: AppTheme.spacingM
                anchors.verticalCenter: parent.verticalCenter
                text: app.timeline.typingText
                color: AppTheme.textMuted
                font.italic: true
                font.pixelSize: 11
            }
        }

        // v0.5.9: Save As result feedback (auto-clears).
        Rectangle {
            Layout.fillWidth: true
            visible: saveResult.text.length > 0
            implicitHeight: saveResult.implicitHeight + 6
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

        // The composer has no target when no room is selected — the Home
        // surface is shown instead. visible:false collapses its space.
        MessageComposerBar {
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
            property string addNotice: ""
            property bool settingsOpen: false
            function refresh() {
                info = app.spaces ? app.spaces.spaceInfo(spaceId) : {}
                childRooms = app.spaces
                           ? app.spaces.childRoomsDetailed(spaceId) : []
            }
            onSpaceIdChanged: { addNotice = ""; refresh() }
            Component.onCompleted: refresh()
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
                        ColumnLayout {
                            id: settingsCol
                            anchors.fill: parent
                            anchors.margins: AppTheme.spacing16
                            spacing: AppTheme.spacingS
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
                                    colorKey: modelData.roomId || ""
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
                                    colorKey: modelData.roomId || ""
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
}
