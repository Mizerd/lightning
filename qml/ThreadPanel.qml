import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// v0.6.0: the thread side panel. Backed entirely by app.thread
// (ThreadController): the pinned root header renders rootInfo(), the reply
// list is an ordinary ListView over app.thread.model (the SDK thread
// timeline under its composite id) reusing MessageDelegate, and the
// composer sends through the SDK thread path only. No Matrix protocol
// logic lives here.
Rectangle {
    id: panel
    color: AppTheme.background

    signal closeRequested()
    // Media entry points supplied by TimelinePane (shared image viewer and
    // Save As dialog).
    property var openImage: function(mediaKey, httpUrl) {}
    property var saveMedia: function(mediaKey, filename) {}

    property var rootData: ({})
    function refreshRoot() { rootData = app.thread.rootInfo() }

    // v0.6.0 checkpoint 5: per-thread scroll restoration (session-local).
    // Positions are keyed by the composite thread timeline id and saved when
    // scrolling settles; reopening the same thread restores the position
    // instead of always jumping to the end.
    property var savedScrollPositions: ({})
    function saveThreadScrollPosition() {
        var key = app.thread.model.roomId
        if (key === "" || !app.thread.active)
            return
        var positions = savedScrollPositions
        positions[key] = { contentY: replyList.contentY,
                           follow: replyList.followLatest }
        savedScrollPositions = positions
    }
    function restoreThreadScrollPosition() {
        var key = app.thread.model.roomId
        var saved = savedScrollPositions[key]
        if (saved === undefined) {
            replyList.followLatest = true
            replyList.scrollToEndDeferredIfFollowing()
            return
        }
        replyList.followLatest = saved.follow
        if (saved.follow) {
            replyList.scrollToEndDeferredIfFollowing()
        } else {
            Qt.callLater(function() {
                var lo = replyList.originY
                var hi = Math.max(lo, replyList.originY
                                      + replyList.contentHeight
                                      - replyList.height)
                replyList.contentY =
                    Math.max(lo, Math.min(hi, saved.contentY))
            })
        }
    }

    Connections {
        target: app.thread
        function onStateChanged() {
            panel.refreshRoot()
            if (app.thread.state === ThreadController.Ready)
                panel.restoreThreadScrollPosition()
        }
    }
    Connections {
        target: app.thread.model
        function onCountChanged() {
            panel.refreshRoot()
            replyList.scrollToEndDeferredIfFollowing()
        }
    }
    Component.onCompleted: refreshRoot()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ───────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: threadHeader.implicitHeight + AppTheme.spacingM * 2
            color: AppTheme.surface
            RowLayout {
                id: threadHeader
                anchors.fill: parent
                anchors.margins: AppTheme.spacingM
                spacing: AppTheme.spacingS
                // Back to the Threads list when the panel was entered from it.
                ToolButton {
                    objectName: "threadBackToListButton"
                    visible: app.thread.active && app.thread.listOpen
                    text: "‹"
                    font.pixelSize: 14
                    Accessible.name: qsTr("Back to threads")
                    onClicked: app.thread.close()
                }
                Label {
                    text: app.thread.active ? qsTr("Thread") : qsTr("Threads")
                    color: AppTheme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: app.thread.active && app.thread.model.count > 1
                          ? qsTr("%n reply(s)", "", app.thread.model.count - 1)
                          : ""
                    color: AppTheme.textMuted
                    font.pixelSize: 11
                    elide: Label.ElideRight
                }
                // v0.6.0 checkpoint 5: MSC4306 follow state. Hidden until the
                // homeserver confirms support — never a pretend toggle.
                ToolButton {
                    objectName: "threadFollowButton"
                    visible: app.thread.active && app.thread.followSupported
                    enabled: !app.thread.followBusy
                    text: app.thread.followed ? qsTr("Unfollow") : qsTr("Follow")
                    font.pixelSize: 11
                    ToolTip.text: app.thread.followed
                                  ? qsTr("Stop following this thread")
                                  : qsTr("Follow this thread")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: app.thread.setFollowed(!app.thread.followed)
                }
                ToolButton {
                    objectName: "threadCloseButton"
                    text: "✕"
                    font.pixelSize: 13
                    Accessible.name: qsTr("Close thread")
                    ToolTip.text: qsTr("Close thread")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: panel.closeRequested()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // ── Threads list (Threads view mode) ─────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !app.thread.active && app.thread.listOpen

            Column {
                anchors.centerIn: parent
                spacing: AppTheme.spacingS
                visible: app.thread.listLoading && threadListView.count === 0
                BusyIndicator {
                    width: 22; height: 22
                    running: visible
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Label {
                    text: qsTr("Loading threads…")
                    color: AppTheme.textMuted
                    font.pixelSize: 11
                }
            }
            Label {
                anchors.centerIn: parent
                visible: !app.thread.listLoading && threadListView.count === 0
                text: app.thread.listFailed
                      ? qsTr("Threads could not be loaded.")
                      : qsTr("No threads in this room yet.")
                color: AppTheme.textMuted
                font.pixelSize: 11
            }

            ListView {
                id: threadListView
                objectName: "threadListView"
                anchors.fill: parent
                clip: true
                model: app.thread.threadList
                spacing: 1
                delegate: Rectangle {
                    width: threadListView.width
                    color: rowHover.hovered ? AppTheme.surface : "transparent"
                    implicitHeight: listEntry.implicitHeight
                                    + AppTheme.spacingS * 2
                    HoverHandler { id: rowHover }
                    TapHandler {
                        onTapped: app.thread.openThread(
                            app.currentRoomId, modelData.rootEventId || "")
                    }
                    ColumnLayout {
                        id: listEntry
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: AppTheme.spacingM
                        anchors.rightMargin: AppTheme.spacingM
                        spacing: 2
                        RowLayout {
                            spacing: AppTheme.spacingS
                            Rectangle {
                                visible: modelData.unread === true
                                width: 7; height: 7; radius: 3.5
                                color: AppTheme.accent
                            }
                            Label {
                                text: modelData.rootSenderName || ""
                                color: AppTheme.text
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: modelData.latestTimestamp
                                      ? Qt.formatDateTime(
                                            modelData.latestTimestamp,
                                            "d MMM hh:mm")
                                      : ""
                                color: AppTheme.textMuted
                                font.pixelSize: 9
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.rootPreview || ""
                            color: AppTheme.text
                            font.pixelSize: 11
                            elide: Label.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("%n reply(s)", "",
                                       modelData.replyCount || 0)
                                  + ((modelData.latestPreview || "").length > 0
                                     ? " · " + (modelData.latestSenderName || "")
                                       + ": " + modelData.latestPreview
                                     : "")
                            color: AppTheme.textMuted
                            font.pixelSize: 10
                            elide: Label.ElideRight
                        }
                    }
                }
                footer: Item {
                    width: threadListView.width
                    height: !app.thread.listEndReached ? 30 : 0
                    Label {
                        anchors.centerIn: parent
                        visible: !app.thread.listEndReached
                        text: app.thread.listLoading
                              ? qsTr("Loading…") : qsTr("Load more threads")
                        color: AppTheme.accent
                        font.pixelSize: 10
                        font.underline: !app.thread.listLoading
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: app.thread.paginateList()
                        }
                    }
                }
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            }
        }

        // ── Pinned root ──────────────────────────────────────────────────
        Rectangle {
            objectName: "threadRootHeader"
            Layout.fillWidth: true
            visible: app.thread.active
                     && app.thread.state !== ThreadController.Failed
            implicitHeight: Math.min(rootColumn.implicitHeight
                                     + AppTheme.spacingM * 2, 180)
            clip: true
            color: AppTheme.surface
            ColumnLayout {
                id: rootColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: AppTheme.spacingM
                spacing: 4
                RowLayout {
                    spacing: AppTheme.spacingS
                    visible: panel.rootData.loaded === true
                    Avatar {
                        size: 24
                        name: panel.rootData.senderDisplayName || ""
                        mxc: panel.rootData.senderAvatarMxc || ""
                    }
                    Label {
                        text: panel.rootData.senderDisplayName || ""
                        color: AppTheme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: panel.rootData.timestamp
                              ? Qt.formatDateTime(panel.rootData.timestamp,
                                                  "d MMM hh:mm")
                              : ""
                        color: AppTheme.textMuted
                        font.pixelSize: 10
                    }
                    Label {
                        visible: panel.rootData.isEncrypted === true
                        text: "\u{1F512}"
                        font.pixelSize: 10
                        color: AppTheme.textMuted
                    }
                    Item { Layout.fillWidth: true }
                    // Root context action: locate the root in the room
                    // timeline (highlighted), without leaving the room.
                    Label {
                        text: qsTr("Open in room")
                        color: AppTheme.accent
                        font.pixelSize: 10
                        font.underline: true
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: app.pagination.jumpToEvent(
                                panel.rootData.eventId || "")
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: panel.rootData.loaded === true
                    text: panel.rootData.redacted === true
                          ? qsTr("Message deleted")
                          : panel.rootData.undecryptable === true
                            ? qsTr("Unable to decrypt this message")
                            : (panel.rootData.body || "")
                    font.italic: panel.rootData.redacted === true
                                 || panel.rootData.undecryptable === true
                    color: (panel.rootData.redacted === true
                            || panel.rootData.undecryptable === true)
                           ? AppTheme.textMuted : AppTheme.text
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    maximumLineCount: 6
                    elide: Text.ElideRight
                }
                Label {
                    visible: panel.rootData.loaded !== true
                    text: qsTr("The original message is unavailable.")
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                    font.italic: true
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // ── Body states + reply list ─────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Opening / loading state.
            Column {
                anchors.centerIn: parent
                spacing: AppTheme.spacingS
                visible: app.thread.state === ThreadController.Opening
                BusyIndicator {
                    width: 22; height: 22
                    running: visible
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Label {
                    text: qsTr("Loading thread…")
                    color: AppTheme.textMuted
                    font.pixelSize: 11
                }
            }

            // Failed state (unknown root, network, redacted-away root).
            Column {
                anchors.centerIn: parent
                spacing: AppTheme.spacingS
                visible: app.thread.state === ThreadController.Failed
                width: parent.width - AppTheme.spacingM * 4
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: app.thread.failureCategory === "unknown_root"
                          ? qsTr("This thread's first message is no longer available.")
                          : qsTr("The thread could not be loaded.")
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Retry")
                    color: AppTheme.accent
                    font.pixelSize: 12
                    font.underline: true
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.thread.openThread(app.thread.roomId,
                                                         app.thread.rootEventId)
                    }
                }
            }

            // Empty state: thread is ready but only the root is loaded.
            Label {
                anchors.centerIn: parent
                visible: app.thread.state === ThreadController.Ready
                         && replyList.count <= 1
                text: qsTr("No replies yet. Start the conversation below.")
                color: AppTheme.textMuted
                font.pixelSize: 11
            }

            ListView {
                id: replyList
                objectName: "threadReplyList"
                anchors.fill: parent
                visible: app.thread.state === ThreadController.Ready
                clip: true
                spacing: 0
                model: app.thread.model
                topMargin: AppTheme.spacingS
                bottomMargin: AppTheme.spacingS
                leftMargin: AppTheme.spacingS
                rightMargin: AppTheme.spacingS

                // MessageDelegate view contract (shared with the room
                // timeline ListView in TimelinePane.qml).
                property var timelineModel: app.thread.model
                property string suppressRootEventId: app.thread.rootEventId
                property bool threadContext: true
                property string pinnedActionsKey: ""
                property bool emojiPickerOpen: false
                property bool roomEncrypted: {
                    var info = app.roomList.findRoom(app.thread.roomId)
                    return info && info.encrypted === true
                }
                function stateGroupExpanded(groupId) { return true }
                function toggleStateGroup(groupId) {}
                property var openImage: panel.openImage
                property var saveMedia: panel.saveMedia

                property bool followLatest: true
                function scrollToEndDeferredIfFollowing() {
                    if (followLatest && count > 0) {
                        Qt.callLater(function() { replyList.positionViewAtEnd() })
                        // v0.6.0 checkpoint 5: reading the latest reply sends
                        // ONE deduplicated THREADED receipt (never room-wide).
                        app.thread.markRead()
                    }
                }
                onMovementEnded: {
                    followLatest = atYEnd
                                   || (contentY + height >= contentHeight - 40)
                    if (followLatest)
                        app.thread.markRead()
                    // Near-top backfill for long threads; the model's
                    // requestOlder() is single-flight in the backend.
                    if (contentY - originY < height * 0.5)
                        app.thread.model.requestOlder()
                    panel.saveThreadScrollPosition()
                }
                Component.onCompleted: scrollToEndDeferredIfFollowing()

                delegate: MessageDelegate {
                    width: {
                        var available = ListView.view
                                      ? ListView.view.width
                                        - AppTheme.spacingS * 2 : 0
                        return available > 0 ? available : 320
                    }
                }

                header: Item {
                    width: replyList.width
                    height: app.thread.model.paginating ? 28 : 0
                    Row {
                        anchors.centerIn: parent
                        spacing: 6
                        visible: app.thread.model.paginating
                        BusyIndicator {
                            width: 14; height: 14
                            running: visible
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Loading older replies…")
                            color: AppTheme.textMuted
                            font.pixelSize: 10
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // Reply-within-thread banner (checkpoint 4): shows the active reply
        // target; ✕ or Escape cancels it without closing the panel.
        Rectangle {
            objectName: "threadReplyBanner"
            Layout.fillWidth: true
            visible: app.thread.inReply
            color: AppTheme.surface
            implicitHeight: visible
                            ? replyBannerRow.implicitHeight + AppTheme.spacingS
                            : 0
            RowLayout {
                id: replyBannerRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: AppTheme.spacingM
                anchors.rightMargin: AppTheme.spacingS
                spacing: AppTheme.spacingS
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Replying to %1: %2")
                          .arg(app.thread.replyToSender)
                          .arg(app.thread.replyToPreview)
                    color: AppTheme.textMuted
                    font.pixelSize: 10
                    elide: Label.ElideRight
                }
                ToolButton {
                    objectName: "threadReplyCancelButton"
                    text: "✕"
                    font.pixelSize: 10
                    Accessible.name: qsTr("Cancel reply")
                    onClicked: app.thread.cancelReply()
                }
            }
        }

        // ── Thread composer ──────────────────────────────────────────────
        // Sends ONLY through ThreadController.sendText → the backend's SDK
        // m.thread path. The main room composer is untouched.
        Rectangle {
            Layout.fillWidth: true
            visible: app.thread.active
            color: AppTheme.surface
            implicitHeight: composerRow.implicitHeight + AppTheme.spacingS * 2
            RowLayout {
                id: composerRow
                anchors.fill: parent
                anchors.margins: AppTheme.spacingS
                spacing: AppTheme.spacingS
                ToolButton {
                    text: "🙂"
                    font.pixelSize: 13
                    enabled: app.thread.state === ThreadController.Ready
                    Accessible.name: qsTr("Insert emoji")
                    onClicked: {
                        var p = mapToItem(panel, 0, 0)
                        threadEmojiPicker.anchorPoint = Qt.point(p.x, p.y)
                        threadEmojiPicker.open()
                    }
                }
                TextArea {
                    id: threadComposerInput
                    objectName: "threadComposerInput"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Reply in thread…")
                    wrapMode: TextArea.Wrap
                    enabled: app.thread.state === ThreadController.Ready
                    background: Rectangle {
                        color: AppTheme.background
                        border.color: threadComposerInput.activeFocus
                                      ? AppTheme.accent : AppTheme.border
                        radius: AppTheme.radiusSm
                    }
                    color: AppTheme.text
                    font.pixelSize: 12
                    Keys.onPressed: (event) => {
                        if ((event.key === Qt.Key_Return
                             || event.key === Qt.Key_Enter)
                            && !(event.modifiers & Qt.ShiftModifier)) {
                            panel.sendComposerText()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape) {
                            if (app.thread.inReply)
                                app.thread.cancelReply()
                            else
                                panel.closeRequested()
                            event.accepted = true
                        }
                    }
                }
                Button {
                    objectName: "threadSendButton"
                    text: qsTr("Send")
                    enabled: app.thread.state === ThreadController.Ready
                             && threadComposerInput.text.trim().length > 0
                    onClicked: panel.sendComposerText()
                }
            }
        }
    }

    EmojiPicker {
        id: threadEmojiPicker
        mode: "composer"
        onEmojiChosen: (emoji) => {
            threadComposerInput.insert(threadComposerInput.cursorPosition,
                                       emoji)
        }
        onClosed: Qt.callLater(threadComposerInput.forceActiveFocus)
    }

    function sendComposerText() {
        var body = threadComposerInput.text.trim()
        if (body.length === 0 || app.thread.state !== ThreadController.Ready)
            return
        app.thread.sendText(body)
        threadComposerInput.text = ""
        replyList.followLatest = true
        replyList.scrollToEndDeferredIfFollowing()
    }
}
