import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

Rectangle {
    id: root
    color: AppTheme.background

    property var currentRoom: ({})
    // v0.5.9: Room Information side panel (Phases 6/10 surface).
    property bool infoOpen: false
    // v0.6.0: the thread side surface is open when a thread panel OR the
    // room's Threads list view is showing.
    readonly property bool threadSurfaceOpen: app.thread.active
                                              || app.thread.listOpen

    function refreshCurrentRoom() {
        currentRoom = app.currentRoomId === ""
                      ? ({})
                      : app.roomList.findRoom(app.currentRoomId)
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

    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            refreshCurrentRoom()
            // Old-room wheel motion must not continue into the new room.
            timeline.cancelWheelMotion()
            // A pinned message-action toolbar belongs to the room it was
            // pinned in; drop it when the room changes.
            timeline.pinnedActionsKey = ""
            timeline.anchorStableId = ""
            timeline.anchorOffset = 0
            timeline.anchorContentHeight = 0
            // The info panel follows the open room; no room closes it.
            if (root.infoOpen) {
                if (app.currentRoomId === "")
                    root.infoOpen = false
                else
                    infoPanel.openForRoom(app.currentRoomId)
            }
        }
    }

    // Escape closes the room info panel first, then any pinned toolbar.
    Shortcut {
        sequence: "Escape"
        enabled: !timeline.emojiPickerOpen
                 && (root.infoOpen || root.threadSurfaceOpen
                     || timeline.pinnedActionsKey !== "")
        onActivated: {
            if (root.infoOpen)
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
        // v0.6.0: on narrow windows the open thread surface takes the whole
        // pane (the room and its state stay alive underneath and return
        // when the panel closes or the window widens).
        visible: !(root.threadSurfaceOpen && root.width < 900)
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 320
        spacing: 0

        // Room header
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: header.implicitHeight + AppTheme.spacingM * 2
            color: AppTheme.surface
            RowLayout {
                id: header
                anchors.fill: parent
                anchors.margins: AppTheme.spacingM
                spacing: AppTheme.spacingS
                Avatar {
                    visible: app.currentRoomId !== ""
                    size: 36
                    name: root.currentRoom.name || app.currentRoomId
                    mxc: root.currentRoom.avatarUrl || ""
                    circle: !(root.currentRoom.isSpace === true)
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
                                     ? qsTr("No room selected")
                                     : app.currentRoomId)
                            color: AppTheme.text
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        Label {
                            visible: root.currentRoom.encrypted === true
                            text: "\u{1F512}"
                            color: AppTheme.textMuted
                            font.pixelSize: 12
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
                ToolButton {
                    objectName: "threadsViewButton"
                    visible: app.currentRoomId !== "" && app.thread.supported
                    text: "🧵"
                    font.pixelSize: 14
                    checked: app.thread.listOpen
                    Accessible.name: qsTr("Threads")
                    ToolTip.text: qsTr("Threads in this room")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: {
                        if (app.thread.listOpen && !app.thread.active) {
                            app.thread.closeList()
                        } else {
                            app.thread.close()
                            app.thread.openList(app.currentRoomId)
                        }
                    }
                }
                ToolButton {
                    visible: app.currentRoomId !== "" && app.roomInfo.supported
                    text: "ⓘ"
                    font.pixelSize: 16
                    checked: root.infoOpen
                    Accessible.name: qsTr("Room information")
                    ToolTip.text: qsTr("Room information")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: root.toggleRoomInfo()
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // Timeline
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: timeline
                objectName: "timelineListView"
                anchors.fill: parent
                clip: true
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
                    if (count > 0 && stickToBottom)
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
                    value: timeline.visible && app.currentRoomId !== ""
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
                    value: timeline.visible && timeline.Window.active === true
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
                onContentHeightChanged: maybeFillViewport()
                onHeightChanged: maybeFillViewport()

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
                    if (stickToBottom) Qt.callLater(scrollToEndDeferred)
                }
                onMovementEnded: {
                    // Scrolling settled: recompute whether we are at the
                    // bottom (return-to-bottom must clear unread — the
                    // coordinator reacts to the nearBottom binding).
                    stickToBottom = atYEnd
                                    || (contentY + height >= contentHeight - 40)
                    saveRoomPosition()
                }
                Component.onCompleted: {
                    Qt.callLater(scrollToEndDeferred)
                    maybeFillViewport()
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
                        timeline.expandedStateGroups = ({})
                        Qt.callLater(function() {
                            app.pagination.restoreScrollAnchor(app.currentRoomId)
                        })
                        Qt.callLater(timeline.maybeFillViewport)
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
                    visible: timeline.count === 0
                    text: app.currentRoomId === ""
                          ? qsTr("Select a room from the left")
                          : qsTr("No messages yet")
                    color: AppTheme.textMuted
                }
            }

            Button {
                id: jumpToLatestButton
                objectName: "jumpToLatestButton"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: AppTheme.spacingM + 12
                anchors.bottomMargin: AppTheme.spacingM + 8
                visible: app.currentRoomId !== "" && !timeline.stickToBottom
                text: qsTr("Jump to latest")
                z: 20
                focusPolicy: Qt.StrongFocus
                Accessible.name: text
                ToolTip.text: qsTr("Return to the newest message")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                // Shares goToLatest() with the End key — both cancel any wheel
                // animation, resume follow-latest, and re-evaluate read state.
                onClicked: timeline.goToLatest()
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
            color: AppTheme.surface
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
            color: AppTheme.surface
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

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        MessageComposerBar { Layout.fillWidth: true }
    }

    // ── Thread side panel ────────────────────────────────────────────────
    Rectangle {
        visible: root.threadSurfaceOpen && root.width >= 900
        Layout.fillHeight: true
        implicitWidth: 1
        color: AppTheme.border
    }
    ThreadPanel {
        id: threadPanel
        objectName: "threadPanel"
        visible: root.threadSurfaceOpen
        Layout.fillHeight: true
        Layout.preferredWidth: root.width >= 900 ? 360 : root.width
        Layout.fillWidth: root.threadSurfaceOpen && root.width < 900
        onCloseRequested: {
            if (app.thread.active)
                app.thread.close()
            else
                app.thread.closeList()
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
