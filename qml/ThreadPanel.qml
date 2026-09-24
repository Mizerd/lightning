import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MatrixClient

// Thread side panel: a 340px right-side surface replacing the member panel,
// never the room timeline. Header, the root event in a raised card, an "N
// replies" divider, reply rows and a single-row composer. Backed by app.thread
// (ThreadController): replies are the SDK thread timeline in MessageDelegate,
// and the composer sends only through the SDK thread path.
Rectangle {
    id: panel
    color: AppTheme.sidebar

    signal closeRequested()
    // Media entry points supplied by TimelinePane.
    property var openImage: function(mediaKey, httpUrl) {}
    property var saveMedia: function(mediaKey, filename) {}

    property var rootData: ({})
    function refreshRoot() { rootData = app.thread.rootInfo() }

    readonly property string panelRoomId:
        app.thread.active ? app.thread.roomId : app.currentRoomId
    /// The panel's room, re-read when the room list changes: findRoom() is a
    /// Q_INVOKABLE, so a binding calling it would keep its first answer. Matters
    /// for roomEncrypted, which picks the link-preview wording and auto-load
    /// policy.
    property var panelRoom: ({})
    function refreshPanelRoom() {
        panelRoom = panelRoomId === "" ? ({})
                                       : app.roomList.findRoom(panelRoomId)
    }
    onPanelRoomIdChanged: refreshPanelRoom()
    Connections {
        target: app.roomList
        function onDataChanged() { panel.refreshPanelRoom() }
        function onModelReset() { panel.refreshPanelRoom() }
    }
    readonly property string panelRoomName:
        panelRoom && panelRoom.name ? panelRoom.name : ""

    // Presentation-only normalization for the reply preview, mirroring
    // matrix::preview::normalizePreviewText (beginReply stores raw text).
    // Identical to MessageComposerBar.previewLine; both should go once
    // beginReply uses the C++ function.
    function previewLine(text) {
        if (!text)
            return ""
        return String(text)
            .replace(/\[([^\]\n]{1,120})\]\(https:\/\/matrix\.to\/#\/[^)\s]{1,512}\)/g,
                     "$1")
            .replace(/[\u2028\u2029]/g, " ")
            .replace(/\s+/g, " ")
            .trim()
    }

    // List-row recency, identical to RoomDelegate.activityLabel() and
    // HomePane.activityLabel() (no shared JS module between these hosts).
    function activityLabel(when) {
        if (!when || isNaN(when.getTime()) || when.getTime() <= 0)
            return ""
        var now = new Date()
        var days = Math.floor((now - when) / 86400000)
        if (when.toDateString() === now.toDateString())
            // The app-wide clock format, resolved in C++ to a Qt format string.
            // Being inside a function, it updates when the caller's binding
            // next re-evaluates.
            return Qt.formatTime(when, app.settings.clockTimeFormat)
        if (days < 2) return qsTr("Yesterday")
        if (days < 7) return Qt.formatDate(when, "ddd")
        if (when.getFullYear() === now.getFullYear())
            return Qt.formatDate(when, "d MMM")
        return Qt.formatDate(when, "MMM yyyy")
    }

    // Per-thread scroll positions (session-local), keyed by the composite
    // thread timeline id, so reopening a thread restores its position.
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
            // A thread switch/close abandons any open mention popup.
            threadMentionPopup.close()
            // Any lifecycle transition cancels in-flight wheel motion.
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
    Connections {
        target: app.thread
        // The root card is a snapshot. Late decryption, edits, redactions and
        // profile resolution arrive as in-place changes with no row-count
        // change, so the controller signals them explicitly.
        function onRootInfoChanged() { panel.refreshRoot() }
    }
    Connections {
        target: app.thread
        function onNavigationTargetLocated(row) {
            replyList.positionAtNavigationRow(row)
        }
    }
    Component.onCompleted: { refreshRoot(); refreshPanelRoom() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header: forum glyph · Thread · room name · close
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
                // Back to the Threads list when entered from it.
                IconButton {
                    objectName: "threadBackToListButton"
                    visible: app.thread.active && app.thread.listOpen
                    implicitWidth: 30; implicitHeight: 30
                    radius: AppTheme.radiusControl
                    iconName: "arrow_back"
                    iconSize: 18
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
                    color: AppTheme.textPrimary
                    // Pane-header size from the type scale.
                    font.pixelSize: AppTheme.scaled(AppTheme.textTitle)
                    font.weight: AppTheme.weightBold
                }
                Label {
                    // Untrusted text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: panel.panelRoomName
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    elide: Label.ElideRight
                }
                // MSC4306 follow state. Hidden until the homeserver confirms
                // support.
                AppButton {
                    id: followButton
                    objectName: "threadFollowButton"
                    visible: app.thread.active && app.thread.followSupported
                    enabled: !app.thread.followBusy
                    size: "sm"
                    // No width floor in a 340px header; it would eat the room
                    // name.
                    minWidth: 0
                    text: app.thread.followed ? qsTr("Unfollow")
                                              : qsTr("Follow")
                    ToolTip.text: app.thread.followed
                                  ? qsTr("Stop following this thread")
                                  : qsTr("Follow this thread")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: app.thread.setFollowed(!app.thread.followed)
                }
                IconButton {
                    objectName: "threadCloseButton"
                    implicitWidth: 30; implicitHeight: 30
                    radius: AppTheme.radiusControl
                    iconName: "close"
                    iconSize: 18
                    Accessible.name: qsTr("Close thread")
                    ToolTip.text: qsTr("Close thread")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: panel.closeRequested()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: AppTheme.border }

        // Threads list (Threads view mode)
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !app.thread.active && app.thread.listOpen

            Column {
                anchors.centerIn: parent
                spacing: AppTheme.spacingS
                visible: app.thread.listLoading && threadListView.count === 0
                AppBusyIndicator {
                    size: 22
                    running: visible
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Label {
                    text: qsTr("Loading threads…")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                }
            }
            // Empty state with icon and explanation.
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - AppTheme.spacing24 * 2, 260)
                visible: !app.thread.listLoading && threadListView.count === 0
                spacing: AppTheme.spacing8
                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: app.thread.listFailed ? "error" : "forum"
                    size: 28
                    color: AppTheme.textDisabled
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: app.thread.listFailed
                          ? qsTr("Threads could not be loaded.")
                          : qsTr("No threads in this room yet.")
                    color: AppTheme.textSecondary
                    font.pixelSize: AppTheme.scaled(AppTheme.textSubtitle)
                    font.weight: AppTheme.weightStrong
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    visible: !app.thread.listFailed
                    text: qsTr("Reply in a thread from a message's menu to "
                               + "start one.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    wrapMode: Text.Wrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                }
            }

            ListView {
                id: threadListView
                objectName: "threadListView"
                anchors.fill: parent
                clip: true
                model: app.thread.threadList
                spacing: 1
                // Thread rows use the same tile as room rows (avatar, type
                // scale, inset hover chip), as Element does.
                delegate: Item {
                    id: threadRow
                    width: threadListView.width
                    height: listEntry.implicitHeight + AppTheme.spacing10 * 2

                    readonly property string rowSender:
                        modelData.rootSenderName || ""
                    // The root sender's MXID, so the name takes the same
                    // identity ink and the avatar the same fallback colour as
                    // elsewhere. Never hash the display name.
                    readonly property string rowSenderId:
                        modelData.rootSender || ""
                    readonly property bool rowUnread: modelData.unread === true

                    function open() {
                        app.thread.openThread(app.currentRoomId,
                                              modelData.rootEventId || "")
                    }

                    // Keyboard reachable.
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: {
                        var parts = [threadRow.rowSender,
                                     modelData.rootPreview || "",
                                     qsTr("%n reply(s)", "",
                                          modelData.replyCount || 0)]
                        if (threadRow.rowUnread)
                            parts.push(qsTr("Unread"))
                        return parts.filter(function(x) { return x.length > 0 })
                                    .join(", ")
                    }
                    Accessible.onPressAction: threadRow.open()
                    Keys.onReturnPressed: threadRow.open()
                    Keys.onEnterPressed: threadRow.open()
                    Keys.onSpacePressed: threadRow.open()

                    Rectangle {
                        anchors.fill: parent
                        // A 4px gutter, as in the room list, so the hover
                        // chip's corners show.
                        anchors.leftMargin: AppTheme.spacing4
                        anchors.rightMargin: AppTheme.spacing4
                        radius: AppTheme.radiusMd
                        color: rowHover.hovered ? AppTheme.hover : "transparent"
                        border.width: threadRow.activeFocus ? 2 : 0
                        border.color: AppTheme.focusRing
                        HoverHandler { id: rowHover }
                        TapHandler { onTapped: threadRow.open() }
                    }

                    RowLayout {
                        id: listEntry
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: AppTheme.spacing12
                        anchors.rightMargin: AppTheme.spacing12
                        spacing: AppTheme.spacing10

                        Avatar {
                            Layout.alignment: Qt.AlignTop
                            size: 30
                            name: threadRow.rowSender
                            colorKey: threadRow.rowSenderId
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: AppTheme.spacing6
                                Label {
                                    // Untrusted text: never markup.
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: threadRow.rowSender
                                    color: AppTheme.userColor(
                                        threadRow.rowSenderId)
                                    font.pixelSize:
                                        AppTheme.scaled(AppTheme.textBody)
                                    font.weight: AppTheme.weightStrong
                                    elide: Label.ElideRight
                                }
                                Label {
                                    text: panel.activityLabel(
                                        modelData.latestTimestamp)
                                    color: AppTheme.textMuted
                                    font.pixelSize:
                                        AppTheme.scaled(AppTheme.textMeta)
                                }
                                Rectangle {
                                    visible: threadRow.rowUnread
                                    Layout.preferredWidth: 8
                                    Layout.preferredHeight: 8
                                    Layout.alignment: Qt.AlignVCenter
                                    radius: 4
                                    color: AppTheme.unreadBadge
                                }
                            }
                            Label {
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: modelData.rootPreview || ""
                                color: threadRow.rowUnread
                                       ? AppTheme.textPrimary
                                       : AppTheme.textSecondary
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.textMeta)
                                elide: Label.ElideRight
                                maximumLineCount: 1
                            }
                            Label {
                                // Untrusted text: never markup.
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                text: qsTr("%n reply(s)", "",
                                           modelData.replyCount || 0)
                                      + ((modelData.latestPreview || "").length > 0
                                         ? " · " + (modelData.latestSenderName || "")
                                           + ": " + modelData.latestPreview
                                         : "")
                                color: AppTheme.textMuted
                                font.pixelSize:
                                    AppTheme.scaled(AppTheme.textMeta)
                                elide: Label.ElideRight
                                maximumLineCount: 1
                            }
                        }
                    }
                }
                footer: Item {
                    width: threadListView.width
                    height: !app.thread.listEndReached
                            ? AppTheme.buttonHeightSm + AppTheme.spacing12 : 0
                    AppButton {
                        anchors.centerIn: parent
                        visible: !app.thread.listEndReached
                        enabled: !app.thread.listLoading
                        kind: "ghost"
                        size: "sm"
                        text: app.thread.listLoading
                              ? qsTr("Loading…") : qsTr("Load more threads")
                        onClicked: app.thread.paginateList()
                    }
                }
                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            }
        }

        // Pinned root: raised, bordered card (12px pad, radius 10)
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
            radius: AppTheme.radiusLg
            // Reply navigation to the root pulses this card, since the root has
            // no row in the list below. It must never close the thread or jump
            // to the room.
            readonly property bool navigationHighlighted:
                app.thread.navigationHighlightEventId !== ""
                && app.thread.navigationHighlightEventId
                   === (panel.rootData.eventId || "")
            // Colour, not width: animating the border width nudged the card's
            // content.
            border.color: navigationHighlighted ? AppTheme.accent
                                                : AppTheme.border
            border.width: 1
            Behavior on border.color { ColorAnimation { duration: 120 } }
            color: AppTheme.surface
            ColumnLayout {
                id: rootColumn
                objectName: "threadRootColumn"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: AppTheme.spacing12
                spacing: 4
                RowLayout {
                    objectName: "threadRootHeaderRow"
                    // Capped at the column: a fillWidth child here was
                    // otherwise given the layout's implicit width and wrapped
                    // wider than the card, which clipped it. No loop: the
                    // card's width comes from the panel.
                    Layout.fillWidth: true
                    Layout.maximumWidth: rootColumn.width
                    spacing: AppTheme.spacingS
                    visible: panel.rootData.loaded === true
                    Avatar {
                        size: 32
                        name: panel.rootData.senderDisplayName || ""
                        mxc: panel.rootData.senderAvatarMxc || ""
                        colorKey: panel.rootData.sender || ""
                    }
                    Label {
                        // Untrusted text: never markup.
                        textFormat: Text.PlainText
                        // The name gives way when it must: it may shrink
                        // (minimum 0, elided) but not grow past its natural
                        // width. The zero minimum is essential; a Text offers
                        // its implicit width as its minimum, which made the row
                        // overflow the card.
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: implicitWidth
                        elide: Text.ElideRight
                        text: panel.rootData.senderDisplayName || ""
                        // Same per-user identity ink as the timeline rows.
                        color: AppTheme.userColor(panel.rootData.sender || "")
                        font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                        font.weight: AppTheme.weightBold
                    }
                    Label {
                        text: panel.rootData.timestamp
                              ? Qt.formatDateTime(panel.rootData.timestamp,
                                                  "d MMM hh:mm")
                              : ""
                        color: AppTheme.textMuted
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    }
                    Icon {
                        visible: panel.rootData.isEncrypted === true
                        name: "lock"
                        size: 12
                        color: AppTheme.textMuted
                    }
                    Item { Layout.fillWidth: true }
                    // Locate the root in the room timeline without leaving the
                    // room. A real button (pressed state, focus ring, proper
                    // hit area).
                    AppButton {
                        objectName: "threadOpenInRoomButton"
                        Layout.alignment: Qt.AlignVCenter
                        kind: "ghost"
                        size: "sm"
                        minWidth: 0
                        text: qsTr("Open in room")
                        onClicked: app.pagination.jumpToEvent(
                            panel.rootData.eventId || "")
                    }
                }
                Label {
                    objectName: "threadRootBody"
                    // Untrusted text: never markup.
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    Layout.maximumWidth: rootColumn.width
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
                    font.pixelSize: AppTheme.scaled(AppTheme.textBody)
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    wrapMode: Text.Wrap
                    maximumLineCount: 6
                    elide: Text.ElideRight
                }
                Label {
                    visible: panel.rootData.loaded !== true
                    text: qsTr("The original message is unavailable.")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                    font.italic: true
                }
            }
        }

        // "N replies" divider: hairline · label · hairline
        RowLayout {
            objectName: "threadReplyDivider"
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing16
            Layout.rightMargin: AppTheme.spacing16
            Layout.topMargin: AppTheme.spacing8
            Layout.bottomMargin: AppTheme.spacing4
            visible: app.thread.active
                     && app.thread.state === ThreadController.Ready
                     && app.thread.replyCount > 0
            spacing: AppTheme.spacing8
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: AppTheme.border
            }
            Label {
                // Resolved by the controller (SDK num_replies when known, else
                // a loaded count, with the root subtracted only when it is a
                // row). See ThreadController::replyCount.
                readonly property int replies: app.thread.replyCount
                text: qsTr("%n reply(s)", "replies in the open thread",
                           replies)
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                font.weight: AppTheme.weightStrong
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: AppTheme.border
            }
        }

        // Body states + reply list
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Opening / loading state.
            Column {
                anchors.centerIn: parent
                spacing: AppTheme.spacingS
                visible: app.thread.state === ThreadController.Opening
                AppBusyIndicator {
                    size: 22
                    running: visible
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Label {
                    text: qsTr("Loading thread…")
                    color: AppTheme.textMuted
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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
                    color: AppTheme.textSecondary
                    font.pixelSize: AppTheme.scaled(AppTheme.textSubtitle)
                    font.weight: AppTheme.weightStrong
                    wrapMode: Text.Wrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                }
                // A real button for recovery.
                AppButton {
                    objectName: "threadRetryButton"
                    anchors.horizontalCenter: parent.horizontalCenter
                    size: "sm"
                    minWidth: 0
                    text: qsTr("Retry")
                    onClicked: app.thread.openThread(app.thread.roomId,
                                                     app.thread.rootEventId)
                }
            }

            // Empty state: ready, but only the root is loaded.
            Label {
                anchors.centerIn: parent
                visible: app.thread.state === ThreadController.Ready
                         && replyList.count <= 1
                text: qsTr("No replies yet. Start the conversation below.")
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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

                // MessageDelegate view contract, shared with TimelinePane.
                property var timelineModel: app.thread.model
                property string suppressRootEventId: app.thread.rootEventId
                property bool threadContext: true
                property string pinnedActionsKey: ""
                property bool emojiPickerOpen: false
                // Required by MessageDelegate, which writes and reads
                // timelineView.hoveredActionsKey; without it the hover toolbar
                // never appears here.
                property string hoveredActionsKey: ""
                // The same transient-interaction contract as the room timeline;
                // an undefined owner would block nothing.
                property string transientInteractionOwner: ""
                function claimTransientInteraction(owner) {
                    if (!owner || owner.length === 0)
                        return
                    // Clear, not cover.
                    hoveredActionsKey = ""
                    pinnedActionsKey = ""
                    transientInteractionOwner = owner
                }
                function releaseTransientInteraction(owner, fallback) {
                    if (transientInteractionOwner !== owner)
                        return
                    transientInteractionOwner = fallback ? fallback : ""
                }
                onHoveredActionsKeyChanged: {
                    if (transientInteractionOwner !== ""
                        && hoveredActionsKey !== "")
                        hoveredActionsKey = ""
                }
                // From the panel's re-read record (see panelRoom).
                property bool roomEncrypted:
                    panel.panelRoom && panel.panelRoom.encrypted === true
                function stateGroupExpanded(groupId) { return true }
                function toggleStateGroup(groupId) {}
                property var openImage: panel.openImage
                property var saveMedia: panel.saveMedia
                // Shared reaction picker / sender profile entry points; event
                // id captured at open.
                property var openReactionPicker: function(eventId, point) {
                    if (!eventId || eventId.length === 0)
                        return
                    threadReactionPicker.targetEventId = eventId
                    threadReactionPicker.anchorPoint = point
                    threadReactionPicker.open()
                }
                property var openSenderProfile: function(member) {
                    threadSenderProfile.openFor(member)
                }
                // Reply navigation. The policy (bounded thread pagination,
                // thread-identity guard, failure wording) lives in
                // ThreadController; this view only places the row. Targets
                // resolve in this thread only, so there is no app.pagination
                // path here.
                property string navigationHighlightEventId:
                    app.thread.navigationHighlightEventId
                property var navigateToEvent: function(eventId) {
                    app.thread.navigateToEvent(eventId || "")
                }
                function positionAtNavigationRow(row) {
                    if (row < 0 || row >= count)
                        return
                    followLatest = false
                    // A programmatic landing must not be undone by a lingering
                    // wheel animation or by a pagination anchor armed for where
                    // the reader was; dropping it makes restoreCapturedAnchor()
                    // a no-op.
                    app.threadScroll.cancel()
                    anchorStableId = ""
                    positionViewAtIndex(row, ListView.Center)
                    panel.saveThreadScrollPosition()
                }

                // The panel's own wheel motion engine (app.threadScroll), with
                // the same policy as the room timeline and isolated state.
                // Programmatic positioning cancels it first.
                function wheelMinY() { return originY - topMargin }
                function wheelMaxY() {
                    var maxY = originY + contentHeight + bottomMargin - height
                    var minY = wheelMinY()
                    return maxY < minY ? minY : maxY
                }
                function afterWheelSettled() {
                    followLatest = atBottomEdge()
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
                        // A phased frame is a touchpad frame even with 0 whole
                        // pixels (Qt Wayland carries the remainder); treating
                        // those as notches made slow swipes glide far too far.
                        // See TimelinePane's wheel handler. Wheels never have a
                        // phase.
                        var continuousSource = event.phase !== Qt.NoScrollPhase
                        if (event.pixelDelta.y !== 0
                                || (continuousSource && event.angleDelta.y !== 0)) {
                            app.threadScroll.cancel()
                            replyList.contentY = app.threadScroll.pixelTargetY(
                                event.pixelDelta.y, replyList.contentY,
                                minY, maxY)
                            replyList.afterWheelSettled()
                            // Upward touchpad intent leaves follow-latest.
                            if (event.pixelDelta.y > 0
                                    || (continuousSource && event.angleDelta.y > 0))
                                replyList.followLatest = false
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

                // Scroll-anchor preservation across a backward prepend into
                // this reply list (a plain ListView). The restore reads the
                // live contentY and applies a relative shift, so a reader still
                // scrolling when the page lands is not snapped back. Separate
                // from TimelinePane's mechanism: different views, scroll
                // engines and pagination models.
                property string anchorStableId: ""
                property real anchorOffset: 0
                property real anchorContentHeight: 0
                property real anchorItemY: 0
                function captureAnchor() {
                    var row = indexAt(width / 2, contentY + topMargin + 1)
                    if (row < 0) { anchorStableId = ""; return }
                    var it = itemAtIndex(row)
                    // Skip collapsed activity rows so the anchor has height
                    // (defensive; the delegate is shared).
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
                    anchorStableId = app.thread.model.stableIdAt(row)
                    anchorOffset = it ? (contentY - it.y) : 0
                    anchorContentHeight = contentHeight
                    anchorItemY = it ? it.y : 0
                }
                function restoreCapturedAnchor() {
                    // A reader following latest must not be pulled back to the
                    // anchor.
                    if (anchorStableId === "" || followLatest) {
                        anchorStableId = ""
                        return
                    }
                    // Read the current position now: requestOlder() is async
                    // and the reader may have kept scrolling. The restore is a
                    // relative shift from this value.
                    var beforeY = contentY
                    // Cancel any lingering wheel animation.
                    app.threadScroll.cancel()
                    var newRow = app.thread.model.rowForStableId(anchorStableId)
                    if (newRow < 0) {
                        var delta = contentHeight - anchorContentHeight
                        if (delta > 0) contentY += delta
                        anchorStableId = ""
                        return
                    }
                    // Forces the anchor delegate to exist so its geometry is
                    // real.
                    positionViewAtIndex(newRow, ListView.Beginning)
                    var it = itemAtIndex(newRow)
                    if (it) {
                        var shift = it.y - anchorItemY
                        contentY = beforeY + shift
                    }
                    anchorStableId = ""
                }
                Connections {
                    target: app.thread.model
                    property bool wasPaginating: false
                    function onPaginationChanged() {
                        var busy = app.thread.model.paginating
                        if (busy && !wasPaginating && !replyList.followLatest)
                            replyList.captureAnchor()
                        if (wasPaginating && !busy) {
                            if (app.thread.model.paginationFailed)
                                replyList.anchorStableId = ""
                            else
                                Qt.callLater(replyList.restoreCapturedAnchor)
                        }
                        wasPaginating = busy
                    }
                }

                property bool followLatest: true
                // Bottom-follow is latched to user intent (see the room
                // timeline): a sub-line slack means scrolling up always
                // disengages.
                readonly property real bottomFollowSlack: 8
                function atBottomEdge() {
                    return atYEnd
                           || (contentY + height >= contentHeight - bottomFollowSlack)
                }
                function scrollToEndDeferredIfFollowing() {
                    if (followLatest && count > 0) {
                        Qt.callLater(function() { replyList.positionViewAtEnd() })
                        // Reading the latest reply sends one deduplicated
                        // threaded receipt (never room-wide).
                        app.thread.markRead()
                    }
                }
                onMovementEnded: {
                    followLatest = atBottomEdge()
                    if (followLatest)
                        app.thread.markRead()
                    // Near-top backfill; requestOlder() is single-flight.
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
                        AppBusyIndicator {
                            size: 14
                            running: visible
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Loading older replies…")
                            color: AppTheme.textMuted
                            font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        }
                    }
                }

                ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
            }

            // Failure notice for reply navigation, the same sentence as the
            // room timeline. An inline pill, not a dialog.
            Label {
                objectName: "threadNavigationNotice"
                visible: app.thread.navigationMessage.length > 0
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: AppTheme.spacing12
                width: Math.min(implicitWidth,
                                parent.width - AppTheme.spacing12 * 2)
                text: app.thread.navigationMessage
                color: AppTheme.textMuted
                font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                topPadding: AppTheme.spacingS
                bottomPadding: AppTheme.spacingS
                leftPadding: AppTheme.spacing12
                rightPadding: AppTheme.spacing12
                z: 5
                background: Rectangle {
                    radius: AppTheme.radiusSm
                    color: AppTheme.surface
                    border.color: AppTheme.border
                    border.width: 1
                }
            }
        }

        // Reply-within-thread banner: shows the active reply target; ✕ or
        // Escape cancels it without closing the panel. Built like the room
        // composer's context strip.
        Item {
            id: threadReplyBanner
            objectName: "threadReplyBanner"
            visible: app.thread.inReply
            Layout.fillWidth: true
            Layout.leftMargin: AppTheme.spacing12 + 4
            Layout.rightMargin: AppTheme.spacing12
            Layout.topMargin: AppTheme.spacing4
            implicitHeight: threadReplyLayout.implicitHeight

            readonly property bool jumpable:
                (app.thread.replyToEventId || "").length > 0
            readonly property string bodyText:
                panel.previewLine(app.thread.replyToPreview)
            function jumpToTarget() {
                if (threadReplyBanner.jumpable)
                    app.thread.navigateToEvent(app.thread.replyToEventId)
            }

            activeFocusOnTab: visible && jumpable
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Replying to %1").arg(
                                 app.thread.replyToSender)
                             + (bodyText.length > 0 ? ": " + bodyText : "")
            Accessible.onPressAction: threadReplyBanner.jumpToTarget()
            Keys.onReturnPressed: threadReplyBanner.jumpToTarget()
            Keys.onEnterPressed: threadReplyBanner.jumpToTarget()
            Keys.onSpacePressed: threadReplyBanner.jumpToTarget()

            TapHandler {
                enabled: threadReplyBanner.jumpable
                onTapped: threadReplyBanner.jumpToTarget()
            }
            HoverHandler {
                id: threadReplyHover
                enabled: threadReplyBanner.jumpable
                cursorShape: Qt.PointingHandCursor
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: AppTheme.radiusSm
                color: "transparent"
                border.color: AppTheme.focusRing
                border.width: 2
                visible: threadReplyBanner.activeFocus
            }

            RowLayout {
                id: threadReplyLayout
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: AppTheme.spacing8

                Rectangle {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 2
                    radius: 1
                    color: threadReplyHover.hovered ? AppTheme.accentHover
                                                    : AppTheme.accent
                    Behavior on color { ColorAnimation { duration: 90 } }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Replying to %1")
                                  .arg(app.thread.replyToSender
                                       || qsTr("someone"))
                        textFormat: Text.PlainText
                        color: AppTheme.textPrimary
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        font.weight: AppTheme.weightStrong
                        elide: Label.ElideRight
                        maximumLineCount: 1
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: threadReplyBanner.bodyText.length > 0
                        text: threadReplyBanner.bodyText
                        textFormat: Text.PlainText
                        color: AppTheme.textSecondary
                        font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
                        elide: Label.ElideRight
                        maximumLineCount: 1
                    }
                }
                IconButton {
                    objectName: "threadReplyCancelButton"
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: 24; implicitHeight: 24
                    radius: AppTheme.radiusControl
                    iconName: "close"
                    iconSize: 16
                    Accessible.name: qsTr("Cancel reply")
                    ToolTip.text: Accessible.name
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    onClicked: app.thread.cancelReply()
                }
            }
        }

        // Mini composer: one row in a raised card, pinned at the bottom. Sends
        // only through ThreadController.sendText (the SDK m.thread path, text
        // and attachments); the room composer and its draft are untouched.
        Item {
            Layout.fillWidth: true
            visible: app.thread.active
            implicitHeight: threadComposerCol.implicitHeight
                            + AppTheme.spacing12

            // Files dropped here are queued for the open thread, never the room
            // composer.
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
                    font.pixelSize: AppTheme.scaled(AppTheme.textMeta)
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
                                        // Untrusted text: never markup.
                                        textFormat: Text.PlainText
                                        text: model.fileName
                                        color: AppTheme.text
                                        font.pixelSize:
                                            AppTheme.scaled(AppTheme.textMeta)
                                        font.weight: AppTheme.weightMedium
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
                                        font.pixelSize:
                                            AppTheme.scaled(AppTheme.textMeta)
                                        elide: Label.ElideRight
                                        Layout.maximumWidth: 120
                                    }
                                }
                                IconButton {
                                    visible: model.state === "failed"
                                    implicitWidth: 20; implicitHeight: 20
                                    radius: AppTheme.radiusSm
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
                                    radius: AppTheme.radiusSm
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
                    id: threadMiniComposer
                    objectName: "threadMiniComposer"
                    Layout.fillWidth: true
                    implicitHeight: composerRow.implicitHeight
                                    + AppTheme.spacing8 * 2
                    radius: AppTheme.radiusLg
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
                            radius: AppTheme.radiusControl
                            iconName: "add_circle"
                            iconSize: 18
                            enabled: app.thread.attachmentsSupported
                            Accessible.name: qsTr("Attach files")
                            ToolTip.text: qsTr("Attach")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: threadAttachDialog.open()
                        }
                        // Rich composer for the thread panel; same growth rules
                        // as threadInputFlick, visibility-exclusive with it.
                        Flickable {
                            id: threadRichFlick
                            objectName: "threadRichInputFlick"
                            visible: panel.richMode
                            Layout.fillWidth: true
                            Layout.maximumHeight: AppTheme.scaled(110)
                            implicitHeight: threadRichInput.implicitHeight
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            flickableDirection: Flickable.VerticalFlick
                            ScrollBar.vertical: AppScrollBar { thin: true }
                            ToolTip.visible: panel.richMode
                                             && app.thread.commandError.length > 0
                            ToolTip.text: app.thread.commandError + " "
                                          + qsTr("Press Enter again to send it as a message.")

                            TextArea.flickable: TextArea {
                                id: threadRichInput
                                objectName: "threadRichInput"
                                onWidthChanged: threadRichSpellTimer.restart()
                                Timer {
                                    id: threadRichSpellTimer
                                    objectName: "threadRichSpellTimer"
                                    interval: 150
                                    repeat: false
                                    onTriggered: panel.refreshThreadRichSpellUnderlines()
                                }
                                Repeater {
                                    objectName: "threadRichSpellUnderlines"
                                    model: panel.threadRichSpellUnderlines
                                    delegate: Rectangle {
                                        objectName: "threadRichSpellUnderline"
                                        required property var modelData
                                        x: modelData.x
                                        y: modelData.y
                                        width: modelData.w
                                        height: 2
                                        radius: 1
                                        color: Qt.alpha(AppTheme.danger, 0.85)
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.RightButton
                                    onClicked: (mouse) => {
                                        threadRichInput.forceActiveFocus()
                                        panel.prepareThreadSpellMenu(mouse.x,
                                                                     mouse.y)
                                        threadComposerEditMenu.popup()
                                    }
                                }
                                textFormat: TextEdit.RichText
                                placeholderText: panel.threadRichBlank
                                                 ? qsTr("Reply in thread") : ""
                                placeholderTextColor: AppTheme.textMuted
                                inputMethodHints: Qt.ImhNone
                                wrapMode: TextArea.Wrap
                                enabled: app.thread.state === ThreadController.Ready
                                background: Rectangle { color: "transparent" }
                                color: AppTheme.text
                                font.pixelSize: AppTheme.scaled(13)
                                onTextChanged: {
                                    threadRichSpellTimer.restart()
                                    panel.refreshThreadRichBlank()
                                    if (panel.richSyncing || !panel.richMode)
                                        return
                                    panel.richSyncing = true
                                    app.thread.text =
                                        app.richComposer.toMarkdown(textDocument)
                                    panel.richSyncing = false
                                    panel.updateThreadRichMentionState()
                                }
                                onCursorPositionChanged: {
                                    panel.updateThreadRichMentionState()
                                    threadRichSpellTimer.restart()
                                }
                                Keys.onShortcutOverride: (event) => {
                                    if (app.shortcuts.editorActionForKey(
                                            event.key, event.modifiers) !== "")
                                        event.accepted = true
                                }
                                Keys.onPressed: (event) => {
                                    if (threadMentionPopup.visible) {
                                        if (event.key === Qt.Key_Down) {
                                            threadMentionPopup.moveDown()
                                            event.accepted = true; return
                                        }
                                        if (event.key === Qt.Key_Up) {
                                            threadMentionPopup.moveUp()
                                            event.accepted = true; return
                                        }
                                        if (event.key === Qt.Key_Tab
                                            || event.key === Qt.Key_Return
                                            || event.key === Qt.Key_Enter) {
                                            threadMentionPopup.accept()
                                            event.accepted = true; return
                                        }
                                        if (event.key === Qt.Key_Escape) {
                                            threadMentionPopup.close()
                                            event.accepted = true; return
                                        }
                                    }
                                    var richFormatAction =
                                        app.shortcuts.editorActionForKey(
                                            event.key, event.modifiers)
                                    if (richFormatAction !== "") {
                                        event.accepted = true
                                        panel.applyThreadFormat(
                                            richFormatAction.substring(
                                                "composer.".length))
                                        return
                                    }
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
                        }
                        Flickable {
                            id: threadInputFlick
                            visible: !panel.richMode
                            Layout.fillWidth: true
                            // Slash-command refusal, with the literal-send
                            // escape spelled out.
                            ToolTip.visible: !panel.richMode
                                             && app.thread.commandError.length > 0
                            ToolTip.text: app.thread.commandError + " "
                                          + qsTr("Press Enter again to send it as a message.")
                            // Grows to ~6 lines, then scrolls with the caret
                            // kept in view (as the room composer).
                            Layout.maximumHeight: AppTheme.scaled(110)
                            implicitHeight: threadComposerInput.implicitHeight
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            flickableDirection: Flickable.VerticalFlick
                            ScrollBar.vertical: AppScrollBar { thin: true }

                            TextArea.flickable: TextArea {
                            id: threadComposerInput
                            objectName: "threadComposerInput"
                            placeholderText: qsTr("Reply in thread")
                            placeholderTextColor: AppTheme.textMuted
                            // Declared and deliberately empty; see the room
                            // composer.
                            inputMethodHints: Qt.ImhNone
                            wrapMode: TextArea.Wrap
                            enabled: app.thread.state === ThreadController.Ready
                            // The card carries the chrome; the field is bare.
                            background: Rectangle { color: "transparent" }
                            color: AppTheme.text
                            font.pixelSize: AppTheme.scaled(13)
                            // The text lives on app.thread so outgoing
                            // @-mentions can be tracked (two-way sync, as in
                            // the room composer).
                            text: app.thread.text
                            onTextChanged: {
                                if (app.thread.text !== text)
                                    app.thread.text = text
                                panel.updateThreadMentionState()
                                threadSpellTimer.restart()
                            }
                            onCursorPositionChanged: {
                                panel.updateThreadMentionState()
                                threadSpellTimer.restart()
                            }
                            onWidthChanged: threadSpellTimer.restart()

                            // Spell underlines; see the room composer.
                            Timer {
                                id: threadSpellTimer
                                objectName: "threadSpellTimer"
                                interval: 150
                                repeat: false
                                onTriggered: panel.refreshThreadSpellUnderlines()
                            }
                            Repeater {
                                objectName: "threadSpellUnderlines"
                                model: panel.threadSpellUnderlines
                                delegate: Rectangle {
                                    objectName: "threadSpellUnderline"
                                    required property var modelData
                                    x: modelData.x
                                    y: modelData.y
                                    width: modelData.w
                                    height: 2
                                    radius: 1
                                    color: Qt.alpha(AppTheme.danger, 0.85)
                                }
                            }

                            // Right-click editing menu, for the same reason as
                            // the room composer's (TextEdit's own Paste is
                            // text-only), plus the spelling rows.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onClicked: (mouse) => {
                                    threadComposerInput.forceActiveFocus()
                                    panel.prepareThreadSpellMenu(mouse.x,
                                                                 mouse.y)
                                    threadComposerEditMenu.popup()
                                }
                            }
                            AppMenu {
                                id: threadComposerEditMenu
                                objectName: "threadComposerEditMenu"
                                menuWidth: AppTheme.menuWidthFlyout
                                AppMenuItem {
                                    objectName: "threadSpellSuggestion0"
                                    visible: panel.threadSpellMenuSuggestions.length > 0
                                    text: visible
                                          ? panel.threadSpellMenuSuggestions[0] : ""
                                    onTriggered: panel.applyThreadSpellSuggestion(
                                                     panel.threadSpellMenuSuggestions[0])
                                }
                                AppMenuItem {
                                    objectName: "threadSpellSuggestion1"
                                    visible: panel.threadSpellMenuSuggestions.length > 1
                                    text: visible
                                          ? panel.threadSpellMenuSuggestions[1] : ""
                                    onTriggered: panel.applyThreadSpellSuggestion(
                                                     panel.threadSpellMenuSuggestions[1])
                                }
                                AppMenuItem {
                                    objectName: "threadSpellSuggestion2"
                                    visible: panel.threadSpellMenuSuggestions.length > 2
                                    text: visible
                                          ? panel.threadSpellMenuSuggestions[2] : ""
                                    onTriggered: panel.applyThreadSpellSuggestion(
                                                     panel.threadSpellMenuSuggestions[2])
                                }
                                AppMenuItem {
                                    objectName: "threadSpellSuggestion3"
                                    visible: panel.threadSpellMenuSuggestions.length > 3
                                    text: visible
                                          ? panel.threadSpellMenuSuggestions[3] : ""
                                    onTriggered: panel.applyThreadSpellSuggestion(
                                                     panel.threadSpellMenuSuggestions[3])
                                }
                                AppMenuItem {
                                    objectName: "threadSpellSuggestion4"
                                    visible: panel.threadSpellMenuSuggestions.length > 4
                                    text: visible
                                          ? panel.threadSpellMenuSuggestions[4] : ""
                                    onTriggered: panel.applyThreadSpellSuggestion(
                                                     panel.threadSpellMenuSuggestions[4])
                                }
                                AppMenuItem {
                                    objectName: "threadSpellAdd"
                                    visible: panel.threadSpellMenuWord !== ""
                                    text: qsTr("Add to dictionary")
                                    onTriggered: app.spell.addToDictionary(
                                                     panel.threadSpellMenuWord)
                                }
                                AppMenuItem {
                                    objectName: "threadSpellIgnore"
                                    visible: panel.threadSpellMenuWord !== ""
                                    text: qsTr("Ignore")
                                    onTriggered: app.spell.ignoreWord(
                                                     panel.threadSpellMenuWord)
                                }
                                AppMenuSeparator {
                                    visible: panel.threadSpellMenuWord !== ""
                                }
                                AppMenuItem {
                                    text: qsTr("Cut")
                                    enabled: panel.activeThreadEditor().selectedText.length > 0
                                    onTriggered: panel.activeThreadEditor().cut()
                                }
                                AppMenuItem {
                                    text: qsTr("Copy")
                                    enabled: panel.activeThreadEditor().selectedText.length > 0
                                    onTriggered: panel.activeThreadEditor().copy()
                                }
                                AppMenuItem {
                                    objectName: "threadComposerPasteItem"
                                    text: qsTr("Paste")
                                    onTriggered: {
                                        if (!app.thread.pasteFromClipboard())
                                            panel.activeThreadEditor().paste()
                                    }
                                }
                                AppMenuSeparator {}
                                AppMenuItem {
                                    text: qsTr("Select all")
                                    enabled: panel.activeThreadEditor().length > 0
                                    onTriggered: panel.activeThreadEditor().selectAll()
                                }
                            }
                            // Accept ShortcutOverride only for keys this box
                            // handles, so the key arrives here as a plain
                            // press; Ctrl+K and Ctrl+Q still reach the window.
                            Keys.onShortcutOverride: (event) => {
                                if (app.shortcuts.editorActionForKey(
                                        event.key, event.modifiers) !== "")
                                    event.accepted = true
                            }
                            Keys.onPressed: (event) => {
                                // The mention popup gets the navigation keys
                                // first while open.
                                if (threadMentionPopup.visible) {
                                    if (event.key === Qt.Key_Down) {
                                        threadMentionPopup.moveDown()
                                        event.accepted = true; return
                                    }
                                    if (event.key === Qt.Key_Up) {
                                        threadMentionPopup.moveUp()
                                        event.accepted = true; return
                                    }
                                    if (event.key === Qt.Key_Tab
                                        || event.key === Qt.Key_Return
                                        || event.key === Qt.Key_Enter) {
                                        threadMentionPopup.accept()
                                        event.accepted = true; return
                                    }
                                    if (event.key === Qt.Key_Escape) {
                                        threadMentionPopup.close()
                                        event.accepted = true; return
                                    }
                                }
                                // Editor formatting, before the plain-key
                                // branches (every editor binding has Ctrl).
                                // Unrecognised keys are not accepted.
                                var threadFormatAction =
                                    app.shortcuts.editorActionForKey(
                                        event.key, event.modifiers)
                                if (threadFormatAction !== "") {
                                    event.accepted = true
                                    panel.applyThreadFormat(
                                        threadFormatAction.substring(
                                            "composer.".length))
                                    return
                                }
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
                                } else if ((event.key === Qt.Key_Backspace
                                            || event.key === Qt.Key_Delete)
                                           && threadComposerInput.selectionStart
                                              === threadComposerInput.selectionEnd) {
                                    // Atomic mention delete, as in the room
                                    // composer.
                                    var ranges = app.thread.mentionRanges
                                    for (var i = 0; i < ranges.length; ++i) {
                                        var r = ranges[i]
                                        var hit = event.key === Qt.Key_Backspace
                                            ? threadComposerInput.cursorPosition
                                              === r.start + r.length
                                            : threadComposerInput.cursorPosition
                                              === r.start
                                        if (hit) {
                                            threadComposerInput.remove(
                                                r.start, r.start + r.length)
                                            event.accepted = true
                                            return
                                        }
                                    }
                                }
                            }

                            MentionHighlighter {
                                document: threadComposerInput.textDocument
                                ranges: panel.threadMentionHighlightRanges
                                accentColor: AppTheme.accent
                                // Named, because Qt 6.8 picks a monochrome
                                // emoji face; per-range, so the surrounding
                                // words keep the UI face.
                                emojiFontFamily: app.emojiFontFamily || ""
                            }
                            }
                        }
                        IconButton {
                            id: threadEmojiButton
                            objectName: "threadEmojiButton"
                            implicitWidth: 24; implicitHeight: 24
                            radius: AppTheme.radiusControl
                            iconName: "mood"
                            iconSize: 18
                            visible: panel.composerButtonShown("emoji")
                            enabled: app.thread.state === ThreadController.Ready
                            Accessible.name: qsTr("Insert emoji")
                            onClicked: {
                                // anchorItem maps into overlay coordinates
                                // correctly and re-maps on resize.
                                threadEmojiPicker.anchorItem = threadMiniComposer
                                threadEmojiPicker.open()
                            }
                        }
                        // GIFs and stickers: one button, one window (see the
                        // room composer). An IconButton so it has a focus ring.
                        IconButton {
                            id: threadMediaButton
                            objectName: "threadMediaButton"
                            implicitWidth: 24; implicitHeight: 24
                            radius: AppTheme.radiusControl
                            iconName: "gif_box"
                            iconSize: 18
                            // Present and disabled when neither kind is
                            // available.
                            visible: panel.composerButtonShown("media")
                            enabled: app.thread.state === ThreadController.Ready
                                     && (app.gif.available
                                         || app.stickers.available)
                            Accessible.name: qsTr("Insert a GIF or sticker")
                            ToolTip.text: !app.gif.available && !app.stickers.available
                                          ? qsTr("GIFs and stickers are unavailable "
                                                 + "on this backend")
                                          : panel.mediaPickerBothKinds
                                            ? qsTr("GIFs and stickers")
                                            : (app.gif.available ? qsTr("GIF")
                                                                 : qsTr("Sticker"))
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: panel.openThreadMediaPicker()
                        }
                        // Voice capture with the shared recorder; ownership
                        // lives in app.voiceOwner, so only one composer can
                        // send a finished recording.
                        IconButton {
                            objectName: "threadMicButton"
                            implicitWidth: 24; implicitHeight: 24
                            radius: AppTheme.radiusControl
                            iconName: "mic"
                            iconSize: 18
                            // A recording in flight keeps its controls.
                            visible: !panel.voiceActive
                                     && panel.composerButtonShown("voice")
                            enabled: app.thread.state === ThreadController.Ready
                                     && app.thread.attachmentsSupported
                            Accessible.name: qsTr("Record a voice message")
                            ToolTip.text: qsTr("Record a voice message")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: {
                                if (!app.startVoiceRecording("thread")) {
                                    panel.attachmentNotice =
                                        app.voiceRecordingBusy()
                                        ? qsTr("A recording is already in "
                                               + "progress.")
                                        : qsTr("Voice recording is "
                                               + "unavailable.")
                                    threadNoticeTimer.restart()
                                }
                            }
                        }
                        Rectangle {
                            id: threadVoicePill
                            objectName: "threadVoicePill"
                            // Never touch app.voiceRecorder while idle: its
                            // getter constructs the recorder and audio backend.
                            readonly property var rec:
                                panel.voiceActive ? app.voiceRecorder : null
                            visible: panel.voiceActive
                            Layout.alignment: Qt.AlignVCenter
                            implicitHeight: 24
                            implicitWidth: threadVoiceRow.implicitWidth + 14
                            radius: AppTheme.radiusPill
                            color: AppTheme.accentSoft
                            border.color: AppTheme.accent
                            border.width: 1
                            RowLayout {
                                id: threadVoiceRow
                                anchors.centerIn: parent
                                spacing: AppTheme.spacing6
                                Rectangle {
                                    id: threadVoiceDot
                                    width: 7; height: 7; radius: 3.5
                                    // A fill: `danger` is an ink-only role.
                                    color: AppTheme.dangerFill
                                    property real t: 0
                                    opacity: (threadVoicePill.rec
                                              && threadVoicePill.rec.processing)
                                             || AppTheme.reducedMotion
                                             ? 1.0 : 0.35 + 0.65 * threadVoiceDot.t
                                    SequentialAnimation on t {
                                        running: panel.voiceActive
                                                 && !AppTheme.reducedMotion
                                        loops: Animation.Infinite
                                        NumberAnimation { from: 0; to: 1; duration: 700 }
                                        NumberAnimation { from: 1; to: 0; duration: 700 }
                                    }
                                }
                                Label {
                                    text: {
                                        var ms = threadVoicePill.rec
                                                 ? threadVoicePill.rec.durationMs : 0
                                        var total = Math.floor(ms / 1000)
                                        var m = Math.floor(total / 60)
                                        var s = total % 60
                                        return m + ":" + (s < 10 ? "0" : "") + s
                                    }
                                    color: AppTheme.text
                                    font.pixelSize:
                                        AppTheme.scaled(AppTheme.textMeta)
                                    font.weight: AppTheme.weightStrong
                                }
                                // Pause/resume and Done, as in the room
                                // composer.
                                IconButton {
                                    objectName: "threadVoicePauseButton"
                                    implicitWidth: 22; implicitHeight: 22
                                    iconName: threadVoicePill.rec
                                              && threadVoicePill.rec.paused
                                              ? "play_arrow" : "pause"
                                    iconSize: 14
                                    enabled: threadVoicePill.rec
                                             && threadVoicePill.rec.recording
                                    Accessible.name:
                                        threadVoicePill.rec
                                        && threadVoicePill.rec.paused
                                        ? qsTr("Resume recording")
                                        : qsTr("Pause recording")
                                    ToolTip.text: Accessible.name
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 500
                                    onClicked: {
                                        if (!threadVoicePill.rec)
                                            return
                                        if (threadVoicePill.rec.paused)
                                            threadVoicePill.rec.resume()
                                        else
                                            threadVoicePill.rec.pause()
                                    }
                                }
                                IconButton {
                                    objectName: "threadVoiceCancelButton"
                                    implicitWidth: 22; implicitHeight: 22
                                    iconName: "close"
                                    iconSize: 14
                                    Accessible.name: qsTr("Discard the recording")
                                    ToolTip.text: qsTr("Discard")
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 500
                                    onClicked: app.cancelVoiceRecording()
                                }
                                IconButton {
                                    objectName: "threadVoiceDoneButton"
                                    implicitWidth: 22; implicitHeight: 22
                                    iconName: "check"
                                    iconSize: 14
                                    enabled: threadVoicePill.rec
                                             && threadVoicePill.rec.recording
                                    Accessible.name: qsTr("Finish and review")
                                    ToolTip.text: qsTr("Done")
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 500
                                    onClicked: {
                                        panel.voiceWantsPreview = true
                                        app.voiceRecorder.stop()
                                    }
                                }
                                IconButton {
                                    objectName: "threadVoiceSendButton"
                                    implicitWidth: 22; implicitHeight: 22
                                    fill: true
                                    iconName: "send"
                                    iconSize: 13
                                    enabled: threadVoicePill.rec
                                             && threadVoicePill.rec.recording
                                    Accessible.name: qsTr("Send the voice message")
                                    ToolTip.text: qsTr("Send")
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 500
                                    // stop() finalizes and derives the
                                    // waveform; ready() sends.
                                    onClicked: app.voiceRecorder.stop()
                                }
                            }
                        }
                        VoicePreviewBar {
                            objectName: "threadVoicePreview"
                            compact: true
                            Layout.alignment: Qt.AlignVCenter
                            visible: panel.pendingVoice !== null
                            filePath: panel.pendingVoice
                                      ? panel.pendingVoice.filePath : ""
                            mime: panel.pendingVoice
                                  ? panel.pendingVoice.mime : ""
                            durationMs: panel.pendingVoice
                                        ? panel.pendingVoice.durationMs : 0
                            waveform: panel.pendingVoice
                                      ? panel.pendingVoice.waveform : []
                            onSendRequested: panel.sendPendingVoice()
                            onDiscardRequested: panel.discardPendingVoice()
                        }

                        // Accent-fill thread send: 28×28, 16px icon.
                        IconButton {
                            objectName: "threadSendButton"
                            implicitWidth: 28; implicitHeight: 28
                            radius: AppTheme.radiusControl
                            fill: true
                            iconName: "send"
                            iconSize: 16
                            // app.thread.text is the markdown mirror in both
                            // modes.
                            enabled: app.thread.state === ThreadController.Ready
                                     && (app.thread.text.trim().length > 0
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

    // This panel owns the shared recorder. Derived from app.voiceOwner, never a
    // local flag (see MessageComposerBar and AppController::voiceOwner).
    readonly property bool voiceActive: app.voiceOwner === "thread"
    // A recording belongs to the thread it started in; leaving the thread
    // discards it.
    Connections {
        target: app.thread
        function onStateChanged() {
            if (app.thread.state === ThreadController.Ready)
                return
            if (panel.voiceActive)
                app.cancelVoiceRecording()
            // A finished, unsent recording is deleted on leaving.
            panel.voiceWantsPreview = false
            panel.discardPendingVoice()
        }
    }
    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            if (panel.voiceActive)
                app.cancelVoiceRecording()
            panel.voiceWantsPreview = false
            panel.discardPendingVoice()
        }
    }
    // Recorder results. Only targets the recorder while this panel owns a
    // recording, so it never constructs it.
    Connections {
        target: panel.voiceActive ? app.voiceRecorder : null
        function onReady(filePath, mime, durationMs, waveform) {
            // Release ownership first, so a re-entrant signal finds us
            // disarmed.
            app.endVoiceRecording()
            if (panel.voiceWantsPreview) {
                panel.voiceWantsPreview = false
                // An unanswered preview is replaced and its file deleted.
                panel.discardPendingVoice()
                panel.pendingVoice = { filePath: filePath, mime: mime,
                                       durationMs: durationMs,
                                       waveform: waveform }
                return
            }
            app.thread.sendVoiceMessage(filePath, mime, durationMs, waveform)
        }
        function onFailed(message) {
            app.endVoiceRecording()
            panel.voiceWantsPreview = false
            panel.attachmentNotice = message
            threadNoticeTimer.restart()
        }
    }

    // A finished recording awaiting review (see VoicePreviewBar); owned by this
    // panel until sent or deleted.
    property var pendingVoice: null
    property bool voiceWantsPreview: false
    function sendPendingVoice() {
        if (!panel.pendingVoice)
            return
        var v = panel.pendingVoice
        panel.pendingVoice = null
        app.thread.sendVoiceMessage(v.filePath, v.mime, v.durationMs,
                                    v.waveform)
    }
    function discardPendingVoice() {
        if (!panel.pendingVoice)
            return
        var v = panel.pendingVoice
        panel.pendingVoice = null
        app.discardPreparedVoice(v.filePath)
    }

    EmojiPicker {
        id: threadEmojiPicker
        mode: "composer"
        // Sticky like the room composer's picker; the handler never steals
        // focus.
        closeAfterSelection: false
        onEmojiChosen: (emoji) => {
            threadComposerInput.insert(threadComposerInput.cursorPosition,
                                       emoji)
        }
        onClosed: Qt.callLater(threadComposerInput.forceActiveFocus)
    }

    // Outgoing @-mentions, as in the room composer: current-room members, input
    // keeps focus.
    property int threadMentionTokenStart: -1
    MentionPopup {
        id: threadMentionPopup
        suggestions: app.mentionSuggestions
        onChosen: (userId, displayName) =>
            panel.insertThreadMention(userId, displayName)
        // Closing must drop the synthetic in-progress range without rescanning.
        onVisibleChanged: panel.refreshThreadMentionHighlight()
    }
    // Same rescan guard as the room composer: only genuine edits or cursor
    // moves rescan.
    property string lastThreadMentionScanText: ""
    property int lastThreadMentionScanCursor: -1
    function updateThreadMentionState() {
        if (panel.richMode)
            return // the rich editor scans itself (updateThreadRichMentionState)
        if (threadComposerInput.text === panel.lastThreadMentionScanText
            && threadComposerInput.cursorPosition
               === panel.lastThreadMentionScanCursor)
            return
        panel.lastThreadMentionScanText = threadComposerInput.text
        panel.lastThreadMentionScanCursor = threadComposerInput.cursorPosition
        if (!app.thread.active) {
            threadMentionPopup.close()
            panel.refreshThreadMentionHighlight()
            return
        }
        var tok = app.thread.mentionTokenAt(threadComposerInput.text,
                                            threadComposerInput.cursorPosition)
        if (tok && tok.active === true) {
            panel.threadMentionTokenStart = tok.start
            app.mentionSuggestions.roomId = app.thread.roomId
            app.mentionSuggestions.query = tok.query
            threadMentionPopup.query = tok.query
            // Viewport anchor, not the reparented TextArea (as in the room
            // composer).
            var p = threadInputFlick.mapToItem(Overlay.overlay, 0, 0)
            threadMentionPopup.anchorInputTop = Qt.point(p.x, p.y)
            threadMentionPopup.anchorWidth = threadInputFlick.width
            if (!threadMentionPopup.visible)
                threadMentionPopup.open()
        } else {
            panel.threadMentionTokenStart = -1
            threadMentionPopup.close()
        }
        panel.refreshThreadMentionHighlight()
    }
    // The in-progress "@token" chip: app.thread.mentionRanges plus one
    // presentation-only range while the popup is open, never written back.
    // Assigned explicitly (a binding on cursor signals would loop), and copied
    // rather than stored as read (on Qt 6.8 a stored QVariantList stays live).
    // See MessageComposerBar's refreshMentionHighlight.
    property var threadMentionHighlightRanges: []
    function refreshThreadMentionHighlight() {
        var ranges = []
        var live = app.thread.mentionRanges
        for (var k = 0; k < live.length; ++k)
            ranges.push({ start: live[k].start, length: live[k].length })
        if (threadMentionPopup.visible && panel.threadMentionTokenStart >= 0) {
            var len = threadComposerInput.cursorPosition
                      - panel.threadMentionTokenStart
            if (len > 0)
                ranges = ranges.concat([{ start: panel.threadMentionTokenStart,
                                          length: len }])
        }
        // Assign only on a real change.
        var current = panel.threadMentionHighlightRanges
        if (current.length === ranges.length) {
            var same = true
            for (var i = 0; i < ranges.length; ++i) {
                if (current[i].start !== ranges[i].start
                    || current[i].length !== ranges[i].length) {
                    same = false
                    break
                }
            }
            if (same)
                return
        }
        panel.threadMentionHighlightRanges = ranges
    }
    Connections {
        target: app.thread
        function onMentionRangesChanged() {
            panel.refreshThreadMentionHighlight()
        }
    }

    // Spell checking, identical to the room composer; the rationale for drawing
    // rectangles is in MessageComposerBar.qml.
    readonly property bool threadSpellActive: app.spell !== null
                                              && app.spell !== undefined
                                              && app.spell.available
                                              && app.spell.enabled
    property var threadSpellUnderlines: []
    property string threadSpellMenuWord: ""
    property int threadSpellMenuStart: -1
    property int threadSpellMenuLength: 0
    property var threadSpellMenuSuggestions: []

    // Rich-mode underlines with their own geometry.
    property var threadRichSpellUnderlines: []
    // See the room composer: `length` counts characters, and an empty list item
    // has none while drawing its marker.
    property bool threadRichBlank: true
    function refreshThreadRichBlank() {
        panel.threadRichBlank =
            !app.richComposer
            || app.richComposer.documentIsBlank(threadRichInput.textDocument)
    }

    function activeThreadEditor() {
        return panel.richMode ? threadRichInput : threadComposerInput
    }
    function threadSpellEditorText() {
        return panel.richMode ? threadRichInput.getText(0, threadRichInput.length)
                              : threadComposerInput.text
    }
    function threadSpellSkipRanges() {
        // Rich mode uses document-derived ranges only: mentionRanges index the
        // markdown mirror.
        if (panel.richMode)
            return app.richComposer.spellSkipRanges(threadRichInput.textDocument)
        return app.thread.mentionRanges
    }
    function threadSpellUnderlineRects(editor, ranges) {
        var out = []
        for (var i = 0; i < ranges.length; ++i) {
            var start = ranges[i].start
            var end = start + ranges[i].length
            var p = start
            var guard = 0
            while (p < end && guard++ < 64) {
                var head = editor.positionToRectangle(p)
                var q = end
                var tail = editor.positionToRectangle(q)
                if (tail.y !== head.y) {
                    while (q > p + 1
                           && editor.positionToRectangle(q).y !== head.y)
                        --q
                    tail = editor.positionToRectangle(q)
                }
                var w = tail.x - head.x
                if (w > 0)
                    out.push({ x: head.x,
                               y: head.y + head.height - 2,
                               w: w })
                p = q
            }
        }
        return out
    }

    function refreshThreadSpellUnderlines() {
        if (!panel.threadSpellActive || panel.richMode
            || threadComposerInput.text.length === 0) {
            if (panel.threadSpellUnderlines.length > 0)
                panel.threadSpellUnderlines = []
            return
        }
        var ranges = app.spell.misspelledRanges(
            threadComposerInput.text,
            threadComposerInput.cursorPosition,
            app.thread.mentionRanges)
        panel.threadSpellUnderlines =
            panel.threadSpellUnderlineRects(threadComposerInput, ranges)
    }

    function refreshThreadRichSpellUnderlines() {
        if (!panel.threadSpellActive || !panel.richMode
            || threadRichInput.length === 0) {
            if (panel.threadRichSpellUnderlines.length > 0)
                panel.threadRichSpellUnderlines = []
            return
        }
        var ranges = app.spell.misspelledRanges(panel.threadSpellEditorText(),
                                                threadRichInput.cursorPosition,
                                                panel.threadSpellSkipRanges())
        panel.threadRichSpellUnderlines =
            panel.threadSpellUnderlineRects(threadRichInput, ranges)
    }

    function prepareThreadSpellMenu(mx, my) {
        panel.threadSpellMenuWord = ""
        panel.threadSpellMenuStart = -1
        panel.threadSpellMenuLength = 0
        panel.threadSpellMenuSuggestions = []
        if (!panel.threadSpellActive)
            return
        var editor = panel.activeThreadEditor()
        var text = panel.threadSpellEditorText()
        var hit = app.spell.wordAt(text, editor.positionAt(mx, my))
        if (!hit || hit.word === "")
            return
        var wrong = app.spell.misspelledRanges(text, -1,
                                               panel.threadSpellSkipRanges())
        var rejected = false
        for (var i = 0; i < wrong.length; ++i) {
            if (wrong[i].start === hit.start) {
                rejected = true
                break
            }
        }
        if (!rejected)
            return
        panel.threadSpellMenuWord = hit.word
        panel.threadSpellMenuStart = hit.start
        panel.threadSpellMenuLength = hit.length
        panel.threadSpellMenuSuggestions =
            app.spell.suggestions(hit.word).slice(0, 5)
    }

    function applyThreadSpellSuggestion(replacement) {
        if (panel.threadSpellMenuStart < 0 || replacement === undefined
            || replacement === "")
            return
        var at = panel.threadSpellMenuStart
        if (panel.richMode) {
            app.richComposer.replaceRange(threadRichInput.textDocument, at,
                                          panel.threadSpellMenuLength, replacement)
            threadRichInput.cursorPosition = at + replacement.length
            threadRichInput.forceActiveFocus()
        } else {
            threadComposerInput.remove(at, at + panel.threadSpellMenuLength)
            threadComposerInput.insert(at, replacement)
            threadComposerInput.cursorPosition = at + replacement.length
            threadComposerInput.forceActiveFocus()
        }
    }

    Connections {
        target: app.spell
        function onDictionaryChanged() {
            panel.refreshThreadSpellUnderlines()
            panel.refreshThreadRichSpellUnderlines()
        }
        function onEnabledChanged() {
            panel.refreshThreadSpellUnderlines()
            panel.refreshThreadRichSpellUnderlines()
        }
    }

    function insertThreadMention(userId, displayName) {
        if (panel.richMode) {
            panel.insertThreadRichMention(userId, displayName)
            return
        }
        var newCursor = app.thread.insertMention(
            userId, displayName, panel.threadMentionTokenStart,
            threadComposerInput.cursorPosition)
        if (threadComposerInput.text !== app.thread.text)
            threadComposerInput.text = app.thread.text
        threadComposerInput.cursorPosition = newCursor
        threadMentionPopup.close()
        threadComposerInput.forceActiveFocus()
    }
    Connections {
        target: app.thread
        function onTextChanged() {
            if (threadComposerInput.text !== app.thread.text)
                threadComposerInput.text = app.thread.text
            // Follow a C++-side rewrite unless it is the echo of our own push.
            if (!panel.richMode || panel.richSyncing)
                return
            var current = app.richComposer.toMarkdown(threadRichInput.textDocument)
            if (current.trim() === app.thread.text.trim())
                return
            panel.richSyncing = true
            app.richComposer.loadMarkdown(threadRichInput.textDocument,
                                          app.thread.text)
            panel.richSyncing = false
        }
    }

    // One reaction picker and one profile popover for all thread rows, closed
    // with the thread/room context.
    EmojiPicker {
        id: threadReactionPicker
        mode: "reaction"
        property string targetEventId: ""
        onOpened: {
            replyList.emojiPickerOpen = true
            replyList.claimTransientInteraction("picker")
        }
        onClosed: {
            replyList.emojiPickerOpen = false
            // Release the tone level first: while the tone popup is up the
            // owner is "tone", and releasing only "picker" would leave it
            // latched. Same wiring as TimelinePane.
            replyList.releaseTransientInteraction("tone", "")
            replyList.releaseTransientInteraction("picker", "")
            targetEventId = ""
        }
        // The skin-tone popup owns interaction while up and hands it back to
        // the picker.
        onToneOpened: replyList.claimTransientInteraction("tone")
        onToneClosed: replyList.releaseTransientInteraction("tone", "picker")
        onEmojiChosen: (emoji) => {
            // Through the thread model, never app.composer: the room timeline
            // hides threaded events, so the reaction would be a no-op there.
            // The model passes its composite id, decomposed before the FFI.
            if (targetEventId !== "")
                app.thread.model.toggleReaction(targetEventId, emoji)
        }
    }
    MemberProfilePopover {
        id: threadSenderProfile
        parent: Overlay.overlay
        anchors.centerIn: parent
    }
    Connections {
        target: app
        function onCurrentRoomIdChanged() {
            threadReactionPicker.close()
            threadSenderProfile.close()
            threadMentionPopup.close()
            // One reset point, as closeRowAnchoredSurfaces() in the room
            // timeline.
            replyList.transientInteractionOwner = ""
        }
    }

    // Hidden composer buttons and the merged picker, mirroring
    // MessageComposerBar.qml.
    readonly property var hiddenComposerButtons:
        app.settings ? app.settings.hiddenComposerButtons : []
    function composerButtonShown(key) {
        return panel.hiddenComposerButtons.indexOf(key) < 0
    }
    readonly property bool mediaPickerBothKinds:
        app.gif.available && app.stickers.available
    property string mediaPickerKind: "gif"
    function effectiveMediaKind() {
        if (panel.mediaPickerKind === "sticker" && app.stickers.available)
            return "sticker"
        if (app.gif.available)
            return "gif"
        return app.stickers.available ? "sticker" : "gif"
    }
    function openThreadMediaPicker() {
        if (threadGifPicker.opened || threadStickerPicker.opened) {
            threadGifPicker.close()
            threadStickerPicker.close()
            return
        }
        panel.swapThreadMediaPicker(panel.effectiveMediaKind())
    }
    function swapThreadMediaPicker(kind) {
        panel.mediaPickerKind = kind
        threadEmojiPicker.close()
        if (kind === "sticker") {
            threadGifPicker.close()
            threadStickerPicker.anchorItem = threadMiniComposer
            threadStickerPicker.open()
        } else {
            threadStickerPicker.close()
            threadGifPicker.anchorItem = threadMiniComposer
            threadGifPicker.open()
        }
    }

    GifPicker {
        id: threadGifPicker
        target: "thread"
        offerKindTabs: panel.mediaPickerBothKinds
        onKindRequested: (kind) => panel.swapThreadMediaPicker(kind)
        onGifChosen: (result) => panel.onThreadGifPicked(result)
        onClosed: Qt.callLater(threadComposerInput.forceActiveFocus)
    }

    // Download, validate and send into this thread (captured room + root); the
    // SDK produces a real m.thread reply.
    function onThreadGifPicked(result) {
        app.gifSend.sendToThread(app.thread.roomId, app.thread.rootEventId,
                                 result)
    }

    StickerPicker {
        id: threadStickerPicker
        target: "thread"
        offerKindTabs: panel.mediaPickerBothKinds
        onKindRequested: (kind) => panel.swapThreadMediaPicker(kind)
        onStickerChosen: (image) => panel.onThreadStickerPicked(image)
        onClosed: Qt.callLater(threadComposerInput.forceActiveFocus)
    }

    // Send the chosen pack sticker into this thread. No room-send fallback
    // (CLAUDE.md §8); the SDK attaches the m.thread relation.
    function onThreadStickerPicked(image) {
        app.stickers.sendToThread(app.thread.roomId, app.thread.rootEventId,
                                  image)
    }

    // Markdown formatting in the thread composer. MessageComposer::toggleFormat
    // is a pure text transform, so it serves this text and selection too; do
    // not duplicate the markdown rules.
    function applyThreadFormat(format) {
        if (panel.richMode) {
            panel.applyThreadRichFormat(format)
            return
        }
        if (format === "underline")
            return // no markdown form; a rich-mode key
        var result = app.composer.toggleFormat(format,
                                               threadComposerInput.text,
                                               threadComposerInput.selectionStart,
                                               threadComposerInput.selectionEnd)
        threadComposerInput.text = result.text
        // The two-way sync only fires on change, and the values are already
        // equal.
        app.thread.text = result.text
        threadComposerInput.select(result.selectionStart, result.selectionEnd)
        threadComposerInput.forceActiveFocus()
    }

    // Composer modes, the same two-editor design as MessageComposerBar:
    // app.thread.text is the markdown mirror in both modes, and wire bodies
    // come from app.richComposer.sendDocumentToThread.
    readonly property bool richMode: app.settings
                                     && app.settings.composerMode === "rich"
    property bool richSyncing: false
    // A standing command refusal: the first send is refused and keeps the
    // draft; sending the same text again posts it literally, as the tooltip
    // says.
    property string commandErrorText: ""
    onRichModeChanged: {
        panel.threadSpellUnderlines = []
        panel.threadRichSpellUnderlines = []
        panel.refreshThreadRichBlank()
        // Switching back fires no textChanged; refresh here.
        if (panel.richMode)
            threadRichSpellTimer.restart()
        else
            threadSpellTimer.restart()
        if (panel.richMode) {
            panel.richSyncing = true
            app.richComposer.loadMarkdown(threadRichInput.textDocument,
                                          app.thread.text)
            panel.richSyncing = false
            threadMentionPopup.close()
            Qt.callLater(function () { threadRichInput.forceActiveFocus() })
        } else {
            threadMentionPopup.close()
            Qt.callLater(function () { threadComposerInput.forceActiveFocus() })
        }
    }
    function sendComposerText() {
        if (app.thread.state !== ThreadController.Ready)
            return
        if (panel.richMode) {
            var plain = threadRichInput.getText(0, threadRichInput.length).trim()
            if (plain.length === 0 && !app.thread.hasAttachments)
                return
            if (app.thread.commandError.length > 0
                    && panel.commandErrorText === plain) {
                app.thread.sendTextBypassingCommands(plain)
            } else {
                app.richComposer.sendDocumentToThread(threadRichInput.textDocument)
            }
            panel.commandErrorText = app.thread.commandError.length > 0 ? plain : ""
            if (app.thread.commandError.length === 0) {
                replyList.followLatest = true
                replyList.scrollToEndDeferredIfFollowing()
            }
            return
        }
        var body = threadComposerInput.text.trim()
        if (body.length === 0 && !app.thread.hasAttachments)
            return
        // sendText sends queued attachments, then the text, unless it is a
        // refused slash command.
        if (app.thread.commandError.length > 0 && panel.commandErrorText === body)
            app.thread.sendTextBypassingCommands(body)
        else
            app.thread.sendText(body)
        if (app.thread.commandError.length > 0) {
            panel.commandErrorText = body
            return
        }
        panel.commandErrorText = ""
        threadComposerInput.text = ""
        replyList.followLatest = true
        replyList.scrollToEndDeferredIfFollowing()
    }
    function applyThreadRichFormat(format) {
        var argument = ""
        if (format === "link") {
            var selected = threadRichInput.selectedText
            if (selected.length > 0
                    && app.richComposer.isSafeLinkTarget(selected))
                argument = selected
            else if (!app.richComposer.formatState(
                         threadRichInput.textDocument,
                         threadRichInput.selectionStart,
                         threadRichInput.selectionEnd)["link"])
                return // no link dialog in the panel: select a URL to link it
        }
        app.richComposer.toggleFormat(threadRichInput.textDocument,
                                      threadRichInput.selectionStart,
                                      threadRichInput.selectionEnd, format,
                                      argument)
        threadRichInput.forceActiveFocus()
        // Structure changes what is drawn without changing characters.
        panel.refreshThreadRichBlank()
    }
    function updateThreadRichMentionState() {
        if (!panel.richMode)
            return
        if (!app.thread.active) {
            threadMentionPopup.close()
            return
        }
        var plain = threadRichInput.getText(0, threadRichInput.length)
        var tok = app.thread.mentionTokenAt(plain, threadRichInput.cursorPosition)
        if (tok && tok.active === true) {
            panel.threadMentionTokenStart = tok.start
            app.mentionSuggestions.roomId = app.thread.roomId
            app.mentionSuggestions.query = tok.query
            threadMentionPopup.query = tok.query
            var p = threadRichFlick.mapToItem(Overlay.overlay, 0, 0)
            threadMentionPopup.anchorInputTop = Qt.point(p.x, p.y)
            threadMentionPopup.anchorWidth = threadRichFlick.width
            if (!threadMentionPopup.visible)
                threadMentionPopup.open()
        } else {
            panel.threadMentionTokenStart = -1
            threadMentionPopup.close()
        }
    }
    function escapeHtmlText(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                        .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }
    function insertThreadRichMention(userId, displayName) {
        var start = panel.threadMentionTokenStart
        var end = threadRichInput.cursorPosition
        if (start < 0 || end < start)
            return
        var name = displayName && displayName.length > 0
                   ? displayName : String(userId).substring(1)
        threadRichInput.remove(start, end)
        threadRichInput.insert(start, "<a href=\"https://matrix.to/#/"
                               + encodeURIComponent(userId) + "\">@"
                               + panel.escapeHtmlText(name) + "</a> ")
        threadRichInput.cursorPosition = start + name.length + 2
        threadMentionPopup.close()
        threadRichInput.forceActiveFocus()
    }
}
