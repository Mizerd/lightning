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
                 && (root.infoOpen || timeline.pinnedActionsKey !== "")
        onActivated: {
            if (root.infoOpen)
                root.infoOpen = false
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
                spacing: AppTheme.spacingS
                model: app.timeline
                verticalLayoutDirection: ListView.TopToBottom
                topMargin: AppTheme.spacingM
                bottomMargin: AppTheme.spacingM
                leftMargin: AppTheme.spacingM
                rightMargin: AppTheme.spacingM

                delegate: MessageDelegate {
                    width: ListView.view.width - AppTheme.spacingM * 2
                }

                // Auto-scroll to end on new events when already near the bottom.
                property bool stickToBottom: true

                // Which message currently has its action toolbar pinned open
                // (by a click). Shared across delegates so only one can be
                // pinned at a time; keyed by the SDK item id (or event id).
                property string pinnedActionsKey: ""
                property bool emojiPickerOpen: false
                // v0.5.11: whether the open room is encrypted — drives the
                // link-preview privacy gate in each MessageDelegate.
                property bool roomEncrypted: root.currentRoom.encrypted === true
                property var expandedStateGroups: ({})
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

                // v0.5.11: ask the pagination controller for one more batch
                // whenever the loaded content cannot fill the viewport (a
                // short initial snapshot never scrolls, so a scroll-position
                // trigger alone would deadlock). The controller enforces the
                // fill budget, no-progress stop and single-flight, so calling
                // this from every size change is safe.
                function maybeFillViewport() {
                    if (app.currentRoomId !== "" && contentHeight < height)
                        app.pagination.requestViewportFill()
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
                    anchorStableId = app.timeline.stableIdAt(row)
                    anchorOffset = it ? (contentY - it.y) : 0
                    anchorContentHeight = contentHeight
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
                    positionViewAtIndex(newRow, ListView.Beginning)
                    var it = itemAtIndex(newRow)
                    if (it) contentY = it.y + anchorOffset
                    anchorStableId = ""
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
                }

                onContentYChanged: {
                    if (!moving) return
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
                        timeline.stickToBottom = true
                        timeline.pinnedActionsKey = ""
                        timeline.emojiPickerOpen = false
                        timeline.anchorStableId = ""
                        timeline.anchorOffset = 0
                        timeline.anchorContentHeight = 0
                        timeline.expandedStateGroups = ({})
                        Qt.callLater(timeline.scrollToEndDeferred)
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
                    readonly property int paginationState:
                        app.pagination.presentationState
                    height: paginationState === PaginationController.Hidden ? 0 : 32
                    Row {
                        id: paginationHeader
                        anchors.centerIn: parent
                        spacing: 6
                        visible: parent.paginationState !== PaginationController.Hidden
                        BusyIndicator {
                            width: 16; height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            running: parent.parent.paginationState === PaginationController.Loading
                            visible: running
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.parent.paginationState === PaginationController.Loading
                                  ? qsTr("Loading older messages…")
                                  : qsTr("Could not load older messages —")
                            color: parent.parent.paginationState === PaginationController.Failed
                                   ? AppTheme.error : AppTheme.textMuted
                            font.pixelSize: 11
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: parent.parent.paginationState === PaginationController.Failed
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
