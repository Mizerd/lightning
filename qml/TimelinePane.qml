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
        infoPanel.openForRoom(app.currentRoomId, currentRoom)
        infoOpen = true
    }

    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            refreshCurrentRoom()
            // A pinned message-action toolbar belongs to the room it was
            // pinned in; drop it when the room changes.
            timeline.pinnedActionsKey = ""
            // The info panel follows the open room; no room closes it.
            if (root.infoOpen) {
                if (app.currentRoomId === "")
                    root.infoOpen = false
                else
                    infoPanel.openForRoom(app.currentRoomId, root.currentRoom)
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

                // Single read-state gate. Marks the room read only when the
                // user is following the conversation at the bottom of the
                // OPEN room while Lightning is focused. markVisibleAsRead
                // scans backward for the newest real remote event and the
                // receipt is deduped/debounced downstream, so this is safe to
                // call from every trigger (arrival, open, focus, scroll end).
                function maybeMarkRead() {
                    if (Qt.application.state === Qt.ApplicationActive
                            && count > 0 && stickToBottom
                            && !app.timeline.paginating)
                        app.timeline.markVisibleAsRead(0, count - 1)
                }
                onContentYChanged: {
                    if (!moving) return
                    stickToBottom = (contentY + height >= contentHeight - 40)
                }
                onCountChanged: {
                    // A new event arrived (or the timeline reset). Follow the
                    // bottom and re-check read state.
                    if (stickToBottom) Qt.callLater(scrollToEndDeferred)
                    maybeMarkRead()
                }
                onMovementEnded: {
                    // Scrolling settled: recompute whether we are at the
                    // bottom, then re-check read state (return-to-bottom must
                    // clear unread).
                    stickToBottom = atYEnd
                                    || (contentY + height >= contentHeight - 40)
                    maybeMarkRead()
                }
                Component.onCompleted: {
                    Qt.callLater(scrollToEndDeferred)
                    maybeMarkRead()
                }
                // The window regaining focus must re-check read state.
                Connections {
                    target: Qt.application
                    function onStateChanged() { timeline.maybeMarkRead() }
                }
                // A room switch / fresh timeline snapshot opens at the bottom:
                // reset stickToBottom (it may have been left false after
                // scrolling up in the previous room) and re-check read state
                // once the model has settled.
                Connections {
                    target: app.timeline
                    function onModelReset() {
                        timeline.stickToBottom = true
                        timeline.pinnedActionsKey = ""
                        timeline.emojiPickerOpen = false
                        Qt.callLater(timeline.scrollToEndDeferred)
                        Qt.callLater(timeline.maybeMarkRead)
                    }
                }

                // Pagination trigger: scroll to top with backfill available.
                onAtYBeginningChanged: {
                    if (atYBeginning && app.timeline.canPaginate && !app.timeline.paginating)
                        app.timeline.requestOlder()
                }

                header: Item {
                    width: timeline.width
                    height: paginationHeader.visible ? paginationHeader.implicitHeight + 8
                                                     : 0
                    Row {
                        id: paginationHeader
                        anchors.centerIn: parent
                        spacing: 6
                        // v0.5.7: hidden entirely once the start of history
                        // is reached (no permanent placeholder).
                        visible: app.currentRoomId !== ""
                                 && (app.timeline.canPaginate
                                     || app.timeline.paginating
                                     || app.timeline.paginationFailed)
                        BusyIndicator {
                            width: 16; height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            running: app.timeline.paginating
                            visible: app.timeline.paginating
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: app.timeline.paginating
                                  ? qsTr("Loading older messages…")
                                  : (app.timeline.paginationFailed
                                     ? qsTr("Could not load older messages —")
                                     : qsTr("Scroll up to load more history"))
                            color: app.timeline.paginationFailed
                                   ? AppTheme.error : AppTheme.textMuted
                            font.pixelSize: 11
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: app.timeline.paginationFailed
                                     && !app.timeline.paginating
                            text: qsTr("Retry")
                            color: AppTheme.accent
                            font.pixelSize: 11
                            font.underline: true
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: app.timeline.requestOlder()
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
