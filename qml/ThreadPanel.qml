import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// The thread side panel (correction spec §4): a 340px right-side surface
// that replaces the member panel — never the room timeline. 58px header
// (accent forum glyph, "Thread", room name, bare close X), the root event
// in a raised bordered card, an "N replies" hairline divider, plain reply
// rows, and a single-row mini composer pinned at the bottom. Backed
// entirely by app.thread (ThreadController): the reply list is the SDK
// thread timeline reusing MessageDelegate, and the composer sends through
// the SDK thread path only. No Matrix protocol logic lives here.
Rectangle {
    id: panel
    color: AppTheme.sidebar

    signal closeRequested()
    // Media entry points supplied by TimelinePane (shared image viewer and
    // Save As dialog).
    property var openImage: function(mediaKey, httpUrl) {}
    property var saveMedia: function(mediaKey, filename) {}

    property var rootData: ({})
    function refreshRoot() { rootData = app.thread.rootInfo() }

    readonly property string panelRoomId:
        app.thread.active ? app.thread.roomId : app.currentRoomId
    readonly property string panelRoomName: {
        var room = app.roomList.findRoom(panelRoomId)
        return room && room.name ? room.name : ""
    }

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
            // Any lifecycle transition invalidates in-flight wheel motion —
            // old-thread motion must never scroll the new thread.
            app.threadScroll.cancel()
            if (app.thread.state === ThreadController.Ready) {
                panel.restoreThreadScrollPosition()
                Qt.callLater(function() {
                    if (app.thread.active)
                        threadComposerInput.forceActiveFocus()
                })
            }
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

        // ── Header (58px): forum glyph · Thread · room name · close ──────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 58
            color: panel.color
            RowLayout {
                id: threadHeader
                anchors.fill: parent
                anchors.leftMargin: AppTheme.spacing16
                anchors.rightMargin: AppTheme.spacing16
                spacing: AppTheme.spacing8
                // Back to the Threads list when the panel was entered from it
                // (panel-internal navigation, not a route).
                IconButton {
                    objectName: "threadBackToListButton"
                    visible: app.thread.active && app.thread.listOpen
                    implicitWidth: 28; implicitHeight: 28
                    radius: 6
                    iconName: "arrow_back"
                    iconSize: 16
                    Accessible.name: qsTr("Back to threads")
                    onClicked: app.thread.close()
                }
                Icon {
                    name: "forum"
                    size: 20
                    color: AppTheme.accent
                }
                Label {
                    text: app.thread.active ? qsTr("Thread") : qsTr("Threads")
                    color: AppTheme.text
                    font.pixelSize: 15
                    font.weight: Font.ExtraBold
                }
                Label {
                    Layout.fillWidth: true
                    text: panel.panelRoomName
                    color: AppTheme.textMuted
                    font.pixelSize: 12
                    elide: Label.ElideRight
                }
                // v0.6.0 checkpoint 5: MSC4306 follow state. Hidden until the
                // homeserver confirms support — never a pretend toggle.
                AbstractButton {
                    id: followButton
                    objectName: "threadFollowButton"
                    visible: app.thread.active && app.thread.followSupported
                    enabled: !app.thread.followBusy
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    implicitWidth: followLabel.implicitWidth + 12
                    implicitHeight: 24
                    Accessible.role: Accessible.Button
                    Accessible.name: followLabel.text
                    ToolTip.text: app.thread.followed
                                  ? qsTr("Stop following this thread")
                                  : qsTr("Follow this thread")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: app.thread.setFollowed(!app.thread.followed)
                    contentItem: Label {
                        id: followLabel
                        text: app.thread.followed ? qsTr("Unfollow")
                                                  : qsTr("Follow")
                        color: followButton.enabled ? AppTheme.accent
                                                    : AppTheme.textDisabled
                        font.pixelSize: 11
                        font.weight: Font.Bold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 6
                        color: followButton.hovered ? AppTheme.hover
                                                    : "transparent"
                    }
                }
                IconButton {
                    objectName: "threadCloseButton"
                    implicitWidth: 30; implicitHeight: 30
                    radius: 7
                    iconName: "close"
                    iconSize: 20
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
                    color: rowHover.hovered ? AppTheme.hover : "transparent"
                    radius: AppTheme.radiusMd
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

        // ── Pinned root: raised, bordered card (12px pad, radius 10) ─────
        Rectangle {
            objectName: "threadRootHeader"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12
            Layout.rightMargin: AppTheme.spacing12
            Layout.topMargin: AppTheme.spacing12
            visible: app.thread.active
                     && app.thread.state !== ThreadController.Failed
            implicitHeight: Math.min(rootColumn.implicitHeight
                                     + AppTheme.spacing12 * 2, 190)
            clip: true
            radius: 10
            border.color: AppTheme.border
            border.width: 1
            color: AppTheme.surface
            ColumnLayout {
                id: rootColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: AppTheme.spacing12
                spacing: 4
                RowLayout {
                    spacing: AppTheme.spacingS
                    visible: panel.rootData.loaded === true
                    Avatar {
                        size: 32
                        name: panel.rootData.senderDisplayName || ""
                        mxc: panel.rootData.senderAvatarMxc || ""
                    }
                    Label {
                        text: panel.rootData.senderDisplayName || ""
                        color: AppTheme.text
                        font.pixelSize: AppTheme.scaled(13)
                        font.weight: Font.ExtraBold
                    }
                    Label {
                        text: panel.rootData.timestamp
                              ? Qt.formatDateTime(panel.rootData.timestamp,
                                                  "d MMM hh:mm")
                              : ""
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                    }
                    Icon {
                        visible: panel.rootData.isEncrypted === true
                        name: "lock"
                        size: 12
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
                    font.pixelSize: AppTheme.scaled(13)
                    lineHeight: 1.4
                    wrapMode: Text.Wrap
                    maximumLineCount: 6
                    elide: Text.ElideRight
                }
                Label {
                    visible: panel.rootData.loaded !== true
                    text: qsTr("The original message is unavailable.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(12)
                    font.italic: true
                }
            }
        }

        // ── "N replies" divider: hairline · label · hairline ─────────────
        RowLayout {
            objectName: "threadReplyDivider"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing4
            visible: app.thread.active
                     && app.thread.state === ThreadController.Ready
                     && app.thread.model.count > 1
            spacing: AppTheme.spacing8
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: AppTheme.border
            }
            Label {
                readonly property int replies: app.thread.model.count - 1
                text: replies === 1 ? qsTr("1 reply")
                                    : qsTr("%1 replies").arg(replies)
                color: AppTheme.textMuted
                font.pixelSize: 11
                font.weight: Font.Bold
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: AppTheme.border
            }
        }

        // ── Body states + reply list (plain rows) ────────────────────────
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
                leftMargin: AppTheme.spacing12
                rightMargin: AppTheme.spacing12

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

                // v0.6.0 checkpoint 6: the panel's own wheel motion engine
                // (app.threadScroll) — same device-aware policy as the room
                // timeline, fully isolated motion state. Programmatic
                // positioning (restore, follow-latest) cancels it first.
                function wheelMinY() { return originY - topMargin }
                function wheelMaxY() {
                    var maxY = originY + contentHeight + bottomMargin - height
                    var minY = wheelMinY()
                    return maxY < minY ? minY : maxY
                }
                function afterWheelSettled() {
                    followLatest = atYEnd
                                   || (contentY + height >= contentHeight - 40)
                    if (followLatest)
                        app.thread.markRead()
                    if (contentY - originY < height * 0.5)
                        app.thread.model.requestOlder()
                    panel.saveThreadScrollPosition()
                }
                WheelHandler {
                    objectName: "threadWheelHandler"
                    target: null
                    acceptedDevices: PointerDevice.Mouse
                                     | PointerDevice.TouchPad
                    onWheel: (event) => {
                        var minY = replyList.wheelMinY()
                        var maxY = replyList.wheelMaxY()
                        if (event.pixelDelta.y !== 0) {
                            app.threadScroll.cancel()
                            replyList.contentY = app.threadScroll.pixelTargetY(
                                event.pixelDelta.y, replyList.contentY,
                                minY, maxY)
                            replyList.afterWheelSettled()
                        } else if (event.angleDelta.y !== 0) {
                            app.threadScroll.wheelNotch(
                                event.angleDelta.y, replyList.contentY,
                                minY, maxY, replyList.height)
                            if (event.angleDelta.y > 0)
                                replyList.followLatest = false
                        }
                        event.accepted = true
                    }
                }
                Connections {
                    target: app.threadScroll
                    function onWheelPositionChanged(y) {
                        var lo = replyList.wheelMinY()
                        var hi = replyList.wheelMaxY()
                        var clamped = y < lo ? lo : (y > hi ? hi : y)
                        replyList.contentY = clamped
                        if (clamped !== y)
                            app.threadScroll.notifyBoundReached(clamped)
                    }
                    function onWheelMotionSettled() {
                        replyList.afterWheelSettled()
                    }
                }
                Component.onDestruction: app.threadScroll.cancel()

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
                                        - AppTheme.spacing12 * 2 : 0
                        return available > 0 ? available : 316
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

        // Reply-within-thread banner (checkpoint 4): shows the active reply
        // target; ✕ or Escape cancels it without closing the panel.
        RowLayout {
            objectName: "threadReplyBanner"
            visible: app.thread.inReply
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12 + 4
            Layout.rightMargin: AppTheme.spacing12
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
            IconButton {
                objectName: "threadReplyCancelButton"
                implicitWidth: 20; implicitHeight: 20
                radius: 5
                iconName: "close"
                iconSize: 13
                Accessible.name: qsTr("Cancel reply")
                onClicked: app.thread.cancelReply()
            }
        }

        // ── Mini composer: ONE row in a raised card, pinned at the bottom.
        // Sends ONLY through ThreadController.sendText → the backend's SDK
        // m.thread path (text and attachments). The main room composer and
        // its draft are untouched. ─────────────────────────────────────────
        Item {
            Layout.fillWidth: true
            visible: app.thread.active
            implicitHeight: threadComposerCol.implicitHeight
                            + AppTheme.spacing12

            // Files dropped over the thread composer are queued for the OPEN
            // thread — never rerouted to the room composer.
            DropArea {
                id: threadDropArea
                anchors.fill: parent
                enabled: app.thread.attachmentsSupported
                keys: ["text/uri-list"]
                onDropped: (drop) => {
                    if (!drop.hasUrls) return
                    for (var i = 0; i < drop.urls.length; ++i)
                        app.thread.addAttachment(drop.urls[i])
                    drop.accept(Qt.CopyAction)
                }
            }
            Rectangle {
                anchors.fill: parent
                visible: threadDropArea.containsDrag
                color: "transparent"
                border.color: AppTheme.accent
                border.width: 2
                radius: AppTheme.radiusSm
                z: 10
            }

            ColumnLayout {
                id: threadComposerCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: AppTheme.spacing12
                anchors.rightMargin: AppTheme.spacing12
                anchors.bottomMargin: AppTheme.spacing12
                spacing: AppTheme.spacingXS

                // Attachment validation notice.
                Label {
                    visible: panel.attachmentNotice.length > 0
                    Layout.fillWidth: true
                    text: panel.attachmentNotice
                    color: AppTheme.warning
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                // Attachment tray.
                Flow {
                    visible: app.thread.hasAttachments
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingXS
                    Repeater {
                        model: app.thread.attachments
                        Rectangle {
                            radius: AppTheme.radiusSm
                            color: AppTheme.cardElevated
                            border.color: model.state === "failed"
                                          ? AppTheme.danger : AppTheme.border
                            border.width: 1
                            implicitWidth: Math.min(
                                threadChipRow.implicitWidth
                                + AppTheme.spacingS * 2, 240)
                            implicitHeight: threadChipRow.implicitHeight
                                            + AppTheme.spacingS
                            RowLayout {
                                id: threadChipRow
                                anchors.centerIn: parent
                                spacing: AppTheme.spacingXS
                                Icon {
                                    name: model.isImage ? "image" : "attach_file"
                                    size: 15
                                }
                                ColumnLayout {
                                    spacing: 0
                                    Label {
                                        text: model.fileName
                                        color: AppTheme.text
                                        font.pixelSize: 11
                                        elide: Label.ElideMiddle
                                        Layout.maximumWidth: 120
                                    }
                                    Label {
                                        text: model.state === "failed"
                                              ? (model.error || qsTr("Failed"))
                                              : (model.state === "dispatching"
                                                 ? qsTr("Sending…")
                                                 : model.sizeLabel)
                                        color: model.state === "failed"
                                               ? AppTheme.danger
                                               : AppTheme.textMuted
                                        font.pixelSize: 10
                                        elide: Label.ElideRight
                                        Layout.maximumWidth: 120
                                    }
                                }
                                IconButton {
                                    visible: model.state === "failed"
                                    implicitWidth: 20; implicitHeight: 20
                                    radius: 5
                                    iconName: "refresh"
                                    iconSize: 14
                                    Accessible.name:
                                        qsTr("Retry sending %1").arg(model.fileName)
                                    onClicked: {
                                        app.thread.attachments.retryAt(index)
                                        app.thread.sendText("")
                                    }
                                }
                                IconButton {
                                    enabled: model.state !== "dispatching"
                                    implicitWidth: 20; implicitHeight: 20
                                    radius: 5
                                    iconName: "close"
                                    iconSize: 13
                                    Accessible.name:
                                        qsTr("Remove attachment %1").arg(model.fileName)
                                    onClicked: app.thread.attachments.removeAt(index)
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "threadMiniComposer"
                    Layout.fillWidth: true
                    implicitHeight: composerRow.implicitHeight
                                    + AppTheme.spacing8 * 2
                    radius: 10
                    color: AppTheme.surface
                    border.color: AppTheme.border
                    border.width: 1

                    RowLayout {
                        id: composerRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.rightMargin: AppTheme.spacing8
                        spacing: AppTheme.spacing8

                        IconButton {
                            objectName: "threadAttachButton"
                            implicitWidth: 24; implicitHeight: 24
                            radius: 6
                            iconName: "add_circle"
                            iconSize: 18
                            enabled: app.thread.attachmentsSupported
                            Accessible.name: qsTr("Attach files")
                            ToolTip.text: qsTr("Attach")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: threadAttachDialog.open()
                        }
                        TextArea {
                            id: threadComposerInput
                            objectName: "threadComposerInput"
                            Layout.fillWidth: true
                            Layout.maximumHeight: 110
                            placeholderText: qsTr("Reply in thread")
                            placeholderTextColor: AppTheme.textMuted
                            wrapMode: TextArea.Wrap
                            enabled: app.thread.state === ThreadController.Ready
                            // The card carries the chrome; the field itself
                            // is bare (no inner frame).
                            background: Rectangle { color: "transparent" }
                            color: AppTheme.text
                            font.pixelSize: AppTheme.scaled(13)
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
                                } else if (event.matches(StandardKey.Paste)
                                           && app.thread.pasteFromClipboard()) {
                                    event.accepted = true
                                }
                            }
                        }
                        IconButton {
                            objectName: "threadEmojiButton"
                            implicitWidth: 24; implicitHeight: 24
                            radius: 6
                            iconName: "mood"
                            iconSize: 20
                            enabled: app.thread.state === ThreadController.Ready
                            Accessible.name: qsTr("Insert emoji")
                            onClicked: {
                                var p = mapToItem(panel, 0, 0)
                                threadEmojiPicker.anchorPoint = Qt.point(p.x, p.y)
                                threadEmojiPicker.open()
                            }
                        }
                        // GIF keycap (kept: thread GIF sending is a real
                        // feature; mono text chip per the design language).
                        AbstractButton {
                            id: threadGifButton
                            objectName: "threadGifButton"
                            implicitWidth: threadGifCap.implicitWidth + 8
                            implicitHeight: 24
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            Accessible.role: Accessible.Button
                            enabled: app.thread.state === ThreadController.Ready
                                     && app.gif.available
                            Accessible.name: qsTr("Insert a GIF")
                            ToolTip.text: app.gif.available ? qsTr("GIF")
                                : qsTr("GIFs are unavailable on this backend")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            contentItem: Item {
                                implicitWidth: threadGifCap.implicitWidth
                                implicitHeight: 24
                                Label {
                                    id: threadGifCap
                                    anchors.centerIn: parent
                                    text: qsTr("GIF")
                                    font.family: AppTheme.monoFont
                                    font.pixelSize: 10
                                    font.weight: Font.Bold
                                    leftPadding: 4
                                    rightPadding: 4
                                    topPadding: 2
                                    bottomPadding: 2
                                    color: threadGifButton.enabled
                                           ? AppTheme.icon
                                           : AppTheme.textDisabled
                                }
                            }
                            background: Item {
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: threadGifCap.implicitWidth
                                    height: threadGifCap.implicitHeight
                                    radius: 5
                                    color: threadGifButton.hovered
                                           ? AppTheme.hover : "transparent"
                                    border.color: AppTheme.borderStrong
                                    border.width: 1.5
                                }
                            }
                            onClicked: {
                                threadEmojiPicker.close()
                                var p = threadGifButton.mapToItem(
                                    Overlay.overlay,
                                    threadGifButton.width / 2, 0)
                                threadGifPicker.anchorPoint = p
                                threadGifPicker.open()
                            }
                        }
                        // Accent-fill thread send: 28×28, radius 7, 16px icon.
                        IconButton {
                            objectName: "threadSendButton"
                            implicitWidth: 28; implicitHeight: 28
                            radius: 7
                            fill: true
                            iconName: "send"
                            iconSize: 16
                            enabled: app.thread.state === ThreadController.Ready
                                     && (threadComposerInput.text.trim().length > 0
                                         || app.thread.hasAttachments)
                            Accessible.name: qsTr("Send thread reply")
                            ToolTip.text: qsTr("Send")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: panel.sendComposerText()
                        }
                    }
                }
            }
        }
    }

    // Attachment picker for the thread composer.
    FileDialog {
        id: threadAttachDialog
        title: qsTr("Attach files")
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            for (var i = 0; i < selectedFiles.length; ++i)
                app.thread.addAttachment(selectedFiles[i])
        }
    }

    // Transient attachment validation feedback.
    property string attachmentNotice: ""
    Timer {
        id: threadNoticeTimer
        interval: 6000
        onTriggered: panel.attachmentNotice = ""
    }
    Connections {
        target: app.thread
        function onAttachmentRejected(reason) {
            panel.attachmentNotice = reason
            threadNoticeTimer.restart()
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

    GifPicker {
        id: threadGifPicker
        target: "thread"
        onGifChosen: (result) => panel.onThreadGifPicked(result)
        onClosed: Qt.callLater(threadComposerInput.forceActiveFocus)
    }

    // Download → validate → send into THIS thread (captured room + root) so a
    // room/thread switch cannot reroute it; the SDK produces a real m.thread
    // reply, never an ordinary room message.
    function onThreadGifPicked(result) {
        app.gifSend.sendToThread(app.thread.roomId, app.thread.rootEventId,
                                 result)
    }

    function sendComposerText() {
        if (app.thread.state !== ThreadController.Ready)
            return
        var body = threadComposerInput.text.trim()
        if (body.length === 0 && !app.thread.hasAttachments)
            return
        // sendText dispatches any queued attachments first, then the text.
        app.thread.sendText(body)
        threadComposerInput.text = ""
        replyList.followLatest = true
        replyList.scrollToEndDeferredIfFollowing()
    }
}
