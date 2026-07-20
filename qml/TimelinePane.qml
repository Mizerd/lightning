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
    signal newConversationRequested(string mode)

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
            timeline.anchorStableId = ""
            timeline.anchorOffset = 0
            timeline.anchorContentHeight = 0
            timeline.viewAnchorId = ""
            timeline.viewAnchorOffset = 0
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
        }
    }
    FileDialog {
        id: saveMediaDialog
        property string pendingMediaKey: ""
        title: qsTr("Save file as…")
        fileMode: FileDialog.SaveFile
        onAccepted: {
            if (pendingMediaKey.length > 0)
                app.mediaBridge.saveAs(pendingMediaKey, selectedFile)
            pendingMediaKey = ""
        }
        onRejected: pendingMediaKey = ""
    }
    Connections {
        target: app.mediaBridge
        function onSaveFinished(ok, message) {
            saveResult.ok = ok
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
        Rectangle {
            id: findBar
            objectName: "timelineFindBar"
            Layout.fillWidth: true
            visible: root.findOpen
            implicitHeight: findRow.implicitHeight + AppTheme.spacingS * 2
            color: AppTheme.surface
            RowLayout {
                id: findRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacingS
                spacing: AppTheme.spacingS
                Label {
                    text: qsTr("Find in loaded messages:")
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                }
                TextField {
                    id: findField
                    objectName: "timelineFindField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search visible messages…")
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
                cacheBuffer: 800
                // Delegates own sender-group spacing: group leaders receive
                // a compact break while continuations stay visually glued
                // together. A global gap made every continuation look like
                // an unrelated row.
                spacing: 0
                model: app.timeline
                verticalLayoutDirection: ListView.TopToBottom
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
                // in-motion states are owned by their own mechanisms; the
                // backward-pagination prepend keeps its dedicated capture/
                // restore pair (which can re-locate rows that left the
                // instantiated range).
                property string viewAnchorId: ""
                property real viewAnchorOffset: 0
                function captureViewAnchor() {
                    if (stickToBottom || count === 0) {
                        viewAnchorId = ""
                        return
                    }
                    var row = indexAt(width / 2, contentY + topMargin + 1)
                    if (row < 0) {
                        viewAnchorId = ""
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
                    // User-driven motion owns contentY; recapture happens on
                    // settle. The pagination prepend path owns re-anchoring
                    // while its own capture is pending.
                    if (moving || wheelAnimating)
                        return
                    if (app.pagination.busy || anchorStableId !== "")
                        return
                    var row = app.timeline.rowForStableId(viewAnchorId)
                    if (row < 0)
                        return
                    var it = itemAtIndex(row)
                    if (!it)
                        return
                    var desired = it.y + viewAnchorOffset
                    var lo = wheelMinY()
                    var hi = wheelMaxY()
                    desired = desired < lo ? lo : (desired > hi ? hi : desired)
                    if (Math.abs(contentY - desired) > 0.5)
                        contentY = desired
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
                    stickToBottom = atYEnd
                                    || (contentY + height >= contentHeight - 40)
                    if (contentY < height * 0.5 && !stickToBottom)
                        app.pagination.requestNearTop()
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
                        scrollSettleTimer.restart()
                    }
                }

                // Save the settled position once input stops, rather than
                // hundreds of intermediate anchors mid-scroll.
                Timer {
                    id: scrollSettleTimer
                    interval: 250
                    onTriggered: {
                        timeline.updateStickAndPaginate()
                        timeline.saveRoomPosition()
                        timeline.captureViewAnchor()
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

                // v0.5.11: scroll-anchor preservation across a backward
                // prepend. When older events are inserted at the top, a fixed
                // contentY would make the whole conversation jump. We record
                // the first visible event's stable id and its pixel offset
                // when a request starts, then re-align to it once the prepend
                // lands (falling back to a content-height delta if the anchor
                // scrolled out of the created range).
                property string anchorStableId: ""
                property real anchorOffset: 0
                property real anchorContentHeight: 0

                function captureAnchor() {
                    var row = indexAt(width / 2, contentY + topMargin + 1)
                    if (row < 0) { anchorStableId = ""; return }
                    var it = itemAtIndex(row)
                    // Room-activity rows may collapse during a presentation
                    // toggle. Prefer the first loaded non-activity row so the
                    // anchor still has height after either setting value.
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
                    anchorStableId = app.timeline.stableIdAt(row)
                    anchorOffset = it ? (contentY - it.y) : 0
                    anchorContentHeight = contentHeight
                }
                function restoreCapturedAnchor() {
                    if (anchorStableId === "")
                        return false
                    // A programmatic re-anchor must never be finished off by a
                    // lingering wheel animation.
                    cancelWheelMotion()
                    var newRow = app.timeline.rowForStableId(anchorStableId)
                    if (newRow < 0) {
                        anchorStableId = ""
                        return false
                    }
                    positionViewAtIndex(newRow, ListView.Beginning)
                    var it = itemAtIndex(newRow)
                    if (it) contentY = it.y + anchorOffset
                    anchorStableId = ""
                    // The prepend moved every row; the persistent viewport
                    // anchor must re-derive from the restored position.
                    captureViewAnchor()
                    return true
                }
                function restoreAnchor(inserted) {
                    if (inserted <= 0 || anchorStableId === "" || stickToBottom) {
                        anchorStableId = ""
                        return
                    }
                    var newRow = app.timeline.rowForStableId(anchorStableId)
                    if (newRow < 0) {
                        var delta = contentHeight - anchorContentHeight
                        if (delta > 0) contentY += delta
                        anchorStableId = ""
                        captureViewAnchor()
                        return
                    }
                    restoreCapturedAnchor()
                }

                Connections {
                    target: app.pagination
                    property bool wasBusy: false
                    function onStateChanged() {
                        if (app.pagination.busy && !wasBusy
                            && !timeline.stickToBottom)
                            timeline.captureAnchor()
                        if (wasBusy && !app.pagination.busy
                            && app.pagination.failed) {
                            timeline.anchorStableId = ""
                            timeline.anchorOffset = 0
                            timeline.anchorContentHeight = 0
                        }
                        wasBusy = app.pagination.busy
                    }
                    function onPaginationCompleted(inserted, reachedStart) {
                        Qt.callLater(function() { timeline.restoreAnchor(inserted) })
                    }
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
                        timeline.captureAnchor()
                        Qt.callLater(function() {
                            timeline.restoreCapturedAnchor()
                            timeline.maybeFillViewport()
                        })
                    }
                }

                onContentYChanged: {
                    // React to a user drag/flick AND to our own wheel/pixel
                    // motion; ignore programmatic navigation (jump, reply,
                    // restore) which manages follow-latest itself.
                    if (!moving && !wheelAnimating) return
                    stickToBottom = (contentY + height >= contentHeight - 40)
                    // Trigger backfill before hitting the exact top so history
                    // is ready as the user approaches it.
                    if (contentY < height * 0.5 && !stickToBottom)
                        app.pagination.requestNearTop()
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
                    stickToBottom = atYEnd
                                    || (contentY + height >= contentHeight - 40)
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
                        timeline.anchorStableId = ""
                        timeline.anchorOffset = 0
                        timeline.anchorContentHeight = 0
                        timeline.viewAnchorId = ""
                        timeline.viewAnchorOffset = 0
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
                    if (atYBeginning)
                        app.pagination.requestNearTop()
                }

                // v0.5.11: the header shows only transient loading / failure
                // states. The beginning of history is rendered by the virtual
                // "Beginning of conversation" row (eventType 9), so there is no
                // permanent "scroll up" placeholder that lingers when the
                // viewport cannot scroll.
                header: Item {
                    // Exposed for TimelinePaneQmlTest.cpp so the pagination
                    // presentation surface can be located and asserted on
                    // without a fragile visual/coordinate probe.
                    objectName: "paginationHeader"
                    width: timeline.width
                    // PaginationController is the single semantic state
                    // source. Do not mirror this in a local property that can
                    // be re-entered by ListView geometry notifications.
                    height: app.pagination.presentationState
                            === PaginationController.Hidden ? 0 : 32
                    Row {
                        id: paginationHeader
                        anchors.centerIn: parent
                        spacing: 6
                        visible: app.pagination.presentationState
                                 !== PaginationController.Hidden
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
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: app.pagination.presentationState
                                     === PaginationController.Failed
                            text: qsTr("Retry")
                            color: AppTheme.accent
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

            // v0.7.1: Home surface — replaces the bare "select a room"
            // placeholder when nothing is selected. Sits over the (empty,
            // hidden) timeline; the ListView stays present for tests and to
            // resume the selected room instantly.
            HomePane {
                objectName: "homePane"
                anchors.fill: parent
                visible: app.currentRoomId === ""
                onNewMessageRequested: root.newConversationRequested("dm")
                onCreateRoomRequested: root.newConversationRequested("room")
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
}
